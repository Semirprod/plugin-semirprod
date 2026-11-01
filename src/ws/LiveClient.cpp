#include "LiveClient.hpp"

#include "../Config.hpp"
#include "../SecretKeys.hpp"
#include "../Settings.hpp"
#include "../obs/ProgramPreviewOutput.hpp"
#include "../obs/VerticalBridge.hpp"
#include "../obs/MultistreamBridge.hpp"
#include "WebSocketClient.hpp"

#include <algorithm>
#include <functional>

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslError>
#include <QSslKey>
#include <QSslSocket>
#include <QTimer>
#include <QUrl>

#include <obs-module.h>

namespace semirprod {

namespace {

// Emplacements reserves (voir Config.hpp) : AUCUN epinglage n'est actif, la
// connexion repose sur la validation TLS standard. Liste jamais consultee tant
// que kPinningArmed vaut false.
const QStringList kPinnedSpkis = {
    QString::fromLatin1(Config::kLiveWsSpkiPinPrimary),
    QString::fromLatin1(Config::kLiveWsSpkiPinSecondary),
};


// Remplace la valeur des cles sensibles par <redacted> avant journalisation ou
// affichage. Applique au trafic entrant ET sortant : rien de ce qui transite
// sur ce canal ne doit atterrir dans le log OBS (publie tel quel au support)
// ni dans un widget visible pendant un partage d ecran.
//
// Une cle est sensible si son nom CONTIENT (casse ignoree) l'un des fragments
// ci-dessous : la liste de noms exacts laissait passer `stream_key`,
// `srtPassphrase`, `apiKey`, `Authorization`… Sous une cle sensible, la valeur
// est masquee EN ENTIER, quel que soit son type (objet, tableau, nombre).
// Quelques noms courants contiennent un fragment sans rien porter de secret
// (`keyframeInterval`, `hotkeys`, `author`) : ils sont exclus explicitement.
// Regle partagee avec le nettoyage des sauvegardes : src/SecretKeys.hpp.
bool isSensitiveKey(const QString& key) { return isSecretLookingKey(key); }

// Longueur maximale d'une valeur texte passee au journal : au-dela, coupee
// AVANT tout traitement (le journal n'en a pas besoin, et une valeur de
// plusieurs megaoctets ne doit pas couter cher a chaque trame).
constexpr int kMaxLoggedValue = 4096;

// Segment de chemin qui ressemble a un jeton : 16 caracteres ou plus de
// [A-Za-z0-9_.-] (`https://hote/<jeton>`, overlays a un seul segment).
bool looksLikeToken(const QString& segment) {
  if (segment.size() < 16) return false;
  for (const QChar c : segment) {
    if (!c.isLetterOrNumber() && c != QLatin1Char('_') && c != QLatin1Char('-') &&
        c != QLatin1Char('.')) {
      return false;
    }
  }
  return true;
}

// UNE URL nettoyee : sans identifiants (`user:mdp@`), sans query
// (`?passphrase=` d'un srt://, `?token=` d'un WHIP SRS), sans fragment, et
// sans les segments de chemin au-dela du premier, ou les plateformes mettent
// la cle (`rtmp://hote/app/<cle>`) et les services leurs jetons
// (`https://hote/overlay/<jeton>`). Illisible : masquee en entier.
QString scrubOneUrl(const QString& text) {
  QUrl url(text);
  if (!url.isValid() || url.host().isEmpty()) return QStringLiteral("<redacted-url>");
  url.setUserInfo(QString());
  url.setQuery(QString());
  url.setFragment(QString());
  const QStringList segments = url.path().split(QLatin1Char('/'), Qt::SkipEmptyParts);
  if (!segments.isEmpty()) {
    // Le premier segment reste (il dit quel service), sauf s'il a l'allure
    // d'un jeton ; tout ce qui suit est masque.
    const QString premier =
        looksLikeToken(segments.first()) ? QStringLiteral("redacted") : segments.first();
    url.setPath(QLatin1Char('/') + premier +
                (segments.size() > 1 ? QStringLiteral("/redacted") : QString()));
  }
  return url.toString();
}

// Toute URL trouvee DANS un texte (message d'erreur, phrase libre) est
// nettoyee par scrubOneUrl ; le reste du texte est inchange.
//
// Parcours LINEAIRE, sans expression reguliere : une regex non ancree
// (`[A-Za-z][A-Za-z0-9+.-]*://`) repartait de chaque position d'une longue
// suite de lettres sans `://`, cout quadratique sur une trame fabriquee. Ici
// on cherche chaque `://`, on remonte au plus 32 caracteres pour le schema, et
// on avance jusqu'au premier blanc, guillemet ou chevron.
QString scrubUrlText(const QString& text) {
  const auto estSchema = [](QChar c) {
    return c.isLetterOrNumber() || c == QLatin1Char('+') || c == QLatin1Char('.') ||
           c == QLatin1Char('-');
  };
  const auto finUrl = [](QChar c) {
    return c.isSpace() || c == QLatin1Char('"') || c == QLatin1Char('\'') ||
           c == QLatin1Char('<') || c == QLatin1Char('>');
  };
  QString sortie;
  qsizetype curseur = 0;
  qsizetype pos = text.indexOf(QStringLiteral("://"));
  while (pos >= 0) {
    qsizetype debut = pos;
    while (debut > curseur && pos - debut < 32 && estSchema(text.at(debut - 1))) --debut;
    // Le schema commence par une lettre : on saute chiffres et signes de tete.
    while (debut < pos && !text.at(debut).isLetter()) ++debut;
    qsizetype fin = pos + 3;
    while (fin < text.size() && !finUrl(text.at(fin))) ++fin;
    if (debut < pos) {
      sortie += text.mid(curseur, debut - curseur);
      sortie += scrubOneUrl(text.mid(debut, fin - debut));
      curseur = fin;
    } else if (fin > pos + 3) {
      // Pas de schema alphabetique devant ce `://` (`1://hote/app/cle`,
      // `1://rtmp://hote/cle`) : ce qui suit peut porter une cle, et QUrl ne
      // sait pas le lire sans schema. Masque en entier jusqu'au premier blanc.
      sortie += text.mid(curseur, pos - curseur);
      sortie += QStringLiteral("<redacted-url>");
      curseur = fin;
    }
    // Recherche suivante APRES ce qui vient d'etre parcouru : chaque caractere
    // n'est lu qu'une fois vers l'avant, le parcours reste lineaire meme sur
    // `1://1://1://...` (il refaisait le balayage jusqu'au blanc a chaque `://`).
    pos = text.indexOf(QStringLiteral("://"), fin);
  }
  if (curseur == 0) return text;
  sortie += text.mid(curseur);
  return sortie;
}

QString redactSecrets(const QString& text) {
  // Parcours a toute profondeur : une cle sensible peut etre imbriquee
  // (`destinations[].streamKey` de set_multistream_destinations).
  std::function<QJsonValue(const QJsonValue&, bool*)> masquer =
      [&](const QJsonValue& valeur, bool* touched) -> QJsonValue {
    if (valeur.isArray()) {
      QJsonArray sortie;
      for (const QJsonValue& item : valeur.toArray()) sortie.append(masquer(item, touched));
      return sortie;
    }
    if (valeur.isString()) {
      const QString brut = valeur.toString();
      const bool coupe = brut.size() > kMaxLoggedValue;
      const QString nettoye =
          scrubUrlText(coupe ? brut.left(kMaxLoggedValue) + QStringLiteral("…") : brut);
      if (!coupe && nettoye == brut) return valeur;
      *touched = true;
      return nettoye;
    }
    if (!valeur.isObject()) return valeur;
    QJsonObject obj = valeur.toObject();
    for (const QString& key : obj.keys()) {
      const QJsonValue v = obj.value(key);
      if (isSensitiveKey(key)) {
        // Valeur ENTIERE masquee, quel que soit son type. Seuls null et la
        // chaine vide restent (« pas de secret » est une information utile).
        if (v.isNull() || v.isUndefined() || (v.isString() && v.toString().isEmpty())) continue;
        if (v.isArray()) {
          // Tableau : on garde le nombre d'elements (utile au diagnostic).
          QJsonArray masked;
          const auto arr = v.toArray();
          for (int i = 0; i < arr.size(); ++i) masked.append(QStringLiteral("<redacted>"));
          obj[key] = masked;
        } else {
          obj[key] = QStringLiteral("<redacted>");
        }
        *touched = true;
        continue;
      }
      if (v.isObject() || v.isArray() || v.isString()) obj[key] = masquer(v, touched);
    }
    return obj;
  };

  const auto doc = QJsonDocument::fromJson(text.toUtf8());
  if (!doc.isObject() && !doc.isArray()) {
    // Pas du JSON : au moins les URLs du texte, borne a kMaxLoggedValue.
    return scrubUrlText(text.size() > kMaxLoggedValue
                            ? text.left(kMaxLoggedValue) + QStringLiteral("…")
                            : text);
  }
  bool touched = false;
  const QJsonValue racine = doc.isObject() ? QJsonValue(doc.object()) : QJsonValue(doc.array());
  const QJsonValue masque = masquer(racine, &touched);
  if (!touched) return text;
  const QJsonDocument sortie =
      masque.isObject() ? QJsonDocument(masque.toObject()) : QJsonDocument(masque.toArray());
  return QString::fromUtf8(sortie.toJson(QJsonDocument::Compact));
}

// Destination de diffusion poussee par le site : RTMP, RTMPS ou SRT vers un
// hote PUBLIC. Le message vient du serveur, mais l'URL finit telle quelle dans
// le service de diffusion d'OBS : sans cette garde, un serveur compromis ou un
// operateur du site faisait pousser tout le direct vers le reseau local du
// poste (`rtmp://192.168.1.10/...`), vers la machine elle-meme, ou par un
// protocole qu'OBS ne devrait pas ouvrir ici.
bool isAcceptableDestination(const QString& url) {
  static const QStringList kSchemes = {QStringLiteral("rtmp"), QStringLiteral("rtmps"),
                                       QStringLiteral("srt")};
  return Config::isPublicNetworkUrl(url, kSchemes);
}

} // namespace

LiveClient::LiveClient(Settings* settings, QObject* parent)
    : QObject(parent),
      settings_(settings),
      socket_(new WebSocketClient(this)),
      reconnectTimer_(new QTimer(this)),
      pingTimer_(new QTimer(this)),
      pongWatchdog_(new QTimer(this)),
      connectWatchdog_(new QTimer(this)) {
  reconnectTimer_->setSingleShot(true);
  connect(reconnectTimer_, &QTimer::timeout, this, &LiveClient::attemptReconnect);

  // Outbound WS ping every 25 s — well under the typical 60 s LB / Cloudflare
  // idle timeout, so the path stays warm and we get a pong to confirm it.
  pingTimer_->setInterval(25000);
  connect(pingTimer_, &QTimer::timeout, this, [this]() {
    if (!connected_) return;
    socket_->ping(QByteArrayLiteral("hb"));
    // Arm the watchdog only if not already running — otherwise consecutive
    // pings would keep pushing the deadline forward and we'd never notice a
    // dead path.
    if (!pongWatchdog_->isActive()) pongWatchdog_->start();
  });

  // If the server doesn't pong within 15 s the path is dead — Qt's own
  // disconnected signal can take minutes to fire on a half-open TCP.
  pongWatchdog_->setSingleShot(true);
  pongWatchdog_->setInterval(15000);
  connect(pongWatchdog_, &QTimer::timeout, this, [this]() {
    if (!connected_) return;
    forceReconnect("pong watchdog timeout");
  });

  // socket_->open() can sit in ConnectingState indefinitely if the SYN is
  // black-holed (corp firewall, captive portal). 15 s is the cap before we
  // tear down and retry through the normal backoff.
  connectWatchdog_->setSingleShot(true);
  connectWatchdog_->setInterval(15000);
  connect(connectWatchdog_, &QTimer::timeout, this, [this]() {
    if (connected_) return;
    if (socket_->state() == QAbstractSocket::ConnectingState ||
        socket_->state() == QAbstractSocket::HostLookupState) {
      forceReconnect("connect watchdog timeout");
    }
  });

  // Either a pong frame or any inbound text counts as proof the path is
  // alive. The latter matters because some intermediaries (rare, but seen
  // on enterprise LBs) drop ping frames without forwarding them; the
  // dashboard's normal traffic still keeps us honest.
  connect(socket_, &WebSocketClient::pong, this,
          [this](quint64 /*elapsed*/, const QByteArray& /*payload*/) {
            pongWatchdog_->stop();
          });

  connect(socket_, &WebSocketClient::connected, this, [this]() {
    connected_ = true;
    reconnectAttempts_ = 0;
    refreshRequestedThisEpisode_ = false;
    reconnectTimer_->stop();
    connectWatchdog_->stop();
    blog(LOG_INFO, "[semirprod-ws] connected to %s",
         Config::liveWsUrl().toUtf8().constData());
    startKeepalive();
    emit connectionChanged(true);
    emit statusMessage(QStringLiteral("Connexion temps réel Semirprod établie."));
    // Le POSTE s'annonce d'abord, le flux ensuite s'il y en a un.
    //
    // L'identite du poste ne s'etablissait qu'a `register_stream`, qui exige un
    // flux : une organisation qui n'en a aucun — une org perso, un client
    // fraichement cree — laissait donc un plugin connecte, authentifie, et
    // TOTALEMENT INVISIBLE cote dashboard. Impossible d'y distinguer un
    // appairage rate, une machine eteinte, un reseau coupe et un flux non
    // selectionne : les quatre donnaient le meme ecran vide.
    registered_ = false;
    restartRetryMs_ = -1;
    sendRegisterInstance();
  });

  connect(socket_, &WebSocketClient::disconnected, this, [this]() {
    // Log the close frame the server sent (RFC 6455 codes). 1009 = message
    // too big — that's the one to watch for if preview_frame payloads ever
    // outgrow the server's maxPayload again.
    const auto code = socket_->closeCode();
    const QString reason = socket_->closeReason();
    blog(LOG_INFO, "[semirprod-ws] disconnected (close=%d reason=\"%s\")",
         static_cast<int>(code),
         reason.isEmpty() ? "" : reason.toUtf8().constData());
    const bool wasConnected = connected_;
    connected_ = false;
    stopKeepalive();
    connectWatchdog_->stop();
    if (wasConnected) {
      emit connectionChanged(false);
      emit statusMessage(QStringLiteral("Connexion temps réel Semirprod interrompue."));
    }
    if (registered_) {
      registered_ = false;
      emit registrationChanged(false);
    }

    // Fermetures serveur du protocole v2. Les refus définitifs ne se
    // rejouent pas : reconnecter en boucle un poste révoqué ne ferait que
    // marteler le serveur et masquer la raison à l'écran.
    if (code == 4003 || code == 4403 || code == 4426) {
      refused_ = true;
      cancelReconnect();
      const QString why = code == 4403 ? QStringLiteral("device_revoked")
                          : code == 4426
                              ? (reason == QStringLiteral("plugin_blocked")
                                     ? QStringLiteral("plugin_blocked")
                                     : QStringLiteral("update_required"))
                              : QStringLiteral("identity_changed");
      emit sessionRefused(why);
      return;
    }
    // 4001 = jeton expiré (le filet : le renouvellement en vol aurait dû passer
    // avant). On demande un jeton neuf ; la reconnexion part avec lui.
    if (code == 4001) {
      emit tokenRenewalNeeded();
      return;
    }

    // Only retry while we still hold a token. disconnectClient() clears it,
    // which is how the user opts out of the auto-reconnect loop.
    if (!token_.isEmpty() && !refused_) scheduleReconnect();
  });

  connect(socket_, &WebSocketClient::errorOccurred,
          this, [this](QAbstractSocket::SocketError) {
            blog(LOG_WARNING, "[semirprod-ws] error: %s",
                 socket_->errorString().toUtf8().constData());
            // The first connection attempt may fail before `connected` ever
            // fires (DNS down at startup, server offline). In that case Qt
            // emits errorOccurred but not disconnected, so we'd never retry
            // unless we also schedule from here.
            stopKeepalive();
            connectWatchdog_->stop();
            if (!token_.isEmpty() && !refused_) scheduleReconnect();
          });

  // Verification TLS : validation STANDARD de Qt (chaine de confiance du
  // systeme + nom d'hote), sans epinglage.
  //
  // We intentionally do NOT call ignoreSslErrors() unconditionally: the
  // previous behaviour let any in-path attacker (corp proxy, malware,
  // hostile Wi-Fi) MITM the WS and harvest the JWT bearer token. Le
  // gestionnaire ne fait que journaliser : evaluateSslErrors rend une liste
  // vide tant que kPinningArmed vaut false, donc toute erreur TLS fait
  // echouer la poignee de main.
  connect(socket_, &WebSocketClient::sslErrors, this,
          [this](const QList<QSslError>& errors) {
            const QList<QSslError> ignorable = evaluateSslErrors(errors);
            for (const auto& e : errors) {
              const bool willIgnore = ignorable.contains(e);
              blog(willIgnore ? LOG_WARNING : LOG_ERROR,
                   "[semirprod-ws] ssl error %s: %s",
                   willIgnore ? "(ignored)" : "(will fail)",
                   e.errorString().toUtf8().constData());
            }
            if (!ignorable.isEmpty()) socket_->ignoreSslErrors(ignorable);
          });

  connect(socket_, &WebSocketClient::textMessageReceived, this, &LiveClient::handleTextMessage);
}

QString LiveClient::spkiPinOfChain(const QList<QSslCertificate>& chain) {
  if (chain.isEmpty()) return {};
  const QSslCertificate leaf = chain.first();
  // Qt exposes the public key but not the SPKI DER directly. Reassemble it
  // as SubjectPublicKeyInfo using OpenSSL-style headers. We do this with
  // QCryptographicHash + the public-key DER bytes — that gives a stable
  // hash over the same bytes a pin-checking library (e.g. TrustKit) would
  // hash. The result is functionally equivalent to SHA-256(SPKI DER).
  const QByteArray pubKeyDer = leaf.publicKey().toDer();
  if (pubKeyDer.isEmpty()) return {};
  const QByteArray digest = QCryptographicHash::hash(pubKeyDer, QCryptographicHash::Sha256);
  return QString::fromLatin1(digest.toBase64());
}

QList<QSslError> LiveClient::evaluateSslErrors(const QList<QSslError>& errors) {
  QList<QSslError> ignorable;
  if (errors.isEmpty()) return ignorable;

  const QSslConfiguration cfg = socket_->sslConfiguration();
  const QList<QSslCertificate> chain = cfg.peerCertificateChain();
  const QString pin = spkiPinOfChain(chain);

  // The pins in kPinnedSpkis are placeholders until the real SPKI is baked
  // in (see Config.hpp comments). Until then we must fail closed: never
  // ignore ANY error, because the pin we have can't actually match.
  // Flip the constant to enable pinning once the real values land.
  constexpr bool kPinningArmed = false;
  if (!kPinningArmed) {
    blog(LOG_ERROR,
         "[semirprod-ws] erreur TLS : connexion refusee (validation standard, aucune "
         "erreur ignoree)");
    return {};
  }

  const bool pinMatches = kPinnedSpkis.contains(pin);
  if (!pinMatches) {
    blog(LOG_ERROR,
         "[semirprod-ws] leaf SPKI pin mismatch — server cert not in pinned set. "
         "Got pin: %s",
         pin.toUtf8().constData());
    return {};
  }

  // Pin matches: tolerate ONLY the cert-trust-category errors. Hostname
  // mismatches, expired/revoked, weak signature, etc. are still fatal —
  // they indicate misconfiguration we want to know about loudly.
  for (const auto& e : errors) {
    switch (e.error()) {
      case QSslError::UnableToGetLocalIssuerCertificate:
      case QSslError::UnableToVerifyFirstCertificate:
      case QSslError::CertificateUntrusted:
      case QSslError::NoSslSupport:
        ignorable.append(e);
        break;
      default:
        // All other errors (HostNameMismatch, CertificateExpired, etc.)
        // stay loud — don't add to ignorable.
        break;
    }
  }
  return ignorable;
}

void LiveClient::connectWithToken(const QString& token) {
  if (halted_) return;
  // A fresh bearer starts a new stall episode — re-arm the refresh request so
  // that if this token is also refused later we can ask for another refresh.
  if (token != token_) refreshRequestedThisEpisode_ = false;
  token_ = token;
  if (token_.isEmpty()) {
    disconnectClient();
    return;
  }
  refused_ = false;

  // Liaison ouverte : on renouvelle SUR le socket. Rouvrir coupait la régie
  // toutes les ~16 min en prod (137 fermetures en 72 h, trou médian 4,6 s).
  if (connected_) {
    QJsonObject renew;
    renew[QStringLiteral("type")] = QStringLiteral("auth_renew");
    renew[QStringLiteral("token")] = token_;
    sendJson(renew);
    return;
  }

  // Belt-and-suspenders: refuse to even open a non-TLS or non-allowlisted URL.
  const QUrl target(Config::liveWsUrl());
  if (target.scheme().toLower() != QStringLiteral("wss") ||
      !Config::isAllowedHost(Config::liveWsUrl())) {
    blog(LOG_ERROR, "[semirprod-ws] refusing to open %s — scheme/host not allowed",
         Config::liveWsUrl().toUtf8().constData());
    emit statusMessage(QStringLiteral("URL live-ws non autorisée."));
    return;
  }

  cancelReconnect();
  QNetworkRequest request{target};
  request.setRawHeader("Authorization", QStringLiteral("Bearer %1").arg(token_).toUtf8());
  request.setRawHeader("User-Agent",
                       QStringLiteral("SemirprodStudio/%1").arg(QStringLiteral(SEMIRPROD_PLUGIN_VERSION)).toUtf8());
  // Never follow a handshake redirect. A WS endpoint answering 3xx is abnormal,
  // and Qt would re-send the Authorization bearer to the redirect target —
  // potentially a host outside our allowlist. Manual policy makes the open()
  // fail instead, which the reconnect loop already handles.
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                       QNetworkRequest::ManualRedirectPolicy);
  blog(LOG_INFO, "[semirprod-ws] opening %s",
       Config::liveWsUrl().toUtf8().constData());
  socket_->open(request);
  connectWatchdog_->start();
}

