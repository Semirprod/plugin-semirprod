#include "WebUiServer.hpp"
#include "SemirprodDock.hpp"
#include "WebUiAsset.hpp"

#include "../Edition.hpp"
#include "../ws/WsFraming.hpp"

#include <QDateTime>
#include <QHostAddress>
#include <QUrl>
#include <QUrlQuery>
#include <QJsonArray>
#include <QRegularExpression>
#include <QStringList>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <utility>

#include <obs-module.h>

namespace semirprod {

namespace {

// Per-session random token. 32 bytes from the system CSPRNG, base64-url so
// it can travel as a URL fragment or subprotocol without escaping drama.
QString makeSessionToken() {
  QByteArray raw(32, '\0');
  QRandomGenerator::system()->fillRange(
      reinterpret_cast<quint32*>(raw.data()),
      raw.size() / static_cast<int>(sizeof(quint32)));
  // base64url without padding: shorter, URL-safe.
  QByteArray b64 = raw.toBase64();
  b64.replace('+', '-');
  b64.replace('/', '_');
  while (b64.endsWith('=')) b64.chop(1);
  return QString::fromLatin1(b64);
}

// Subprotocol expected on every connection: "bearer-<token>". Using the WS
// subprotocol slot (sent during the HTTP upgrade) keeps the token out of
// any URLs that might end up in browser history or referer headers.
QString expectedSubprotocol(const QString& token) {
  return QStringLiteral("bearer-") + token;
}

// Constant-time compare so a local process can't time-probe the session token
// byte by byte. The token length is fixed and not secret, so an early length
// check is fine.
bool constantTimeEquals(const QString& a, const QString& b) {
  const QByteArray ab = a.toUtf8();
  const QByteArray bb = b.toUtf8();
  if (ab.size() != bb.size()) return false;
  quint8 diff = 0;
  for (int i = 0; i < ab.size(); ++i) {
    diff |= static_cast<quint8>(ab[i]) ^ static_cast<quint8>(bb[i]);
  }
  return diff == 0;
}

}  // namespace

QString acceptedAvatarType(const QString& contentType, bool allowSvg) {
  const QString type = contentType.section(QLatin1Char(';'), 0, 0).trimmed().toLower();
  static const QStringList kRaster = {QStringLiteral("image/png"), QStringLiteral("image/jpeg"),
                                      QStringLiteral("image/webp"), QStringLiteral("image/gif")};
  if (kRaster.contains(type)) return type;
  if (allowSvg && type == QStringLiteral("image/svg+xml")) return type;
  return {};
}

// ---------------------------------------------------------------------------
// One authenticated (or in-handshake) browser connection. Performs the RFC 6455
// server-side handshake on a plain QTcpSocket, enforces the bearer subprotocol,
// then frames text messages both ways. Server->client frames are never masked;
// client->server frames are masked and the shared Parser unmasks them.
// ---------------------------------------------------------------------------
class WsServerConnection : public QObject {
  Q_OBJECT
 public:
  // `avatarProvider` rend les octets de l'avatar du compte et leur type MIME.
  // La connexion ne connait pas le dock : elle recoit de quoi repondre a
  // GET /avatar, rien de plus.
  using AvatarProvider = std::function<QByteArray(QString* contentType)>;

  WsServerConnection(QTcpSocket* sock, QString expectedSubprotocol, QString avatarKey,
                     AvatarProvider avatarProvider, QObject* parent)
      : QObject(parent), sock_(sock), expected_(std::move(expectedSubprotocol)),
        avatarKey_(std::move(avatarKey)), avatarProvider_(std::move(avatarProvider)) {
    sock_->setParent(this);
    // Delai de handshake. Une socket qui se connecte et n'envoie jamais son
    // `\r\n\r\n` restait ouverte indefiniment : le plafond de 16 Ko bornait sa
    // memoire, rien ne bornait sa DUREE. N'importe quel processus local pouvait
    // donc accumuler des connexions muettes jusqu'a epuiser les descripteurs.
    // 10 s est enorme pour un aller-retour sur la boucle locale.
    handshakeTimer_.setSingleShot(true);
    handshakeTimer_.setInterval(10000);
    connect(&handshakeTimer_, &QTimer::timeout, this, [this]() {
      if (authed_) return;
      blog(LOG_WARNING,
           "[semirprod-webui] connexion abandonnee : handshake non termine en 10 s");
      sock_->abort();
    });
    handshakeTimer_.start();
    parser_.onText = [this](const QString& m) { emit textMessageReceived(m); };
    parser_.onPing = [this](const QByteArray& p) {
      sock_->write(ws::encodeFrame(ws::OpPong, p, /*mask=*/false));
    };
    parser_.onClose = [this](quint16, const QString&) { sock_->disconnectFromHost(); };
    parser_.onError = [this](const QString&) { sock_->abort(); };
    connect(sock_, &QTcpSocket::readyRead, this, &WsServerConnection::onReadyRead);
    connect(sock_, &QTcpSocket::disconnected, this, [this]() { emit closed(); });
  }

