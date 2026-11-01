#pragma once

// ---------------------------------------------------------------------------
// Drop-in replacement for the subset of QWebSocket that LiveClient relies on,
// implemented over QSslSocket (QtNetwork) so the plugin no longer links
// Qt6::WebSockets. See WsFraming.hpp for the full rationale — in short, the
// bundled Qt6WebSockets.dll pinned the plugin to one exact Qt version and broke
// on every OBS Qt bump; QtNetwork is shipped by OBS and stays binary-compatible
// within Qt6.
//
// Only the client role is implemented here (wss:// to the Semirprod live-ws
// bridge). TLS, peer verification and the sslErrors/ignoreSslErrors hooks are
// delegated straight to the underlying QSslSocket (standard TLS validation; no
// certificate pinning is active, see Config.hpp).
// ---------------------------------------------------------------------------

#include <QAbstractSocket>
#include <QByteArray>
#include <QList>
#include <QObject>
#include <QSslError>
#include <QString>

#include "WsFraming.hpp"

class QSslSocket;
class QNetworkRequest;
class QSslConfiguration;

namespace semirprod {

class WebSocketClient : public QObject {
  Q_OBJECT

 public:
  explicit WebSocketClient(QObject* parent = nullptr);
  ~WebSocketClient() override;

  // Opens a wss:// (or ws://) connection. The request's URL and raw headers
  // (Authorization, User-Agent, …) are carried into the HTTP Upgrade. A 3xx or
  // any non-101 response is treated as an error — we never follow redirects,
  // which preserves LiveClient's bearer-token redirect hardening for free.
  void open(const QNetworkRequest& request);

  // Graceful RFC 6455 close (sends a close frame then tears down the socket).
  void close();
  // Immediate teardown — drops TCP without a close handshake.
  void abort();

  qint64 sendTextMessage(const QString& message);
  // Octets encore en attente d'envoi (clair + chiffré). Sert de contre-pression :
  // un message jetable n'est pas mis en file derrière un envoi qui sature.
  qint64 pendingBytes() const;
  qint64 ping(const QByteArray& payload = QByteArray());

  QAbstractSocket::SocketState state() const;
  QString errorString() const;

  quint16 closeCode() const { return closeCode_; }
  QString closeReason() const { return closeReason_; }

  QSslConfiguration sslConfiguration() const;
  void ignoreSslErrors(const QList<QSslError>& errors);

 signals:
  void connected();
  void disconnected();
  void textMessageReceived(const QString& message);
  // elapsedTime is always 0 (we don't measure RTT); kept for QWebSocket parity.
  void pong(quint64 elapsedTime, const QByteArray& payload);
  void errorOccurred(QAbstractSocket::SocketError error);
  void sslErrors(const QList<QSslError>& errors);

 private:
  void onTransportConnected();      // TLS established (or TCP for ws://)
  void onReadyRead();
  void onSocketDisconnected();
  bool consumeHandshakeResponse();  // returns true once the 101 is validated
  void sendClose(quint16 code, const QString& reason);
  void fail(const QString& message, QAbstractSocket::SocketError err);

  QSslSocket* socket_;
  ws::Parser parser_;

  enum class Phase { Idle, Connecting, Open, Closing };
  Phase phase_ = Phase::Idle;

  bool secure_ = true;
  QString host_;
  QString requestLine_;   // full HTTP upgrade request bytes to send on connect
  QString expectedAccept_;
  QByteArray handshakeBuf_;

  quint16 closeCode_ = 1006;  // abnormal closure until a close frame says otherwise
  QString closeReason_;
  QString lastError_;
};

}  // namespace semirprod