void LiveClient::disconnectClient() {
  token_.clear();
  registered_ = false;
  restartRetryMs_ = -1;
  registeredStreamId_.clear();
  sceneLive_.clear();
  sceneBug_.clear();
  cancelReconnect();
  stopKeepalive();
  connectWatchdog_->stop();
  // abort() au lieu de close() : coupe le TCP immediatement, sans attendre le
  // handshake de fermeture WebSocket. Deux raisons : la sortie d'OBS ne doit
  // pas attendre le reseau, et le re-connect immediat (changement de stream)
  // ne doit pas ouvrir un socket encore en cours de fermeture.
  socket_->abort();
  if (connected_) {
    connected_ = false;
    emit connectionChanged(false);
  }
}

void LiveClient::startKeepalive() {
  pingTimer_->start();
}

void LiveClient::stopKeepalive() {
  pingTimer_->stop();
  pongWatchdog_->stop();
}

void LiveClient::forceReconnect(const char* reason) {
  blog(LOG_WARNING, "[semirprod-ws] forcing reconnect: %s", reason);
  stopKeepalive();
  connectWatchdog_->stop();
  // abort() drops the TCP immediately and emits disconnected, which schedules
  // the reconnect through the normal path while keeping reconnectAttempts_
  // intact for the backoff calculation.
  socket_->abort();
}