  void sendText(const QString& msg) {
    if (!authed_) return;
    sock_->write(ws::encodeFrame(ws::OpText, msg.toUtf8(), /*mask=*/false));
  }

 signals:
  void authenticated();
  void textMessageReceived(const QString& message);
  void closed();

 private:
  void onReadyRead() {
    const QByteArray in = sock_->readAll();
    if (!authed_) {
      hsBuf_.append(in);
      handleHandshake();
    } else {
      parser_.append(in);
    }
  }

  void handleHandshake() {
    const int end = hsBuf_.indexOf("\r\n\r\n");
    if (end < 0) {
      if (hsBuf_.size() > 16 * 1024) reject(400, QStringLiteral("upgrade header too large"));
      return;
    }
    const QByteArray header = hsBuf_.left(end);
    const QByteArray rest = hsBuf_.mid(end + 4);  // any frame bytes after the header

    QString key;
    QString protoHeader;
    QString originHeader;
    QString hostHeader;
    bool upgrade = false;
    const QList<QByteArray> lines = header.split('\n');
    const QByteArray requestLine = lines.isEmpty() ? QByteArray() : lines[0].trimmed();
    for (int i = 1; i < lines.size(); ++i) {
      const QByteArray line = lines[i].trimmed();
      const int colon = line.indexOf(':');
      if (colon < 0) continue;
      const QByteArray name = line.left(colon).trimmed().toLower();
      const QByteArray value = line.mid(colon + 1).trimmed();
      if (name == "sec-websocket-key") key = QString::fromLatin1(value);
      else if (name == "sec-websocket-protocol") protoHeader = QString::fromLatin1(value);
      else if (name == "origin") originHeader = QString::fromLatin1(value);
      else if (name == "host") hostHeader = QString::fromLatin1(value);
      else if (name == "upgrade" && value.toLower() == "websocket") upgrade = true;
    }

    // Origin / Host : ni l un ni l autre n etait examine.
    //
    // Le port est fixe et previsible (47615) et le GET est servi sans
    // authentification : une page tierce pouvait encadrer le pont dans une
    // iframe invisible, laisser la page s authentifier seule depuis le
    // localStorage, et superposer un leurre au-dessus des controles
    // (clickjacking sur « Deconnecter » ou sur le choix du flux, en plein
    // direct). Un rebinding DNS permettait en plus de charger la page sous une
    // origine attaquante. Une Origin absente reste acceptee : elle caracterise
    // un client non-navigateur, qui doit de toute facon presenter le jeton.
    if (!originAllowed(originHeader) || !hostAllowed(hostHeader)) {
      reject(403, QStringLiteral("origin/host not allowed"));
      return;
    }

    if (!upgrade || key.isEmpty()) {
      // Not a WebSocket upgrade. If it's a plain browser GET, serve the embedded
      // UI shell over HTTP so a browser (or an OBS custom browser dock) pointed
      // at http://127.0.0.1:<port> gets the page. The page's own script then
      // opens a *second* connection to the same port that DOES upgrade to WS,
      // carrying the bearer subprotocol. The served HTML holds no secret — the
      // token is entered in the page's gate (dock "Copier le token web" button),
      // never embedded here — so unauthenticated HTTP on loopback is fine.
      if (requestLine.startsWith("GET ")) {
        // Cible de la requete : « GET /chemin?query HTTP/1.1 ».
        const QByteArray target = requestLine.mid(4).split(' ').value(0);
        if (target.startsWith("/avatar")) {
          serveAvatar(target);
        } else {
          serveHtml();
        }
      } else {
        reject(400, QStringLiteral("not a websocket upgrade"));
      }
      return;
    }

    // The peer may offer several comma-separated subprotocols. Accept only the
    // one that exactly equals our expected bearer-<token>, compared in
    // constant time.
    QString chosen;
    const QList<QString> offered = protoHeader.split(',');
    for (const QString& sp : offered) {
      const QString cand = sp.trimmed();
      if (constantTimeEquals(cand, expected_)) {
        chosen = cand;
        break;
      }
    }
    if (chosen.isEmpty()) {
      reject(403, QStringLiteral("missing or invalid bearer subprotocol"));
      return;
    }

    QString resp;
    resp += QStringLiteral("HTTP/1.1 101 Switching Protocols\r\n");
    resp += QStringLiteral("Upgrade: websocket\r\n");
    resp += QStringLiteral("Connection: Upgrade\r\n");
    resp += QStringLiteral("Sec-WebSocket-Accept: %1\r\n").arg(ws::acceptFor(key));
    resp += QStringLiteral("Sec-WebSocket-Protocol: %1\r\n").arg(chosen);
    resp += QStringLiteral("\r\n");
    sock_->write(resp.toUtf8());

    authed_ = true;
    handshakeTimer_.stop();
    hsBuf_.clear();
    emit authenticated();
    if (!rest.isEmpty()) parser_.append(rest);
  }

