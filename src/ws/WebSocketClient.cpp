#include "WebSocketClient.hpp"

#include <QNetworkRequest>
#include <QSet>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QUrl>

#include <obs-module.h>

namespace semirprod {

WebSocketClient::WebSocketClient(QObject* parent)
    : QObject(parent), socket_(new QSslSocket(this)) {
  parser_.onText = [this](const QString& msg) { emit textMessageReceived(msg); };
  parser_.onPing = [this](const QByteArray& payload) {
    // Answer every ping with a pong carrying the same application data.
    if (phase_ == Phase::Open)
      socket_->write(ws::encodeFrame(ws::OpPong, payload, /*mask=*/true));
  };
  parser_.onPong = [this](const QByteArray& payload) { emit pong(0, payload); };
  parser_.onClose = [this](quint16 code, const QString& reason) {
    closeCode_ = code;
    closeReason_ = reason;
    // Echo the close and let the socket tear down (disconnected drives reconnect).
    sendClose(code, QString());
    socket_->disconnectFromHost();
  };
  parser_.onError = [this](const QString& why) {
    fail(QStringLiteral("protocol error: %1").arg(why),
         QAbstractSocket::UnknownSocketError);
  };

  connect(socket_, &QSslSocket::encrypted, this, &WebSocketClient::onTransportConnected);
  // ws:// (non-TLS) path — Config forbids it in release, but keep it correct.
  connect(socket_, &QSslSocket::connected, this, [this]() {
    if (!secure_) onTransportConnected();
  });
  connect(socket_, &QSslSocket::readyRead, this, &WebSocketClient::onReadyRead);
  connect(socket_, &QSslSocket::disconnected, this, &WebSocketClient::onSocketDisconnected);
  connect(socket_, &QAbstractSocket::errorOccurred, this,
          [this](QAbstractSocket::SocketError e) {
            lastError_ = socket_->errorString();
            emit errorOccurred(e);
          });
  // Remonte les erreurs TLS a LiveClient, qui les journalise (aucune n'est
  // ignoree : validation standard, pas d'epinglage).
  connect(socket_, &QSslSocket::sslErrors, this,
          [this](const QList<QSslError>& errors) { emit sslErrors(errors); });
}

WebSocketClient::~WebSocketClient() = default;

void WebSocketClient::open(const QNetworkRequest& request) {
  // Un open() sur un transport encore vivant repassait phase_ à Connecting
  // sans fermer la connexion TLS : les trames suivantes étaient lues comme une
  // réponse de poignée de main et la liaison restait cassée, « connectée » en
  // apparence. On repart toujours d'un transport fermé.
  if (phase_ != Phase::Idle || socket_->state() != QAbstractSocket::UnconnectedState) {
    blog(LOG_WARNING, "[semirprod-ws] open() sur un transport ouvert : fermeture d'abord");
    // Idle AVANT abort() : onSocketDisconnected() s'arrête alors tout de suite
    // et n'émet pas disconnected(), qui déclencherait une reconnexion en plus
    // de celle qu'on est en train de faire.
    phase_ = Phase::Idle;
    socket_->abort();
    parser_.reset();
  }
  const QUrl url = request.url();
  secure_ = url.scheme().compare(QStringLiteral("wss"), Qt::CaseInsensitive) == 0;
  host_ = url.host();
  const int defaultPort = secure_ ? 443 : 80;
  const int port = url.port(defaultPort);

  QString path = url.path(QUrl::FullyEncoded);
  if (path.isEmpty()) path = QStringLiteral("/");
  const QString query = url.query(QUrl::FullyEncoded);
  if (!query.isEmpty()) path += QStringLiteral("?") + query;

  QString hostHeader = host_;
  if (port != defaultPort) hostHeader += QStringLiteral(":%1").arg(port);

  const QString key = ws::makeClientKey();
  expectedAccept_ = ws::acceptFor(key);

  // Build the HTTP/1.1 Upgrade request. We set the mandatory WS headers and
  // carry over any caller-supplied raw headers (Authorization, User-Agent),
  // skipping ones we own so a caller can't corrupt the handshake.
  QString req;
  req += QStringLiteral("GET %1 HTTP/1.1\r\n").arg(path);
  req += QStringLiteral("Host: %1\r\n").arg(hostHeader);
  req += QStringLiteral("Upgrade: websocket\r\n");
  req += QStringLiteral("Connection: Upgrade\r\n");
  req += QStringLiteral("Sec-WebSocket-Key: %1\r\n").arg(key);
  req += QStringLiteral("Sec-WebSocket-Version: 13\r\n");
  const auto owned = QSet<QByteArray>{"host", "upgrade", "connection",
                                      "sec-websocket-key", "sec-websocket-version",
                                      "sec-websocket-protocol", "sec-websocket-extensions"};
  for (const QByteArray& name : request.rawHeaderList()) {
    if (owned.contains(name.toLower())) continue;
    req += QString::fromLatin1(name) + QStringLiteral(": ") +
           QString::fromLatin1(request.rawHeader(name)) + QStringLiteral("\r\n");
  }
  req += QStringLiteral("\r\n");
  requestLine_ = req;

  parser_.reset();
  handshakeBuf_.clear();
  closeCode_ = 1006;
  closeReason_.clear();
  phase_ = Phase::Connecting;

  if (secure_) {
    socket_->connectToHostEncrypted(host_, static_cast<quint16>(port));
  } else {
    socket_->connectToHost(host_, static_cast<quint16>(port));
  }
}

void WebSocketClient::onTransportConnected() {
  if (phase_ != Phase::Connecting) return;
  // Nagle retient les petits messages (résultats de commande, battements) en
  // attendant l'accusé de réception du précédent : jusqu'à ~200 ms de retard
  // avec l'accusé différé du serveur. Ici chaque message est urgent.
  socket_->setSocketOption(QAbstractSocket::LowDelayOption, 1);
  socket_->write(requestLine_.toUtf8());
}

void WebSocketClient::onReadyRead() {
  const QByteArray incoming = socket_->readAll();
  if (phase_ == Phase::Connecting) {
    handshakeBuf_.append(incoming);
    if (!consumeHandshakeResponse()) return;  // need more, or failed
    // consumeHandshakeResponse() moved us to Open and stashed any trailing
    // bytes into handshakeBuf_; feed those to the frame parser.
    const QByteArray leftover = handshakeBuf_;
    handshakeBuf_.clear();
    if (!leftover.isEmpty()) parser_.append(leftover);
  } else if (phase_ == Phase::Open || phase_ == Phase::Closing) {
    parser_.append(incoming);
  }
}

bool WebSocketClient::consumeHandshakeResponse() {
  const int end = handshakeBuf_.indexOf("\r\n\r\n");
  if (end < 0) {
    // Bound the pre-handshake buffer so a rogue server can't grow it forever.
    if (handshakeBuf_.size() > 64 * 1024) {
      fail(QStringLiteral("handshake response too large"),
           QAbstractSocket::UnknownSocketError);
    }
    return false;
  }

  const QByteArray header = handshakeBuf_.left(end);
  handshakeBuf_.remove(0, end + 4);  // keep any frame bytes that followed

  const QList<QByteArray> lines = header.split('\n');
  if (lines.isEmpty()) {
    fail(QStringLiteral("empty handshake response"), QAbstractSocket::UnknownSocketError);
    return false;
  }
  const QByteArray statusLine = lines.first().trimmed();
  if (!statusLine.contains(" 101")) {
    fail(QStringLiteral("handshake not upgraded (status: %1)")
             .arg(QString::fromLatin1(statusLine)),
         QAbstractSocket::UnknownSocketError);
    return false;
  }

  QString accept;
  bool upgradeOk = false;
  for (int i = 1; i < lines.size(); ++i) {
    const QByteArray line = lines[i].trimmed();
    const int colon = line.indexOf(':');
    if (colon < 0) continue;
    const QByteArray name = line.left(colon).trimmed().toLower();
    const QByteArray value = line.mid(colon + 1).trimmed();
    if (name == "sec-websocket-accept") accept = QString::fromLatin1(value);
    else if (name == "upgrade" && value.toLower() == "websocket") upgradeOk = true;
  }

  if (!upgradeOk || accept != expectedAccept_) {
    fail(QStringLiteral("handshake validation failed (accept mismatch)"),
         QAbstractSocket::SslHandshakeFailedError);
    return false;
  }

  phase_ = Phase::Open;
  emit connected();
  return true;
}

void WebSocketClient::onSocketDisconnected() {
  if (phase_ == Phase::Idle) return;  // already torn down (avoid double emit)
  phase_ = Phase::Idle;
  parser_.reset();
  emit disconnected();
}

qint64 WebSocketClient::sendTextMessage(const QString& message) {
  if (phase_ != Phase::Open) return -1;
  return socket_->write(ws::encodeFrame(ws::OpText, message.toUtf8(), /*mask=*/true));
}

qint64 WebSocketClient::pendingBytes() const {
  return socket_->bytesToWrite() + socket_->encryptedBytesToWrite();
}

qint64 WebSocketClient::ping(const QByteArray& payload) {
  if (phase_ != Phase::Open) return -1;
  return socket_->write(ws::encodeFrame(ws::OpPing, payload, /*mask=*/true));
}

void WebSocketClient::sendClose(quint16 code, const QString& reason) {
  if (socket_->state() != QAbstractSocket::ConnectedState) return;
  QByteArray body;
  body.append(static_cast<char>((code >> 8) & 0xFF));
  body.append(static_cast<char>(code & 0xFF));
  body.append(reason.toUtf8());
  socket_->write(ws::encodeFrame(ws::OpClose, body, /*mask=*/true));
  socket_->flush();
}

void WebSocketClient::close() {
  if (phase_ == Phase::Open) {
    phase_ = Phase::Closing;
    sendClose(1000, QString());
    socket_->disconnectFromHost();
  } else {
    socket_->abort();
    if (phase_ != Phase::Idle) onSocketDisconnected();
  }
}

void WebSocketClient::abort() {
  socket_->abort();
  // QAbstractSocket::abort() only emits disconnected() if it was connected;
  // normalise so callers relying on a disconnected() edge (forceReconnect) get
  // one regardless of the exact prior state.
  if (phase_ != Phase::Idle && socket_->state() == QAbstractSocket::UnconnectedState) {
    onSocketDisconnected();
  }
}

QAbstractSocket::SocketState WebSocketClient::state() const {
  switch (phase_) {
    case Phase::Idle:
      return socket_->state();  // usually UnconnectedState
    case Phase::Connecting:
      // Report ConnectingState throughout TLS + HTTP upgrade, matching
      // QWebSocket so LiveClient's connect watchdog behaves identically.
      return QAbstractSocket::ConnectingState;
    case Phase::Open:
      return QAbstractSocket::ConnectedState;
    case Phase::Closing:
      return QAbstractSocket::ClosingState;
  }
  return QAbstractSocket::UnconnectedState;
}

QString WebSocketClient::errorString() const {
  return lastError_.isEmpty() ? socket_->errorString() : lastError_;
}

QSslConfiguration WebSocketClient::sslConfiguration() const {
  return socket_->sslConfiguration();
}

void WebSocketClient::ignoreSslErrors(const QList<QSslError>& errors) {
  socket_->ignoreSslErrors(errors);
}

void WebSocketClient::fail(const QString& message, QAbstractSocket::SocketError err) {
  lastError_ = message;
  blog(LOG_WARNING, "[semirprod-ws] %s", message.toUtf8().constData());
  emit errorOccurred(err);
  socket_->abort();
}

}  // namespace semirprod