bool LiveClient::isReconnecting() const {
  if (reconnectTimer_->isActive()) return true;
  const auto state = socket_->state();
  return state == QAbstractSocket::HostLookupState ||
         state == QAbstractSocket::ConnectingState ||
         state == QAbstractSocket::ConnectedState;
}

void LiveClient::scheduleReconnect() {
  if (reconnectTimer_->isActive()) return;
  // Redémarrage annoncé du pont : on revient tout de suite sur le pod suivant,
  // sans reprise exponentielle ni compteur d'échecs.
  if (restartRetryMs_ >= 0) {
    const int delay = restartRetryMs_;
    restartRetryMs_ = -1;
    blog(LOG_INFO, "[semirprod-ws] redemarrage du serveur annonce : reconnexion dans %d ms", delay);
    reconnectTimer_->start(delay);
    return;
  }
  // 1s, 2s, 4s, 8s, 16s, 30s, 30s…
  const int capMs = 30000;
  const int baseMs = 1000;
  const int shift = std::min(reconnectAttempts_, 5); // 2^5 = 32 → clamp by capMs
  const int delayMs = std::min(capMs, baseMs << shift);
  blog(LOG_INFO, "[semirprod-ws] scheduling reconnect in %d ms (attempt %d)",
       delayMs, reconnectAttempts_ + 1);
  reconnectTimer_->start(delayMs);
}

void LiveClient::attemptReconnect() {
  if (token_.isEmpty()) return;
  ++reconnectAttempts_;

  // After a couple of failed reopens with the same bearer, assume the token
  // is the problem (expired mid-session / rejected by the server) and ask the
  // dock to refresh it before we keep hammering the dead credential. Fire once
  // per episode so we don't spam the auth server on every backoff tick — the
  // dock's refresh will call connectWithToken() with the new bearer, which
  // resets reconnectAttempts_ and this flag on success. If the refresh fails,
  // the normal backoff loop below stays as the fallback.
  constexpr int kRefreshAfterAttempts = 2;
  // Tant que l'episode dure, la demande est renouvelee toutes les 10
  // tentatives (~5 min au plafond de 30 s) : c'est ce que faisait le chien de
  // garde periodique du dock, supprime. Sans ca, un renouvellement rate une
  // fois laissait le socket reessayer le meme jeton mort indefiniment.
  constexpr int kRefreshEveryAttempts = 10;
  const bool firstStall =
      reconnectAttempts_ >= kRefreshAfterAttempts && !refreshRequestedThisEpisode_;
  const bool stillStalled =
      refreshRequestedThisEpisode_ && reconnectAttempts_ > kRefreshAfterAttempts &&
      (reconnectAttempts_ - kRefreshAfterAttempts) % kRefreshEveryAttempts == 0;
  if (firstStall || stillStalled) {
    refreshRequestedThisEpisode_ = true;
    blog(LOG_INFO,
         "[semirprod-ws] reconnect stalled after %d attempts — requesting token refresh",
         reconnectAttempts_);
    emit reconnectStalled();
  }

  // Make sure the socket is fully torn down before reopening; otherwise Qt
  // refuses the open() call and we silently stall.
  if (socket_->state() != QAbstractSocket::UnconnectedState) {
    socket_->abort();
  }
  const QUrl target(Config::liveWsUrl());
  if (target.scheme().toLower() != QStringLiteral("wss") ||
      !Config::isAllowedHost(Config::liveWsUrl())) {
    blog(LOG_ERROR, "[semirprod-ws] reconnect aborted — URL not allowed");
    return;
  }
  QNetworkRequest request{target};
  request.setRawHeader("Authorization", QStringLiteral("Bearer %1").arg(token_).toUtf8());
  request.setRawHeader("User-Agent",
                       QStringLiteral("SemirprodStudio/%1").arg(QStringLiteral(SEMIRPROD_PLUGIN_VERSION)).toUtf8());
  // Same redirect hardening as connectWithToken — don't let a handshake 3xx
  // forward the bearer off our allowlist.
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                       QNetworkRequest::ManualRedirectPolicy);
  blog(LOG_INFO, "[semirprod-ws] reconnect attempt #%d -> %s",
       reconnectAttempts_, Config::liveWsUrl().toUtf8().constData());
  socket_->open(request);
  connectWatchdog_->start();
}

void LiveClient::cancelReconnect() {
  reconnectTimer_->stop();
  reconnectAttempts_ = 0;
}

void LiveClient::registerStream(const QString& streamId, const QString& sceneLive, const QString& sceneBug) {
  sceneLive_ = sceneLive;
  sceneBug_ = sceneBug;
  const bool changed = streamId != registeredStreamId_;
  registeredStreamId_ = streamId;
  // Enregistré : on change la cible SUR le socket. Sinon, la réponse à
  // `register_instance` la réconciliera (voir `register_instance_result`).
  if (changed && isRegistered()) sendSetTarget();
}

void LiveClient::sendSetTarget() {
  QJsonObject payload;
  payload[QStringLiteral("type")] = QStringLiteral("set_target");
  payload[QStringLiteral("streamId")] =
      registeredStreamId_.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(registeredStreamId_);
  sendJson(payload);
}

// Annonce du POSTE v2. L'identité (appareil, espace) est dans le JWT : le
// message ne porte que ce que le serveur ne peut pas savoir. `target` n'est
// qu'un indice, utilisé si le serveur n'a rien mémorisé.
void LiveClient::sendRegisterInstance() {
  if (!connected_) return;

  QJsonObject payload;
  payload[QStringLiteral("type")] = QStringLiteral("register_instance");
  payload[QStringLiteral("label")] = instanceLabel();
  payload[QStringLiteral("clientVersion")] = QStringLiteral(SEMIRPROD_PLUGIN_VERSION);
#if defined(Q_OS_WIN)
  payload[QStringLiteral("platform")] = QStringLiteral("windows");
#elif defined(Q_OS_MACOS)
  payload[QStringLiteral("platform")] = QStringLiteral("macos");
#else
  payload[QStringLiteral("platform")] = QStringLiteral("linux");
#endif
  if (!registeredStreamId_.isEmpty()) payload[QStringLiteral("target")] = registeredStreamId_;
  // Advertise the WebRTC program-preview capability ONLY when this OBS build
  // ships the native whip_output type (OBS 30+).
  QJsonArray capabilities{QStringLiteral("studio_mode"),
                          QStringLiteral("remote_config_switch"),
                          QStringLiteral("scene_mapping"),
                          QStringLiteral("stream_switch"),
                          QStringLiteral("audio_levels")};
  if (ProgramPreviewOutput::isSupported()) {
    capabilities.append(QStringLiteral("program_preview_whip"));
    capabilities.append(QStringLiteral("studio_preview_whip"));
    // Apercu du canvas 9:16 : seulement si Semirprod Vertical est charge et
    // porte le contrat v2. Calcule a chaque annonce : Vertical se charge
    // apres le Studio (ordre alphabetique), mais bien avant la connexion.
    if (VerticalBridge::previewSupported())
      capabilities.append(QStringLiteral("vertical_preview_whip"));
    // Apercu d'un canvas NOMME (edition regie, canvas crees par canvas-kit).
    // L'API canvas de libobs existe depuis 31.1, sous notre plancher 32.0 :
    // seule la sortie WHIP conditionne cette capacite.
    capabilities.append(QStringLiteral("canvas_preview_whip"));
  }
  // Edition regie : chaines du profil poussees au multistream.
  if (MultistreamBridge::available())
    capabilities.append(QStringLiteral("multistream"));
  payload[QStringLiteral("capabilities")] = capabilities;
  sendJson(payload);
}

QString LiveClient::instanceId() const {
  // Poste v2 : l'identifiant du poste EST celui de l'appareil. L'ancien
  // `Settings::instanceId`, tiré par le plugin et gardé à vie, était la cause
  // du « poste resté sur l'ancien compte ».
  return deviceId_;
}

QString LiveClient::instanceLabel() const {
  return settings_ ? settings_->instanceLabel() : QString();
}

bool LiveClient::messageTargetsThisInstance(const QJsonObject& obj) const {
  const QString target = obj.value(QStringLiteral("targetInstanceId")).toString();
  if (target.isEmpty()) return true; // broadcast to all instances
  return target == instanceId();
}