  // Une valeur vide est acceptee : elle caracterise un client non-navigateur,
  // qui doit de toute facon presenter le jeton dans le sous-protocole.
  static bool hostIsLoopback(const QString& hostPort) {
    if (hostPort.isEmpty()) return true;
    const QString h = hostPort.section(QLatin1Char(':'), 0, 0).trimmed().toLower();
    return h == QLatin1String("127.0.0.1") || h == QLatin1String("localhost") ||
           h == QLatin1String("[::1]") || h == QLatin1String("::1");
  }

  static bool originAllowed(const QString& origin) {
    if (origin.isEmpty()) return true;
    const QUrl u(origin);
    if (u.scheme() != QLatin1String("http") && u.scheme() != QLatin1String("https")) return false;
    return hostIsLoopback(u.host());
  }

  static bool hostAllowed(const QString& hostHeader) { return hostIsLoopback(hostHeader); }

  void reject(int status, const QString& reason) {
    blog(LOG_WARNING, "[semirprod-webui] rejected client: %s (peer=%s)",
         reason.toUtf8().constData(),
         sock_->peerAddress().toString().toUtf8().constData());
    const char* phrase = (status == 403) ? "Forbidden" : "Bad Request";
    const QString resp =
        QStringLiteral("HTTP/1.1 %1 %2\r\nConnection: close\r\nContent-Length: 0\r\n\r\n")
            .arg(status)
            .arg(QString::fromLatin1(phrase));
    sock_->write(resp.toUtf8());
    sock_->flush();
    sock_->disconnectFromHost();
  }

  // Serve the embedded single-file UI over HTTP/1.1 then close. `no-store` is
  // deliberate: OBS's CEF aggressively caches browser-dock pages, and a stale
  // cached build is exactly what made a fresh UI look like "the old style".
  void serveHtml() {
    const QByteArray body(reinterpret_cast<const char*>(kWebUiHtml),
                          static_cast<int>(kWebUiHtmlLen));
    QByteArray resp;
    resp += "HTTP/1.1 200 OK\r\n";
    resp += "Content-Type: text/html; charset=utf-8\r\n";
    resp += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    resp += "Cache-Control: no-store, must-revalidate\r\n";
    resp += "X-Content-Type-Options: nosniff\r\n";
    // frame-ancestors + X-Frame-Options : la page ne peut plus être encadrée
    // par un site tiers. Sans ça, une page malveillante ouverte pendant un
    // direct pouvait charger le pont dans une iframe invisible — la page
    // s'authentifiait seule depuis le localStorage — et superposer un leurre
    // au-dessus des contrôles (« Déconnecter », choix du flux de destination).
    // `unsafe-inline` est imposé par le bundle single-file de vite-plugin-singlefile.
    // `img-src 'self'` sert UNIQUEMENT /avatar, servi par ce meme pont. Aucun
    // hote distant n'est autorise : l'avatar du compte est rapatrie par le
    // plugin, jamais charge par la page chez un tiers (qui apprendrait
    // autrement l'IP du streamer et l'heure de chaque ouverture).
    resp += "Content-Security-Policy: default-src 'self'; "
            "script-src 'unsafe-inline'; style-src 'unsafe-inline'; "
            "img-src 'self' data: blob:; connect-src ws://127.0.0.1:* ws://localhost:*; "
            "frame-ancestors 'none'; base-uri 'none'; form-action 'none'\r\n";
    resp += "X-Frame-Options: DENY\r\n";
    resp += "Connection: close\r\n";
    resp += "\r\n";
    resp += body;
    sock_->write(resp);
    sock_->flush();
    sock_->disconnectFromHost();
  }

  // Avatar du compte, rapatrie par le dock. 404 s'il n'y en a pas : la page
  // retombe alors sur son degrade.
  //
  // Servi SEULEMENT avec la cle `k` de l'URL (WebUiServer::avatarKey), que la
  // page recoit dans l'etat, donc apres authentification. Sans elle, n'importe
  // quel site ouvert dans le navigateur pouvait poser
  // `<img src="http://127.0.0.1:47615/avatar">` : une balise <img> n'envoie pas
  // d'Origin, la requete passait, et le chargement reussi disait au site que le
  // plugin tournait ET qu'un compte y etait lie. Cle absente ou fausse : meme
  // 404 que « pas d'avatar », rien a distinguer.
  //
  // Le `nosniff` + le type MIME verifie a la source (image/*) evitent qu'un
  // contenu inattendu soit interprete autrement que comme une image.
  void serveAvatar(const QByteArray& target) {
    const int q = target.indexOf('?');
    const QUrlQuery query(q < 0 ? QString() : QString::fromLatin1(target.mid(q + 1)));
    const QString key = query.queryItemValue(QStringLiteral("k"));
    const bool keyOk = !avatarKey_.isEmpty() && constantTimeEquals(key, avatarKey_);

    QString contentType;
    const QByteArray body =
        (keyOk && avatarProvider_) ? avatarProvider_(&contentType) : QByteArray();
    // Second verrou apres celui de la reception : un type hors liste n'est pas
    // servi du tout, plutot que servi sous un type qu'on n'a pas verifie.
    contentType = acceptedAvatarType(contentType, /*allowSvg=*/true);
    if (body.isEmpty() || contentType.isEmpty()) {
      const QString resp = QStringLiteral(
          "HTTP/1.1 404 Not Found\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
      sock_->write(resp.toUtf8());
      sock_->flush();
      sock_->disconnectFromHost();
      return;
    }
    QByteArray resp;
    resp += "HTTP/1.1 200 OK\r\n";
    resp += "Content-Type: " + contentType.toLatin1() + "\r\n";
    resp += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    // Immuable : l'URL porte une empreinte du contenu, un avatar different a
    // une autre URL. Evite un aller-retour a chaque rendu.
    resp += "Cache-Control: private, max-age=86400, immutable\r\n";
    resp += "X-Content-Type-Options: nosniff\r\n";
    // Une image ouverte DIRECTEMENT (et non par une balise <img>) devient un
    // document de l'origine du pont, celle qui detient le jeton. Aucune
    // ressource, aucun script, document isole : un SVG piege n'y peut rien.
    resp += "Content-Security-Policy: default-src 'none'; sandbox\r\n";
    resp += "Connection: close\r\n";
    resp += "\r\n";
    resp += body;
    sock_->write(resp);
    sock_->flush();
    sock_->disconnectFromHost();
  }

  QTcpSocket* sock_;
  QString expected_;
  QString avatarKey_;
  AvatarProvider avatarProvider_;
  QByteArray hsBuf_;
  bool authed_ = false;
  QTimer handshakeTimer_;
  ws::Parser parser_;
};

WebUiServer::WebUiServer(SemirprodDock* dock, QObject* parent)
    : QObject(parent), server_(new QTcpServer(this)), dock_(dock) {
  connect(server_, &QTcpServer::newConnection, this, &WebUiServer::onNewConnection);
}

WebUiServer::~WebUiServer() {
  if (server_) server_->close();
  // Une connexion n'est jamais dans les deux ensembles a la fois : l'entree
  // dans `clients_` s'accompagne du retrait de `pending_`.
  qDeleteAll(clients_);
  clients_.clear();
  qDeleteAll(pending_);
  pending_.clear();
}

bool WebUiServer::start(uint16_t firstPort) {
  sessionToken_ = makeSessionToken();
  avatarKey_ = makeSessionToken();

  // UN SEUL port, volontairement — et donc une seule instance d'OBS.
  //
  // Un repli sur le port suivant a ete ecrit puis retire sciemment : il
  // faisait demarrer une seconde instance EN SILENCE, alors que `instanceId`
  // est persiste par installation et non par processus (Settings.cpp). Les
  // deux processus partageaient donc la meme identite cote backend, recevaient
  // tous les deux chaque commande, et basculaient de scene ensemble. Un
  // comportement faux qui ne se voit pas est pire qu'un refus qui se lit.
  //
  // L'echec ici est donc la bonne reponse tant que le produit est
  // mono-instance. Le jour ou deux OBS devront cohabiter, il faudra les DEUX :
  // la plage de ports ET une identite par processus.
  port_ = 0;
  if (!server_->listen(QHostAddress::LocalHost, firstPort)) {
    blog(LOG_ERROR,
         "[semirprod-webui] port %d deja pris — pont local indisponible. "
         "Semirprod ne gere qu'une instance d'OBS a la fois ; une autre est "
         "probablement deja lancee.",
         static_cast<int>(firstPort));
    sessionToken_.clear();
    avatarKey_.clear();
    return false;
  }
  port_ = firstPort;
  // NEVER log the token: users upload their full OBS logs verbatim for
  // support (Discord, obsproject.com/logs), so anything printed here must be
  // safe to publish. The dock's "Copier le token web" button hands the token
  // to the clipboard instead.
  blog(LOG_INFO,
       "[semirprod-webui] listening on http://127.0.0.1:%d/ (serves the UI) "
       "and ws:// on the same port — token via the dock's copy button "
       "(rotates every OBS restart)",
       static_cast<int>(port_));
  return true;
}

void WebUiServer::onNewConnection() {
  while (server_->hasPendingConnections()) {
    QTcpSocket* sock = server_->nextPendingConnection();
    if (!sock) break;

    // Plafond du nombre de connexions vivantes, authentifiees ou non.
    //
    // Rien ne bornait ce nombre : chaque socket acceptee construisait un
    // WsServerConnection, et un processus local pouvait en ouvrir autant qu'il
    // voulait. 32 est tres au-dessus de l'usage reel — l'interface en ouvre
    // une, deux le temps d'un rechargement — et tres en-dessous de ce qui
    // gene OBS. On ferme la socket excedentaire sans rien construire.
    static constexpr int kMaxConnections = 32;
    if (clients_.size() + pending_.size() >= kMaxConnections) {
      blog(LOG_WARNING,
           "[semirprod-webui] connexion refusee : %d connexions deja ouvertes",
           kMaxConnections);
      sock->abort();
      sock->deleteLater();
      continue;
    }

    auto* conn = new WsServerConnection(
        sock, expectedSubprotocol(sessionToken_), avatarKey_,
        [this](QString* contentType) -> QByteArray {
          if (!dock_) return {};
          if (contentType) *contentType = dock_->accountAvatarType();
          return dock_->accountAvatarBytes();
        },
        this);
    pending_.insert(conn);
    connect(conn, &WsServerConnection::authenticated, this, [this, conn]() {
      pending_.remove(conn);
      clients_.insert(conn);

      // L'ETAT PART TOUT DE SUITE, sans attendre que la page le demande.
      //
      // La page ouvre son WebSocket puis envoie `get_state`, et n'affiche
      // l'interface qu'a la reponse. Entre les deux elle rend son ecran
      // « connexion » — un aller-retour complet de battement, visible a chaque
      // ouverture depuis que le Studio est une fenetre qui recharge sa page.
      //
      // On pousse donc l'etat des l'instant ou la liaison est authentifiee.
      // C'est sans risque de le manquer : la page a cree le socket elle-meme,
      // donc son gestionnaire de messages existe avant que celui-ci ne
      // s'ouvre, et le navigateur met de toute facon les messages en file.
      //
      // Ca ne supprime pas tout le battement — le premier rendu de la page,
      // avant le moindre message, ne nous appartient pas — mais ca en retire
      // l'aller-retour, qui en est la seule part evitable d'ici.
      if (dock_) dock_->broadcastToWeb();
    });
    connect(conn, &WsServerConnection::textMessageReceived, this,
            &WebUiServer::onMessageReceived);
    connect(conn, &WsServerConnection::closed, this, &WebUiServer::onClientClosed);
  }
}

bool WebUiServer::allowCommand(WsServerConnection* conn) {
  // Large devant l'usage humain (un ecran de reglages envoie une poignee de
  // commandes par salve), etroit devant une boucle : au-dela, la page est
  // buggee ou hostile, et la jeter est le comportement souhaitable.
  static constexpr qint64 kWindowMs = 2000;
  static constexpr int kMaxPerWindow = 20;

  CommandBudget& budget = budgets_[conn];
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  if (now - budget.windowStartMs >= kWindowMs) {
    budget.windowStartMs = now;
    budget.count = 0;
    budget.overflowLogged = false;
  }
  if (++budget.count > kMaxPerWindow) {
    // Un seul log par fenetre : journaliser chaque commande jetee offrirait a
    // l'attaquant un moyen de remplir le log d'OBS.
    if (!budget.overflowLogged) {
      budget.overflowLogged = true;
      blog(LOG_WARNING,
           "[semirprod-webui] client au-dela de %d commandes / %lld ms, "
           "commandes ignorees",
           kMaxPerWindow, static_cast<long long>(kWindowMs));
    }
    return false;
  }
  return true;
}

void WebUiServer::onMessageReceived(const QString& message) {
  auto* conn = qobject_cast<WsServerConnection*>(sender());
  if (!conn || !clients_.contains(conn)) return;
  if (!allowCommand(conn)) return;

  const QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8());
  if (!doc.isObject()) return;