namespace {

// Messages à haute fréquence dont seule la DERNIÈRE valeur compte. Si l'envoi
// sature (aperçu JPEG sur une liaison montante déjà occupée par le direct), on
// les jette plutôt que de les empiler : sinon chaque résultat de commande
// attend derrière des centaines de Ko d'images périmées.
bool isDroppable(const QString& type) {
  return type == QStringLiteral("preview_frame") ||
         type == QStringLiteral("audio_levels") ||
         type == QStringLiteral("output_stats");
}

// Ces messages-là, en plus des jetables, partent plusieurs fois par seconde :
// les écrire dans le journal d'OBS (écriture disque synchrone) coûte du temps
// au fil principal pour une information sans valeur.
bool isChatty(const QString& type) {
  return isDroppable(type) || type == QStringLiteral("instance_heartbeat");
}

// Au-delà, un message jetable n'est pas mis en file. Une image d'aperçu 720p
// pèse 80 à 200 Ko en base64 : le seuil laisse passer une image à la fois,
// donc une commande n'attend jamais derrière plus d'une image.
constexpr qint64 kDroppableBacklogBytes = 64 * 1024;

}  // namespace

void LiveClient::sendJson(const QJsonObject& payload) {
  const QString type = payload.value(QStringLiteral("type")).toString();
  if (isDroppable(type) && socket_->pendingBytes() > kDroppableBacklogBytes) {
    ++droppedWhileBacklogged_;
    return;
  }
  if (droppedWhileBacklogged_ > 0 && !isDroppable(type)) {
    blog(LOG_INFO, "[semirprod-ws] envoi sature : %lld messages d'apercu ou de mesure jetes",
         static_cast<long long>(droppedWhileBacklogged_));
    droppedWhileBacklogged_ = 0;
  }

  const QByteArray body = QJsonDocument(payload).toJson(QJsonDocument::Compact);
  // Au-dessus du plafond annoncé par `hello`, le serveur refuserait le
  // message : un jetable ne part pas, le reste part et sera refusé proprement
  // (le socket reste ouvert en v2).
  if (isDroppable(type) && body.size() > maxPayloadBytes_) {
    ++droppedWhileBacklogged_;
    return;
  }
  socket_->sendTextMessage(QString::fromUtf8(body));

  if (isChatty(type)) {
    blog(LOG_DEBUG, "[semirprod-ws] -> %s (%lld bytes)", type.toUtf8().constData(),
         static_cast<long long>(body.size()));
    return;
  }
  const QString outRedacted = redactSecrets(QString::fromUtf8(body));
  blog(LOG_INFO, "[semirprod-ws] -> %s", outRedacted.toUtf8().constData());
  emit wsTraffic(QStringLiteral("out"), outRedacted);
}

void LiveClient::sendSwitchResult(
    const QString& requestId,
    const QString& streamId,
    const QString& target,
    const QString& status,
    const QString& reason,
    const QString& activeScene,
    const QString& appliedScene,
    qint64 latencyMs) {
  if (!connected_) {
    return;
  }

  QJsonObject payload;
  payload[QStringLiteral("type")] = QStringLiteral("switch_result");
  payload[QStringLiteral("requestId")] = requestId;
  payload[QStringLiteral("streamId")] = streamId;
  payload[QStringLiteral("target")] = target;
  payload[QStringLiteral("status")] = status;
  if (!reason.isEmpty()) payload[QStringLiteral("reason")] = reason;
  payload[QStringLiteral("activeScene")] = activeScene;
  if (!appliedScene.isEmpty()) payload[QStringLiteral("appliedScene")] = appliedScene;
  payload[QStringLiteral("latencyMs")] = static_cast<double>(latencyMs);
  payload[QStringLiteral("ts")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
  sendJson(payload);
}

void LiveClient::sendSourceState(
    const QString& streamId,
    bool ready,
    int width,
    int height,
    const QString& activeScene,
    const QString& sourceScene) {
  if (!connected_ || streamId.isEmpty()) {
    // Called by the dock's 2 s polling timer; logging here would spam the OBS
    // log every tick while the plugin is disconnected. (Downgraded from INFO
    // to DEBUG by audit C5 — was a noise source.)
    return;
  }

  QJsonObject payload;
  payload[QStringLiteral("type")] = QStringLiteral("source_state");
  payload[QStringLiteral("streamId")] = streamId;
  payload[QStringLiteral("ready")] = ready;
  payload[QStringLiteral("width")] = width;
  payload[QStringLiteral("height")] = height;
  if (!activeScene.isEmpty()) payload[QStringLiteral("activeScene")] = activeScene;
  if (!sourceScene.isEmpty()) payload[QStringLiteral("sourceScene")] = sourceScene;
  payload[QStringLiteral("ts")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
  sendJson(payload);
}

void LiveClient::sendObsState(const ObsStateReport& report) {
  if (!connected_) return;

  const auto toArray = [](const QStringList& values) {
    QJsonArray array;
    for (const QString& value : values) array.append(value);
    return array;
  };

  QJsonObject payload;
  payload[QStringLiteral("type")] = QStringLiteral("obs_state");
  payload[QStringLiteral("instanceId")] = instanceId();
  payload[QStringLiteral("instanceLabel")] = instanceLabel();
  if (!report.streamId.isEmpty())
    payload[QStringLiteral("streamId")] = report.streamId;
  payload[QStringLiteral("activeScene")] = report.activeScene;
  payload[QStringLiteral("scenes")] = toArray(report.scenes);
  payload[QStringLiteral("sceneLive")] = report.sceneLive;
  payload[QStringLiteral("sceneBug")] = report.sceneBug;
  payload[QStringLiteral("autoSwitch")] = report.autoSwitch;
  payload[QStringLiteral("streaming")] = report.streaming;
  payload[QStringLiteral("recording")] = report.recording;
  payload[QStringLiteral("sourceReady")] = report.sourceReady;
  payload[QStringLiteral("width")] = report.width;
  payload[QStringLiteral("height")] = report.height;
  // Always include the keys (use null when idle) so the dashboard knows the
  // plugin understands the contract — vs an old plugin where these would
  // simply be missing.
  payload[QStringLiteral("streamingStartedAt")] =
      report.streamingStartedAt.isEmpty()
          ? QJsonValue(QJsonValue::Null)
          : QJsonValue(report.streamingStartedAt);
  payload[QStringLiteral("recordingStartedAt")] =
      report.recordingStartedAt.isEmpty()
          ? QJsonValue(QJsonValue::Null)
          : QJsonValue(report.recordingStartedAt);
  // Mode studio : `studioMode` est TOUJOURS présent (un ancien plugin l'omet,
  // ce que le dashboard lit comme « inconnu » et non comme « désactivé »).
  // `previewScene` vaut null hors mode studio : il n'y a alors pas de scène en
  // préparation, et une chaîne vide se confondrait avec « scène sans nom ».
  payload[QStringLiteral("studioMode")] = report.studioMode;
  payload[QStringLiteral("previewScene")] =
      report.previewScene.isEmpty() ? QJsonValue(QJsonValue::Null)
                                    : QJsonValue(report.previewScene);
  payload[QStringLiteral("profiles")] = toArray(report.profiles);
  payload[QStringLiteral("currentProfile")] = report.currentProfile;
  payload[QStringLiteral("sceneCollections")] = toArray(report.sceneCollections);
  payload[QStringLiteral("currentSceneCollection")] =
      report.currentSceneCollection;
  // Toujours présent, null si Vertical est absent : un ancien plugin omet la
  // clé, ce que le site lit comme « ne sait pas piloter le vertical ».
  if (report.verticalAvailable) {
    QJsonObject vertical;
    vertical[QStringLiteral("streaming")] = report.verticalStreaming;
    vertical[QStringLiteral("configured")] = report.verticalConfigured;
    vertical[QStringLiteral("followStream")] = report.verticalFollowStream;
    vertical[QStringLiteral("server")] = report.verticalServer;
    vertical[QStringLiteral("error")] =
        report.verticalError.isEmpty() ? QJsonValue(QJsonValue::Null)
                                       : QJsonValue(report.verticalError);
    payload[QStringLiteral("vertical")] = vertical;
  } else {
    payload[QStringLiteral("vertical")] = QJsonValue(QJsonValue::Null);
  }
  payload[QStringLiteral("multistream")] = report.multistream;
  sendStateIfChanged(payload);
}

void LiveClient::sendOutputStats(
    const QString& streamId,
    float congestion,
    int kbps,
    int droppedFrames,
    int totalFrames,
    int droppedDelta,
    int renderSkipped,
    int renderTotal) {
  if (!connected_) return;

  QJsonObject payload;
  payload[QStringLiteral("type")] = QStringLiteral("output_stats");
  payload[QStringLiteral("instanceId")] = instanceId();
  if (!streamId.isEmpty()) payload[QStringLiteral("streamId")] = streamId;
  // Only sent while the output is active (the dock gates on it), so streaming
  // is always true here — kept explicit to match the obs_state contract.
  payload[QStringLiteral("streaming")] = true;
  payload[QStringLiteral("kbps")] = kbps;
  payload[QStringLiteral("congestion")] = static_cast<double>(congestion);
  payload[QStringLiteral("droppedFrames")] = droppedFrames;
  payload[QStringLiteral("totalFrames")] = totalFrames;
  payload[QStringLiteral("droppedDelta")] = droppedDelta;
  payload[QStringLiteral("renderSkipped")] = renderSkipped;
  payload[QStringLiteral("renderTotal")] = renderTotal;
  payload[QStringLiteral("ts")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
  sendJson(payload);
}

void LiveClient::sendControlResult(
    const QString& requestId,
    const QString& action,
    const QString& status,
    const QString& reason) {
  if (!connected_) return;

  QJsonObject payload;
  payload[QStringLiteral("type")] = QStringLiteral("control_result");
  payload[QStringLiteral("instanceId")] = instanceId();
  if (!requestId.isEmpty()) payload[QStringLiteral("requestId")] = requestId;
  payload[QStringLiteral("action")] = action;
  payload[QStringLiteral("status")] = status;
  if (!reason.isEmpty()) payload[QStringLiteral("reason")] = reason;
  payload[QStringLiteral("ts")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
  sendJson(payload);
}

// Etat du poste : envoye SEULEMENT quand son contenu change. L'empreinte est
// le JSON compact du message SANS horodatage (les cles d'un QJsonObject sont
// triees, la serialisation est donc stable). Le battement de 5 s ne porte plus
// d'etat : sans ce filtre, chaque poste renvoyait trois messages identiques
// toutes les 5 s, relus et republies par le pont pour rien.
void LiveClient::sendStateIfChanged(QJsonObject payload) {
  const QString type = payload.value(QStringLiteral("type")).toString();
  const QByteArray fingerprint = QJsonDocument(payload).toJson(QJsonDocument::Compact);
  const auto it = stateFingerprints_.constFind(type);
  if (it != stateFingerprints_.constEnd() && it.value() == fingerprint) return;
  stateFingerprints_.insert(type, fingerprint);
  payload[QStringLiteral("ts")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
  sendJson(payload);
}

void LiveClient::forgetStateFingerprint(const QString& type) {
  if (type.isEmpty()) {
    stateFingerprints_.clear();
  } else {
    stateFingerprints_.remove(type);
  }
}

void LiveClient::sendSourcesState(const QString& streamId, const QJsonArray& scenes) {
  if (!connected_) return;
  QJsonObject payload;
  payload[QStringLiteral("type")] = QStringLiteral("sources_state");
  payload[QStringLiteral("instanceId")] = instanceId();
  payload[QStringLiteral("instanceLabel")] = instanceLabel();
  if (!streamId.isEmpty()) payload[QStringLiteral("streamId")] = streamId;
  payload[QStringLiteral("scenes")] = scenes;
  sendStateIfChanged(payload);
}

void LiveClient::sendAudioState(const QString& streamId, const QJsonArray& inputs) {
  if (!connected_) return;
  QJsonObject payload;
  payload[QStringLiteral("type")] = QStringLiteral("audio_state");
  payload[QStringLiteral("instanceId")] = instanceId();
  payload[QStringLiteral("instanceLabel")] = instanceLabel();
  if (!streamId.isEmpty()) payload[QStringLiteral("streamId")] = streamId;
  payload[QStringLiteral("inputs")] = inputs;
  sendStateIfChanged(payload);
}

void LiveClient::sendAudioLevels(const QString& streamId,
                                 const QJsonArray& levels) {
  if (!connected_) return;
  QJsonObject payload;
  payload[QStringLiteral("type")] = QStringLiteral("audio_levels");
  payload[QStringLiteral("instanceId")] = instanceId();
  if (!streamId.isEmpty()) payload[QStringLiteral("streamId")] = streamId;
  payload[QStringLiteral("levels")] = levels;
  payload[QStringLiteral("ts")] =
      QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
  sendJson(payload);
}

void LiveClient::sendSourceControlResult(
    const QString& requestId,
    const QString& action,
    const QString& status,
    const QString& reason) {
  if (!connected_) return;
  QJsonObject payload;
  payload[QStringLiteral("type")] = QStringLiteral("source_control_result");
  payload[QStringLiteral("instanceId")] = instanceId();
  if (!requestId.isEmpty()) payload[QStringLiteral("requestId")] = requestId;
  payload[QStringLiteral("action")] = action;
  payload[QStringLiteral("status")] = status;
  if (!reason.isEmpty()) payload[QStringLiteral("reason")] = reason;
  payload[QStringLiteral("ts")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
  sendJson(payload);
}

void LiveClient::sendPreviewFrame(const QString& streamId, const QString& base64) {
  if (!connected_ || streamId.isEmpty() || base64.isEmpty()) return;

  QJsonObject payload;
  payload[QStringLiteral("type")] = QStringLiteral("preview_frame");
  payload[QStringLiteral("instanceId")] = instanceId();
  payload[QStringLiteral("streamId")] = streamId;
  payload[QStringLiteral("img")] = base64;
  payload[QStringLiteral("ts")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
  sendJson(payload);
}

void LiveClient::sendInstanceHeartbeat() {
  if (!connected_) return;

  QJsonObject payload;
  payload[QStringLiteral("type")] = QStringLiteral("instance_heartbeat");
  payload[QStringLiteral("instanceId")] = instanceId();
  payload[QStringLiteral("instanceLabel")] = instanceLabel();
  if (!registeredStreamId_.isEmpty()) {
    payload[QStringLiteral("streamId")] = registeredStreamId_;
  }
  payload[QStringLiteral("ts")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
  sendJson(payload);
}

namespace {

// Deux messages identiques a moins de ca sont un rejeu, pas deux intentions.
// Le doublon du 15 aout est arrive a 58 ms — la marge est enorme, et aucune
// commande legitime n'est emise deux fois a l'octet pres dans cette fenetre
// (un rafraichissement de bail porte un jeton et un horodatage neufs).
constexpr qint64 kReplayWindowMs = 2000;

// Fenetre des seaux de debit.
constexpr qint64 kRateWindowMs = 10000;

// Plafond global, toutes commandes mutantes confondues. Large : il n'est la
// que pour borner un emetteur devenu fou, pas pour brider un operateur.
constexpr int kMaxCommandsPerWindow = 200;

// Commandes LOURDES : chacune cree ou detruit des objets libobs (sorties,
// encodeurs, mix video, sessions). Ce sont elles qui font tomber OBS quand on
// les enchaine, donc elles ont le plafond serre.
constexpr int kMaxHeavyPerWindow = 6;

// Commandes LEGERES : de simples reglages. Le plafond doit rester au-dessus de
// ce qu'un operateur produit en bougeant un curseur de volume.
constexpr int kMaxLightPerWindow = 60;

// ⚠️ Ces chaines sont le PROTOCOLE DU FIL, pas des noms internes. Elles doivent
// etre copiees a l'octet pres depuis le dispatcher de handleTextMessage(), et
// ce sont les memes que celles qu'accepte le serveur pour le pilotage a
// distance.
//
// Un nom qui ne correspond a rien ne provoque AUCUNE erreur : la commande
// devient simplement non-mutante aux yeux du portier, donc dispensee
// d'anti-rejeu et de plafond de debit. C'est exactement ce qui s'est produit
// ici : `start_recording`/`stop_recording` etaient listes alors que le fil dit
// `start_record`/`stop_record`, si bien que les deux commandes les plus lourdes
// du lot — chacune cree ou detruit une sortie libobs — passaient sans aucune
// borne, rejouables a l'infini. Le contrat cote plugin repetait la meme faute.
// Commandes de pilotage direct de la diffusion et de l'enregistrement.
//
// UNE SEULE definition, partagee par le portier ET par le dispatcher plus bas :
// c'est leur duplication qui avait produit le trou. Ajouter une commande ici la
// rend automatiquement bornee ; l'ajouter au dispatcher seul ne compilerait
// plus au meme endroit.
const QSet<QString>& streamControlCommands() {
  static const QSet<QString> kControl = {
      QStringLiteral("start_stream"),
      QStringLiteral("stop_stream"),
      QStringLiteral("start_record"),
      QStringLiteral("stop_record"),
      QStringLiteral("start_vertical_stream"),
      QStringLiteral("stop_vertical_stream"),
      QStringLiteral("start_multistream"),
      QStringLiteral("stop_multistream"),
  };
  return kControl;
}

bool isHeavyCommand(const QString& type) {
  static const QSet<QString> kHeavyExtra = {
      QStringLiteral("start_program_preview"),
      QStringLiteral("stop_program_preview"),
      QStringLiteral("start_studio_preview"),
      QStringLiteral("stop_studio_preview"),
      QStringLiteral("start_vertical_preview"),
      QStringLiteral("stop_vertical_preview"),
      QStringLiteral("start_canvas_preview"),
      QStringLiteral("stop_canvas_preview"),
      QStringLiteral("start_preview"),
      QStringLiteral("stop_preview"),
      // Capture du programme a la demande : trois rendus d'OBS et un encodage
      // JPEG. Lourde, donc plafonnee, et soumise a l'anti-rejeu.
      QStringLiteral("get_preview"),
      QStringLiteral("set_scene_collection"),
      QStringLiteral("create_scene_collection"),
      QStringLiteral("set_profile"),
      QStringLiteral("set_stream"),
      // Pose l'URL d'ingest et la CLÉ DE FLUX du client sur le service de
      // diffusion, et réécrit le profil sur disque. Lourde à ce double titre :
      // elle écrit, et elle décide vers qui part le direct. Rejouable à
      // l'infini, elle permettrait de marteler le profil pendant qu'un client
      // règle son émission.
      QStringLiteral("set_destination"),
      QStringLiteral("set_vertical_destination"),
      QStringLiteral("set_multistream_destinations"),
  };
  return kHeavyExtra.contains(type) || streamControlCommands().contains(type);
}

// Tout ce qui a un effet de bord sur OBS. Les messages purement informatifs
// (publish_state, quality_state, register_stream_result, hb...) ne passent pas
// par le portier : ils ne declenchent aucune construction d'objet.
bool isMutatingCommand(const QString& type) {
  if (isHeavyCommand(type)) return true;
  static const QSet<QString> kLight = {
      QStringLiteral("switch_scene"),
      QStringLiteral("set_scene_mapping"),
      QStringLiteral("set_studio_mode"),
      QStringLiteral("set_preview_scene"),
      QStringLiteral("trigger_transition"),
      QStringLiteral("set_source_visible"),
      QStringLiteral("set_source_locked"),
      QStringLiteral("set_audio_volume"),
      QStringLiteral("set_audio_mute"),
      QStringLiteral("set_audio_monitoring"),
  };
  return kLight.contains(type);
}

// Retire de `stamps` tout ce qui est sorti de la fenetre. Les historiques
// restent donc bornes par le debit autorise, jamais par la duree de session.
void dropExpired(QVector<qint64>& stamps, qint64 now, qint64 window) {
  int keep = 0;
  while (keep < stamps.size() && now - stamps[keep] > window) ++keep;
  if (keep > 0) stamps.remove(0, keep);
}

}  // namespace

bool LiveClient::acceptRemoteCommand(const QString& type,
                                     const QString& rawText) {
  if (!commandClock_.isValid()) commandClock_.start();
  const qint64 now = commandClock_.elapsed();

  // --- 1. Rejeu ------------------------------------------------------------
  const QByteArray fingerprint = QCryptographicHash::hash(
      rawText.toUtf8(), QCryptographicHash::Sha256);

  for (auto it = recentMessages_.begin(); it != recentMessages_.end();) {
    if (now - it.value() > kReplayWindowMs) {
      it = recentMessages_.erase(it);
    } else {
      ++it;
    }
  }
  if (recentMessages_.contains(fingerprint)) {
    blog(LOG_WARNING,
         "[semirprod-ws] commande '%s' ignoree — message identique deja traite "
         "il y a %lld ms (rejeu)",
         type.toUtf8().constData(), now - recentMessages_.value(fingerprint));
    return false;
  }

  // --- 2. Debit ------------------------------------------------------------
  dropExpired(allCommandHistory_, now, kRateWindowMs);
  if (allCommandHistory_.size() >= kMaxCommandsPerWindow) {
    blog(LOG_WARNING,
         "[semirprod-ws] commande '%s' ignoree — plafond global atteint "
         "(%d commandes en %lld ms)",
         type.toUtf8().constData(), kMaxCommandsPerWindow, kRateWindowMs);
    return false;
  }

  QVector<qint64>& perType = commandHistory_[type];
  dropExpired(perType, now, kRateWindowMs);
  const int perTypeMax =
      isHeavyCommand(type) ? kMaxHeavyPerWindow : kMaxLightPerWindow;
  if (perType.size() >= perTypeMax) {
    blog(LOG_WARNING,
         "[semirprod-ws] commande '%s' ignoree — plafond de type atteint "
         "(%d en %lld ms)",
         type.toUtf8().constData(), perTypeMax, kRateWindowMs);
    return false;
  }

  recentMessages_.insert(fingerprint, now);
  perType.append(now);
  allCommandHistory_.append(now);
  return true;
}

void LiveClient::handleTextMessage(const QString& text) {
  // Any inbound message proves the path is alive — same effect as a pong.
  pongWatchdog_->stop();

  // Rédaction AVANT journalisation et avant affichage dans le dock.
  //
  // Le message était loggé (400 premiers octets) et surtout émis INTÉGRALEMENT
  // vers un QPlainTextEdit visible du dock. Or `start_program_preview` fait
  // moins de 400 octets et transporte `publishToken` : le jeton de publication
  // WHIP partait donc dans le fichier de log OBS, que les utilisateurs
  // publient en entier sur les canaux de support, et s'affichait à l'écran
  // pendant les partages d'écran et les VOD. `WebUiServer` prend déjà cette
  // précaution pour son propre jeton ; ce chemin ne le faisait pas.
  const QString redacted = redactSecrets(text);
  const QByteArray preview = redacted.toUtf8().left(400);
  blog(LOG_INFO, "[semirprod-ws] <- %s", preview.constData());
  emit wsTraffic(QStringLiteral("in"), redacted);

  const auto json = QJsonDocument::fromJson(text.toUtf8());
  if (!json.isObject()) {
    return;
  }

  const auto obj = json.object();
  const QString type = obj.value(QStringLiteral("type")).toString();

  // Portier : AVANT le moindre effet de bord, et avant meme le filtrage par
  // instance. Un message qui n'est pas pour nous ne doit pas consommer de
  // jetons, mais un rejeu adresse a nous ne doit jamais atteindre un handler.
  if (isMutatingCommand(type) && messageTargetsThisInstance(obj) &&
      !acceptRemoteCommand(type, text)) {
    return;
  }

  if (type == QStringLiteral("hello")) {
    // Paramètres du protocole v2, bornés : une valeur absurde du serveur ne
    // doit pas faire renouveler le jeton toutes les secondes.
    renewBeforeS_ = std::clamp(obj.value(QStringLiteral("renewBeforeS")).toInt(120), 60, 600);
    const double maxPayload = obj.value(QStringLiteral("maxPayloadBytes")).toDouble(0);
    if (maxPayload >= 64 * 1024) maxPayloadBytes_ = static_cast<qint64>(maxPayload);
    return;
  }

  // Résultat de l'annonce du poste v2. La cible mémorisée par le serveur est
  // réconciliée avec celle du poste : le poste fait foi (c'est lui qu'on règle).
  if (type == QStringLiteral("register_instance_result")) {
    // Deux formes acceptees : `status: "ok"` (pont v2 d'origine) et `ok: true`
    // (contrat temps reel du 02/10, qui ajoute l'instantane).
    const bool accepted = obj.value(QStringLiteral("status")).toString() == QStringLiteral("ok") ||
                          obj.value(QStringLiteral("ok")).toBool(false);
    if (!accepted) {
      blog(LOG_WARNING, "[semirprod-ws] poste NON enregistre (%s)",
           obj.value(QStringLiteral("error")).toString().toUtf8().constData());
      return;
    }
    registered_ = true;
    // Nouvelle session cote pont : il ne connait plus rien de l'etat du poste.
    // Les empreintes sont oubliees pour que le prochain envoi soit complet.
    stateFingerprints_.clear();
    blog(LOG_INFO, "[semirprod-ws] poste enregistre : il apparait dans le dashboard");
    emit registrationChanged(true);
    // Instantane (acces, fonctions, flux, identite) : le dock l'applique au
    // lieu de rappeler les routes HTTP. Emis AVANT la reconciliation de la
    // cible, qui peut sortir plus bas.
    emit registrationResult(obj);
    const QJsonObject target = obj.value(QStringLiteral("target")).toObject();
    const QString serverTarget = target.value(QStringLiteral("streamId")).toString();
    if (serverTarget != registeredStreamId_) {
      sendSetTarget();
      return;
    }
    if (!serverTarget.isEmpty()) {
      emit streamRegistered(serverTarget, true);
      emit publishStateChanged(serverTarget, target.value(QStringLiteral("publishing")).toBool());
    }
    return;
  }

  if (type == QStringLiteral("set_target_result")) {
    const bool ok = obj.value(QStringLiteral("status")).toString() == QStringLiteral("ok");
    const QString streamId = obj.value(QStringLiteral("streamId")).toString();
    if (ok) {
      if (!streamId.isEmpty()) {
        emit streamRegistered(streamId, true);
        emit publishStateChanged(streamId, obj.value(QStringLiteral("publishing")).toBool());
      }
      emit statusMessage(streamId.isEmpty()
                             ? QStringLiteral("Aucun flux visé.")
                             : QStringLiteral("Flux live enregistré côté Semirprod."));
    } else {
      emit streamRegistered(registeredStreamId_, false);
      emit statusMessage(QStringLiteral("Ce flux n'existe plus dans ton espace Semirprod. Choisis un flux."));
    }
    return;
  }

  if (type == QStringLiteral("auth_renew_result")) {
    if (obj.value(QStringLiteral("status")).toString() != QStringLiteral("ok")) {
      blog(LOG_WARNING, "[semirprod-ws] renouvellement du jeton refuse : nouvelle session demandee");
      emit tokenRenewalNeeded();
    }
    return;
  }

  if (type == QStringLiteral("server_restarting")) {
    restartRetryMs_ = std::clamp(obj.value(QStringLiteral("retryInMs")).toInt(1000), 0, 10000);
    return;
  }

  if (type == QStringLiteral("error")) {
    const QString err = obj.value(QStringLiteral("error")).toString();
    if (err == QStringLiteral("device_revoked")) {
      refused_ = true;
      emit sessionRefused(err);
    } else {
      blog(LOG_WARNING, "[semirprod-ws] message refuse par le serveur : %s",
           err.toUtf8().constData());
    }
    return;
  }

  if (type == QStringLiteral("publish_state")) {
    const QString streamId = obj.value(QStringLiteral("streamId")).toString();
    const bool publishing = obj.value(QStringLiteral("publishing")).toBool();
    if (!streamId.isEmpty()) emit publishStateChanged(streamId, publishing);
    // Cle de diffusion regeneree sur le site : l'URL de lecture de la source
    // change avec elle, la liste des flux est relue.
    if (obj.value(QStringLiteral("reason")).toString() == QStringLiteral("stream_key_rotated")) {
      emit streamsChanged();
    }
    return;
  }

  if (type == QStringLiteral("quality_state")) {
    const QString streamId = obj.value(QStringLiteral("streamId")).toString();
    const QString state = obj.value(QStringLiteral("state")).toString();
    const QString reason = obj.value(QStringLiteral("reason")).toString();
    const int bitrateKbps = obj.value(QStringLiteral("bitrateKbps")).toInt();
    if (!streamId.isEmpty() && (state == QStringLiteral("ok") || state == QStringLiteral("degraded"))) {
      emit qualityStateChanged(streamId, state == QStringLiteral("ok"), reason, bitrateKbps);
    }
    return;
  }

  if (type == QStringLiteral("switch_scene")) {
    if (!messageTargetsThisInstance(obj)) return;
    const QString requestId = obj.value(QStringLiteral("requestId")).toString();
    const QString streamId = obj.value(QStringLiteral("streamId")).toString();
    const QString target = obj.value(QStringLiteral("target")).toString();
    const QDateTime expiresAt =
        QDateTime::fromString(obj.value(QStringLiteral("expiresAt")).toString(), Qt::ISODate);
    // `streamId` PEUT etre vide : un poste se pilote sans destination (le site
    // ouvre alors le pilotage au niveau de l'organisation). L'exiger faisait ignorer EN
    // SILENCE toutes les bascules de scene d'un poste neuf, un OBS Cloud tout
    // juste attribue par exemple, pendant que volume et mode studio passaient.
    // Il n'est que renvoye dans la reponse.
    if (requestId.isEmpty() || target.isEmpty()) {
      return;
    }
    emit switchSceneRequested(requestId, streamId, target, expiresAt);
    return;
  }

  // `get_preview` voyage avec elles sur le fil sans etre une commande de
  // diffusion : hors de streamControlCommands(), mais classee lourde
  // (isHeavyCommand), donc plafonnee comme les autres captures.
  // Multistream : avant le cas general, qui perdrait l'identifiant de chaine.
  if (type == QStringLiteral("start_multistream") ||
      type == QStringLiteral("stop_multistream")) {
    if (!messageTargetsThisInstance(obj)) return;
    emit multistreamControlRequested(
        obj.value(QStringLiteral("requestId")).toString(), type,
        obj.value(QStringLiteral("destinationId")).toString());
    return;
  }

  // Les chaines du profil (edition regie). Les cles ne sont JAMAIS
  // journalisees, comme pour set_destination.
  if (type == QStringLiteral("set_multistream_destinations")) {
    if (!messageTargetsThisInstance(obj)) return;
    emit multistreamDestinationsRequested(
        obj.value(QStringLiteral("requestId")).toString(),
        obj.value(QStringLiteral("destinations")).toArray());
    return;
  }

  if (streamControlCommands().contains(type) ||
      type == QStringLiteral("get_preview")) {
    if (!messageTargetsThisInstance(obj)) return;
    const QString requestId = obj.value(QStringLiteral("requestId")).toString();
    emit controlRequested(type, requestId);
    return;
  }

  // Destination de diffusion poussée par le site. Le client ne colle plus sa
  // clé de flux à la main dans les réglages d'OBS : elle vit chiffrée en base,
  // et n'existe en clair que le temps de ce message et du réglage qu'il pose.
  //
  // La clé N'EST JAMAIS JOURNALISÉE, ni ici ni plus bas. Le journal d'une
  // instance est lisible par l'exploitation, et une clé de flux dans un journal
  // est une diffusion détournable des mois plus tard.
  if (type == QStringLiteral("set_destination")) {
    if (!messageTargetsThisInstance(obj)) return;
    const QString requestId = obj.value(QStringLiteral("requestId")).toString();
    const QString rtmpUrl = obj.value(QStringLiteral("rtmpUrl")).toString();
    const QString streamKey = obj.value(QStringLiteral("streamKey")).toString();

    // Un message amputé ne doit surtout pas effacer une destination valide en
    // posant deux chaînes vides : le client se retrouverait avec un OBS qui
    // refuse de diffuser sans qu'aucun écran ne l'ait demandé.
    if (rtmpUrl.isEmpty() || streamKey.isEmpty()) {
      sendControlResult(requestId, type, QStringLiteral("failed"),
                        QStringLiteral("destination_incomplete"));
      return;
    }
    if (!isAcceptableDestination(rtmpUrl)) {
      // L'hote seul : le chemin d'une URL d'ingest porte parfois la cle.
      blog(LOG_WARNING,
           "[semirprod-ws] set_destination refuse : schema ou hote non autorise (hote « %s »)",
           QUrl(rtmpUrl).host().toUtf8().constData());
      sendControlResult(requestId, type, QStringLiteral("failed"),
                        QStringLiteral("destination_invalid"));
      return;
    }
    emit destinationRequested(requestId, rtmpUrl, streamKey);
    return;
  }

  // Destination de la sortie verticale. Mêmes règles que set_destination :
  // message amputé refusé, clé jamais journalisée.
  if (type == QStringLiteral("set_vertical_destination")) {
    if (!messageTargetsThisInstance(obj)) return;
    const QString requestId = obj.value(QStringLiteral("requestId")).toString();
    const QString rtmpUrl = obj.value(QStringLiteral("rtmpUrl")).toString();
    const QString streamKey = obj.value(QStringLiteral("streamKey")).toString();
    const bool followStream = obj.value(QStringLiteral("followStream")).toBool(false);
    if (rtmpUrl.isEmpty() || streamKey.isEmpty()) {
      sendControlResult(requestId, type, QStringLiteral("failed"),
                        QStringLiteral("destination_incomplete"));
      return;
    }
    if (!isAcceptableDestination(rtmpUrl)) {
      blog(LOG_WARNING,
           "[semirprod-ws] set_vertical_destination refuse : schema ou hote non autorise "
           "(hote « %s »)",
           QUrl(rtmpUrl).host().toUtf8().constData());
      sendControlResult(requestId, type, QStringLiteral("failed"),
                        QStringLiteral("destination_invalid"));
      return;
    }
    emit verticalDestinationRequested(requestId, rtmpUrl, streamKey, followStream);
    return;
  }

  if (type == QStringLiteral("start_preview")) {
    if (!messageTargetsThisInstance(obj)) return;
    const QString requestId = obj.value(QStringLiteral("requestId")).toString();
    // Defaults are intentionally conservative — if the dashboard misses a
    // field we'd rather under-deliver than blow the bandwidth budget.
    const int fps = obj.value(QStringLiteral("fps")).toInt(2);
    const int quality = obj.value(QStringLiteral("quality")).toInt(75);
    const int width = obj.value(QStringLiteral("width")).toInt(1280);
    const int ttlMs = obj.value(QStringLiteral("ttlMs")).toInt(30000);
    emit previewStreamStartRequested(requestId, fps, quality, width, ttlMs);
    return;
  }

  if (type == QStringLiteral("stop_preview")) {
    if (!messageTargetsThisInstance(obj)) return;
    const QString requestId = obj.value(QStringLiteral("requestId")).toString();
    emit previewStreamStopRequested(requestId);
    return;
  }

  if (type == QStringLiteral("start_program_preview")) {
    if (!messageTargetsThisInstance(obj)) return;
    const QString requestId = obj.value(QStringLiteral("requestId")).toString();
    const QString whipUrl = obj.value(QStringLiteral("whipUrl")).toString();
    const QString publishToken = obj.value(QStringLiteral("publishToken")).toString();
    // Conservative defaults mirror the screenshot path: if the dashboard drops
    // a field we fall back to 720p30 / 2.5 Mbps / 30 s lease rather than
    // over-committing the regie uplink. Les valeurs fournies sont plafonnees
    // (1920x1080, 6000 kbps) pour les quatre apercus : voir
    // ProgramPreviewOutput::clampEncodeParams.
    const int ttlMs = obj.value(QStringLiteral("ttlMs")).toInt(30000);
    int width = obj.value(QStringLiteral("width")).toInt(1280);
    int height = obj.value(QStringLiteral("height")).toInt(720);
    const int fps = obj.value(QStringLiteral("fps")).toInt(30);
    int bitrateKbps = obj.value(QStringLiteral("bitrateKbps")).toInt(2500);
    ProgramPreviewOutput::clampEncodeParams(&width, &height, &bitrateKbps);
    if (whipUrl.isEmpty()) return;

    // `whipUrl` est fourni par le SERVEUR et n'était contrôlé que sur sa
    // vacuité. C'était la seule surface réseau du plugin qui échappait à
    // l'allowlist (`Config::isAllowedHost` est appliqué au HTTP, au WebSocket,
    // au downloadUrl de l'updater et aux URI de pairing) — et celle qui
    // transporte le plus : le PROGRAMME complet, vidéo et audio, en continu.
    //
    // Sans ce contrôle, un backend compromis, un opérateur du dashboard ou un
    // MITM envoyait `{"type":"start_program_preview","whipUrl":"http://…"}` et
    // OBS poussait tout ce qui passe à l'antenne vers l'hôte de son choix, en
    // clair si le schéma était http, sans aucun indicateur à l'écran du
    // streamer. Variante : une URL vers 127.0.0.1 ou une IP privée transformait
    // le plugin en pivot SSRF depuis le poste.
    const QUrl parsedWhip(whipUrl);
    if (parsedWhip.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) != 0 ||
        !Config::isAllowedHostFor(whipUrl, Config::HostUse::Whip)) {
      blog(LOG_ERROR,
           "[semirprod-ws] start_program_preview REFUSE : hote « %s » hors "
           "allowlist (schema « %s »). Autorises : %s. Cote client l'apercu ne "
           "demarre pas et RIEN ne s'affiche : c'est cette ligne, et elle seule, "
           "qui dit pourquoi.",
           parsedWhip.host().toUtf8().constData(),
           parsedWhip.scheme().toUtf8().constData(),
           Config::allowedHostsForLog().toUtf8().constData());
      return;
    }

    emit programPreviewStartRequested(requestId, whipUrl, publishToken, ttlMs,
                                      width, height, fps, bitrateKbps);
    return;
  }

  if (type == QStringLiteral("stop_program_preview")) {
    if (!messageTargetsThisInstance(obj)) return;
    const QString requestId = obj.value(QStringLiteral("requestId")).toString();
    emit programPreviewStopRequested(requestId);
    return;
  }

  // Aperçu WebRTC de la scène de PRÉPARATION (mode studio). Mêmes contrôles que
  // start_program_preview : c'est la même surface réseau, elle transporte cette
  // fois ce qui est en préparation — donc la même allowlist, sans exception.
  if (type == QStringLiteral("start_studio_preview")) {
    if (!messageTargetsThisInstance(obj)) return;
    const QString requestId = obj.value(QStringLiteral("requestId")).toString();
    const QString whipUrl = obj.value(QStringLiteral("whipUrl")).toString();
    const QString publishToken = obj.value(QStringLiteral("publishToken")).toString();
    const int ttlMs = obj.value(QStringLiteral("ttlMs")).toInt(30000);
    int width = obj.value(QStringLiteral("width")).toInt(1280);
    int height = obj.value(QStringLiteral("height")).toInt(720);
    const int fps = obj.value(QStringLiteral("fps")).toInt(30);
    int bitrateKbps = obj.value(QStringLiteral("bitrateKbps")).toInt(2500);
    ProgramPreviewOutput::clampEncodeParams(&width, &height, &bitrateKbps);
    if (whipUrl.isEmpty()) return;

    const QUrl parsedStudioWhip(whipUrl);
    if (parsedStudioWhip.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) != 0 ||
        !Config::isAllowedHostFor(whipUrl, Config::HostUse::Whip)) {
      blog(LOG_ERROR,
           "[semirprod-ws] start_studio_preview refuse : whipUrl hors allowlist");
      return;
    }

    emit studioPreviewStartRequested(requestId, whipUrl, publishToken, ttlMs,
                                     width, height, fps, bitrateKbps);
    return;
  }

  if (type == QStringLiteral("stop_studio_preview")) {
    if (!messageTargetsThisInstance(obj)) return;
    const QString requestId = obj.value(QStringLiteral("requestId")).toString();
    emit studioPreviewStopRequested(requestId);
    return;
  }

  // Aperçu WebRTC du canvas VERTICAL (Semirprod Vertical). Même surface
  // réseau que start_program_preview, cette fois avec l'image 9:16 : même
  // allowlist, sans exception. La sortie vit chez Vertical, le bail ici.
  if (type == QStringLiteral("start_vertical_preview")) {
    if (!messageTargetsThisInstance(obj)) return;
    const QString requestId = obj.value(QStringLiteral("requestId")).toString();
    const QString whipUrl = obj.value(QStringLiteral("whipUrl")).toString();
    const QString publishToken = obj.value(QStringLiteral("publishToken")).toString();
    const int ttlMs = obj.value(QStringLiteral("ttlMs")).toInt(30000);
    int width = obj.value(QStringLiteral("width")).toInt(540);
    int height = obj.value(QStringLiteral("height")).toInt(960);
    const int fps = obj.value(QStringLiteral("fps")).toInt(30);
    int bitrateKbps = obj.value(QStringLiteral("bitrateKbps")).toInt(1000);
    ProgramPreviewOutput::clampEncodeParams(&width, &height, &bitrateKbps);
    if (whipUrl.isEmpty()) return;

    const QUrl parsedVerticalWhip(whipUrl);
    if (parsedVerticalWhip.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) != 0 ||
        !Config::isAllowedHostFor(whipUrl, Config::HostUse::Whip)) {
      blog(LOG_ERROR,
           "[semirprod-ws] start_vertical_preview refuse : whipUrl hors allowlist");
      return;
    }

    emit verticalPreviewStartRequested(requestId, whipUrl, publishToken, ttlMs,
                                       width, height, fps, bitrateKbps);
    return;
  }

  if (type == QStringLiteral("stop_vertical_preview")) {
    if (!messageTargetsThisInstance(obj)) return;
    const QString requestId = obj.value(QStringLiteral("requestId")).toString();
    emit verticalPreviewStopRequested(requestId);
    return;
  }

  // Aperçu WebRTC d'un canvas OBS NOMMÉ (édition régie). Même surface réseau
  // que les autres aperçus : même allowlist, sans exception. Taille par défaut
  // 0×0 : le dock la déduit alors du canvas, pour ne pas écraser un 9:16 en
  // 16:9.
  if (type == QStringLiteral("start_canvas_preview")) {
    if (!messageTargetsThisInstance(obj)) return;
    const QString requestId = obj.value(QStringLiteral("requestId")).toString();
    const QString whipUrl = obj.value(QStringLiteral("whipUrl")).toString();
    const QString publishToken = obj.value(QStringLiteral("publishToken")).toString();
    const QString canvas = obj.value(QStringLiteral("canvas")).toString();
    const int ttlMs = obj.value(QStringLiteral("ttlMs")).toInt(30000);
    int width = obj.value(QStringLiteral("width")).toInt(0);
    int height = obj.value(QStringLiteral("height")).toInt(0);
    const int fps = obj.value(QStringLiteral("fps")).toInt(30);
    int bitrateKbps = obj.value(QStringLiteral("bitrateKbps")).toInt(1500);
    ProgramPreviewOutput::clampEncodeParams(&width, &height, &bitrateKbps);
    if (whipUrl.isEmpty()) return;

    const QUrl parsedCanvasWhip(whipUrl);
    if (parsedCanvasWhip.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) != 0 ||
        !Config::isAllowedHostFor(whipUrl, Config::HostUse::Whip)) {
      blog(LOG_ERROR,
           "[semirprod-ws] start_canvas_preview refuse : whipUrl hors allowlist");
      return;
    }

    emit canvasPreviewStartRequested(requestId, canvas, whipUrl, publishToken,
                                     ttlMs, width, height, fps, bitrateKbps);
    return;
  }

  if (type == QStringLiteral("stop_canvas_preview")) {
    if (!messageTargetsThisInstance(obj)) return;
    const QString requestId = obj.value(QStringLiteral("requestId")).toString();
    emit canvasPreviewStopRequested(requestId);
    return;
  }

  // Création d'une collection de scènes depuis le dashboard (un client par
  // collection, rattachée à son profil). Ni renommage ni suppression.
  if (type == QStringLiteral("create_scene_collection")) {
    if (!messageTargetsThisInstance(obj)) return;
    const QString name = obj.value(QStringLiteral("name")).toString();
    if (name.isEmpty()) return;
    emit createSceneCollectionRequested(obj.value(QStringLiteral("requestId")).toString(),
                                        name);
    return;
  }

  // Pilotage à distance de la configuration du poste : des bascules entre
  // éléments existants (la création de collection est juste au-dessus).
  if (type == QStringLiteral("set_profile") ||
      type == QStringLiteral("set_scene_collection")) {
    if (!messageTargetsThisInstance(obj)) return;
    const QString requestId = obj.value(QStringLiteral("requestId")).toString();
    const QString name = obj.value(QStringLiteral("name")).toString();
    if (name.isEmpty()) return;
    if (type == QStringLiteral("set_profile")) {
      emit setProfileRequested(requestId, name);
    } else {
      emit setSceneCollectionRequested(requestId, name);
    }
    return;
  }

  if (type == QStringLiteral("start_audio_levels")) {
    if (!messageTargetsThisInstance(obj)) return;
    const QString requestId = obj.value(QStringLiteral("requestId")).toString();
    const int fps = obj.value(QStringLiteral("fps")).toInt(10);
    const int ttlMs = obj.value(QStringLiteral("ttlMs")).toInt(15000);
    emit audioLevelsStartRequested(requestId, fps, ttlMs);
    return;
  }

  if (type == QStringLiteral("stop_audio_levels")) {
    if (!messageTargetsThisInstance(obj)) return;
    emit audioLevelsStopRequested(
        obj.value(QStringLiteral("requestId")).toString());
    return;
  }

  if (type == QStringLiteral("set_stream")) {
    if (!messageTargetsThisInstance(obj)) return;
    const QString requestId = obj.value(QStringLiteral("requestId")).toString();
    const QString streamId = obj.value(QStringLiteral("streamId")).toString();
    if (streamId.isEmpty()) return;
    emit setStreamRequested(requestId, streamId);
    return;
  }

  if (type == QStringLiteral("set_scene_mapping")) {
    if (!messageTargetsThisInstance(obj)) return;
    const QString requestId = obj.value(QStringLiteral("requestId")).toString();
    const QString sceneLive = obj.value(QStringLiteral("sceneLive")).toString();
    const QString sceneBug = obj.value(QStringLiteral("sceneBug")).toString();
    // `autoSwitch` absent = « ne touche pas à ce réglage » (-1). Sans ce
    // tri-état, un dashboard qui n'envoie que les deux scènes désarmerait la
    // bascule automatique au passage.
    const int autoSwitch = obj.contains(QStringLiteral("autoSwitch"))
                               ? (obj.value(QStringLiteral("autoSwitch")).toBool() ? 1 : 0)
                               : -1;
    emit setSceneMappingRequested(requestId, sceneLive, sceneBug, autoSwitch);
    return;
  }

  if (type == QStringLiteral("set_studio_mode")) {
    if (!messageTargetsThisInstance(obj)) return;
    const QString requestId = obj.value(QStringLiteral("requestId")).toString();
    emit setStudioModeRequested(requestId,
                                obj.value(QStringLiteral("enabled")).toBool());
    return;
  }

  if (type == QStringLiteral("set_preview_scene")) {
    if (!messageTargetsThisInstance(obj)) return;
    const QString requestId = obj.value(QStringLiteral("requestId")).toString();
    const QString scene = obj.value(QStringLiteral("scene")).toString();
    if (scene.isEmpty()) return;
    emit setPreviewSceneRequested(requestId, scene);
    return;
  }

  if (type == QStringLiteral("trigger_transition")) {
    if (!messageTargetsThisInstance(obj)) return;
    emit triggerTransitionRequested(
        obj.value(QStringLiteral("requestId")).toString());
    return;
  }

  if (type == QStringLiteral("request_state")) {
    if (!messageTargetsThisInstance(obj)) return;
    emit stateRefreshRequested();
    return;
  }

  // `permissions_changed` (canal d'espace ou d'utilisateur) : meme effet,
  // relire l'acces.
  if (type == QStringLiteral("check_studio_access") ||
      type == QStringLiteral("permissions_changed")) {
    emit checkStudioAccessRequested();
    return;
  }

  if (type == QStringLiteral("streams_changed")) {
    emit streamsChanged();
    return;
  }

  if (type == QStringLiteral("stream_deleted")) {
    const QString streamId = obj.value(QStringLiteral("streamId")).toString();
    if (!streamId.isEmpty()) emit streamDeleted(streamId);
    return;
  }

  if (type == QStringLiteral("features_changed")) {
    emit featuresChanged();
    return;
  }

  // `org_renamed {name}` porte le nouveau nom ; `identity_changed` (canal de
  // l'utilisateur) n'en porte pas : le dock relit /api/me. A distinguer de la
  // FERMETURE 4003 `identity_changed`, traitee dans onDisconnected.
  if (type == QStringLiteral("org_renamed")) {
    emit orgRenamed(obj.value(QStringLiteral("name")).toString());
    return;
  }
  if (type == QStringLiteral("identity_changed")) {
    emit identityChanged();
    return;
  }

  if (type == QStringLiteral("instance_renamed")) {
    if (!messageTargetsThisInstance(obj)) return;
    // null ou absent = nom retire cote site : le poste retombe sur son nom
    // local.
    emit instanceRenamed(obj.value(QStringLiteral("label")).toString());
    return;
  }

  // Liste des postes de l'espace : rien a faire cote poste.
  if (type == QStringLiteral("instances_changed")) {
    return;
  }

  if (type == QStringLiteral("get_sources")) {
    if (!messageTargetsThisInstance(obj)) return;
    emit sourcesRefreshRequested();
    return;
  }

  if (type == QStringLiteral("get_audio")) {
    if (!messageTargetsThisInstance(obj)) return;
    emit audioRefreshRequested();
    return;
  }

  if (type == QStringLiteral("set_source_visible") ||
      type == QStringLiteral("set_source_locked")) {
    if (!messageTargetsThisInstance(obj)) return;
    const QString requestId = obj.value(QStringLiteral("requestId")).toString();
    const QString sceneName = obj.value(QStringLiteral("scene")).toString();
    const QString sourceName = obj.value(QStringLiteral("sourceName")).toString();
    const bool flag = obj.value(QStringLiteral("value")).toBool();
    if (sceneName.isEmpty() || sourceName.isEmpty()) return;
    if (type == QStringLiteral("set_source_visible")) {
      emit setSceneItemVisibleRequested(requestId, sceneName, sourceName, flag);
    } else {
      emit setSceneItemLockedRequested(requestId, sceneName, sourceName, flag);
    }
    return;
  }

  if (type == QStringLiteral("set_audio_mute")) {
    if (!messageTargetsThisInstance(obj)) return;
    const QString requestId = obj.value(QStringLiteral("requestId")).toString();
    const QString sourceName = obj.value(QStringLiteral("sourceName")).toString();
    const bool muted = obj.value(QStringLiteral("muted")).toBool();
    if (sourceName.isEmpty()) return;
    emit setSourceMutedRequested(requestId, sourceName, muted);
    return;
  }

  if (type == QStringLiteral("set_audio_volume")) {
    if (!messageTargetsThisInstance(obj)) return;
    const QString requestId = obj.value(QStringLiteral("requestId")).toString();
    const QString sourceName = obj.value(QStringLiteral("sourceName")).toString();
    // toDouble defaults to 0 when absent — we accept it as a valid request
    // (0 dB = unity gain). The plugin clamps to [-100, +26].
    const double volumeDb = obj.value(QStringLiteral("volumeDb")).toDouble();
    if (sourceName.isEmpty()) return;
    emit setSourceVolumeRequested(requestId, sourceName, static_cast<float>(volumeDb));
    return;
  }

  if (type == QStringLiteral("set_audio_monitoring")) {
    if (!messageTargetsThisInstance(obj)) return;
    const QString requestId = obj.value(QStringLiteral("requestId")).toString();
    const QString sourceName = obj.value(QStringLiteral("sourceName")).toString();
    const int monitoringType = obj.value(QStringLiteral("monitoringType")).toInt();
    if (sourceName.isEmpty()) return;
    emit setSourceMonitoringRequested(requestId, sourceName, monitoringType);
    return;
  }
}

} // namespace semirprod