  const QJsonObject obj = doc.object();
  const QString type = obj["type"].toString();

  if (type == "get_state") {
    dock_->broadcastToWeb();
  } else if (type == "close_studio") {
    dock_->closeStudioWindow();
  } else if (type == "connect" || type == "disconnect") {
    // ─── DANS UNE INSTANCE INFOGEREE, LE COMPTE NE SE TOUCHE PAS ───────────
    //
    // Le jeton d'une instance vient de son appairage au demarrage, pas d'une
    // connexion faite par le client. Le laisser se deconnecter le couperait de
    // SON PROPRE OBS, sans moyen de se rappairer depuis l'interieur.
    //
    // Le bouton est deja masque dans la page (etat « kiosk »). Ce refus-ci est
    // le second verrou : la page est servie en local et rien n'empeche de lui
    // envoyer la commande a la main.
    if (!edition::allowsAccountCommands()) {
      blog(LOG_INFO, "[semirprod-webui] commande « %s » refusee : instance infogeree",
           qUtf8Printable(type));
    } else if (type == "connect") {
      dock_->startAuth();
    } else {
      dock_->disconnectAccount();
    }
  } else if (type == "refresh") {
    // Plus aucune UI n'emet cette commande : l'acces Studio arrive par push
    // serveur (`check_studio_access`) et la liste des scenes par
    // OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED. Le handler reste pour une page
    // servie par un ancien binaire, et refreshAll() est desormais throttle.
    dock_->refreshAll();
  } else if (type == "select_stream") {
    dock_->selectStream(obj["id"].toString());
  } else if (type == "set_config") {
    dock_->setSceneConfig(obj["live"].toString(), obj["bug"].toString(),
                          obj["autoSwitch"].toBool());
  } else if (type == "add_stream_source") {
    // La page envoie un IDENTIFIANT de flux, jamais un nom de source ni de
    // scene : le dock refuse tout identifiant absent de la liste du compte, et
    // la scene visee est celle ouverte dans OBS.
    dock_->placeStreamSourceInCurrentScene(obj["id"].toString());
  } else if (type == "place_source_in_live_scene") {
    // Aucun parametre : la scene visee est celle deja configuree cote plugin.
    // La page ne nomme ni scene ni source — elle demande la reparation, le
    // plugin decide de son contenu.
    dock_->placeSourceInLiveScene();
  } else if (type == "set_preview_enabled") {
    dock_->setPreviewEnabled(obj["on"].toBool(true));
  } else if (type == "dismiss_remote_notice") {
    dock_->dismissRemoteDestinationNotice();
  } else if (type == "set_instance_label") {
    dock_->setInstanceLabel(obj["label"].toString());
#ifdef SEMIRPROD_COLLECTIONS
  } else if (type == "backup_collection") {
    // La page envoie des NOMS de collections, jamais un chemin, et le plugin
    // refuse tout nom qu'OBS ne declare pas (CollectionPackager). Sans cette
    // regle, une commande venue du pont local ferait lire, empaqueter et monter
    // n'importe quel fichier de la machine.
    //
    // Tableau absent ou vide = la collection active, le geste par defaut.
    QStringList collections;
    const QJsonArray requested = obj["collections"].toArray();
    for (const QJsonValue& item : requested) {
      const QString name = item.toString();
      // Borne de longueur ici aussi : le nom traverse un journal et une requete
      // HTTP, et rien n'oblige la page a etre honnete sur sa taille.
      if (!name.isEmpty() && name.size() <= 200) collections.append(name);
    }
    dock_->backupCollections(collections);
  } else if (type == "refresh_backups") {
    dock_->refreshBackups();
  } else if (type == "import_backup") {
    // Un identifiant de sauvegarde, borne a la forme d'un UUID : il finit dans
    // une URL. Les dossiers d'ecriture, eux, sont choisis par le plugin — la
    // page ne nomme aucun chemin.
    const QString id = obj["id"].toString();
    static const QRegularExpression uuid(
        QStringLiteral("^[0-9a-fA-F-]{36}$"));
    if (uuid.match(id).hasMatch()) dock_->importBackup(id);
  } else if (type == "open_imported_collection") {
    // Aucun parametre : la collection ouverte est celle que le dernier import
    // a installee, la page ne la nomme pas.
    dock_->openImportedCollection();
#endif  // SEMIRPROD_COLLECTIONS
  } else if (type == "run_conn_test") {
    dock_->runConnectionTest();
  } else if (type == "open_subscribe") {
    // Sans URL en parametre : c'est Config qui la decide. Voir
    // SemirprodDock::openSubscribePage().
    dock_->openSubscribePage();
  } else if (type == "open_feature_action") {
    // Le NOM d'une fonction, jamais une URL : le geste et son chemin viennent de
    // la matrice resolue par le serveur, et l'hote est celui du plugin. Voir
    // SemirprodDock::openFeatureAction().
    const QString feature = obj["feature"].toString();
    if (!feature.isEmpty() && feature.size() <= 40) dock_->openFeatureAction(feature);
  } else if (type == "open_update") {
    // Idem : l'URL vient du manifeste signe, pas de la page.
    dock_->openUpdateDownload();
  }
}

void WebUiServer::onClientClosed() {
  auto* conn = qobject_cast<WsServerConnection*>(sender());
  if (!conn) return;
  clients_.remove(conn);
  // Une connexion jetee AVANT de s'authentifier (delai de handshake, origine
  // refusee, simple GET du HTML) passe aussi par ici : sans ce retrait, elle
  // resterait comptee dans le plafond pour toujours.
  pending_.remove(conn);
  budgets_.remove(conn);
  conn->deleteLater();
}

void WebUiServer::broadcastState(const QJsonObject& state) {
  QJsonObject msg;
  msg["type"] = "state";
  msg["data"] = state;
  const QString json = QString::fromUtf8(QJsonDocument(msg).toJson(QJsonDocument::Compact));
  for (WsServerConnection* client : clients_) client->sendText(json);
}

}  // namespace semirprod

#include "WebUiServer.moc"
