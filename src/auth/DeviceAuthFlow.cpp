#include "DeviceAuthFlow.hpp"
#include "../Settings.hpp"
#include <QUuid>
#include <QFileInfo>
#include <QCryptographicHash>

#include "../Config.hpp"
#include "../http/HttpClient.hpp"
#include "JwtClaims.hpp"

#include <algorithm>
#include <memory>

#include <QDateTime>
#include <QHostInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSysInfo>
#include <QTimer>
#include <QUrlQuery>

#include <obs-module.h>
#include "../Locale.hpp"

namespace semirprod {

namespace {
constexpr int kMinPollIntervalSeconds = 1;
constexpr int kMaxPollIntervalSeconds = 60;

// Reject user codes that don't match the OAuth Device Code alphabet (RFC 8628
// §6.1, "no ambiguous characters"). Allows letters, digits, and a couple of
// separators but nothing that a phishing page would try to inject via JSON.
bool isPlausibleUserCode(const QString& code) {
  if (code.size() < 4 || code.size() > 64) return false;
  for (QChar c : code) {
    const ushort u = c.unicode();
    const bool ok =
        (u >= 'A' && u <= 'Z') ||
        (u >= 'a' && u <= 'z') ||
        (u >= '0' && u <= '9') ||
        u == '-' || u == '_' || u == ' ' || u == '.' || u == ':';
    if (!ok) return false;
  }
  return true;
}

// Pairing URIs MUST point at our auth host. We accept the same allowlist
// Config uses — auth.semirprod.fr in prod, plus loopback/.local in dev.
bool isAllowedPairingUri(const QString& uri) {
  if (uri.isEmpty()) return false;
  if (!uri.startsWith(QStringLiteral("https://"))) return false;
  return Config::isAllowedHost(uri);
}

} // namespace

DeviceAuthFlow::DeviceAuthFlow(HttpClient* http, QObject* parent)
    : QObject(parent), http_(http), pollTimer_(new QTimer(this)) {
  pollTimer_->setSingleShot(true);
  connect(pollTimer_, &QTimer::timeout, this, &DeviceAuthFlow::poll);
}

void DeviceAuthFlow::start() {
  cancel();
  running_ = true;
  emit pollingChanged(true);
  requestDeviceCode();
}

void DeviceAuthFlow::cancel() {
  // Toute reponse encore en vol devient perimee (voir generation_).
  ++generation_;
  refreshInFlight_ = false;
  pollTimer_->stop();
  session_ = {};
  deviceCode_.clear();
  intervalSeconds_ = 5;
  pendingRefreshToken_.clear();
  if (running_) {
    running_ = false;
    emit pollingChanged(false);
  }
}

QString DeviceAuthFlow::deviceLabel() {
  // Surface enough context that the user can recognize their own pairing
  // request on the consent page — useful when several streamers share
  // a streaming PC. Hostname + OS gives "STREAM-PC-1 (Windows 11)".
  const QString host = QHostInfo::localHostName();
  const QString os = QSysInfo::prettyProductName();
  if (host.isEmpty()) return os;
  if (os.isEmpty()) return host;
  return QStringLiteral("%1 (%2) · Semirprod for OBS %3")
      .arg(host, os, QStringLiteral(SEMIRPROD_PLUGIN_VERSION));
}

// Cle de machine, au format UUID qu'attend le serveur.
//
// Ni le nom d'hote (un PC se renomme) ni l'UUID de la config OBS : la
// desinstallation propose d'effacer ce dossier, et la cle repartirait neuve
// au moment precis ou elle doit reconnaitre le PC. On hache l'identifiant de
// la machine (MachineGuid sous Windows) avec le dossier de config du module :
// stable a la mise a jour, a la reinstallation et au renommage, et distinct
// pour deux OBS portables du meme PC. Hache pour ne pas envoyer l'identifiant
// brut de la machine. Repli sur l'UUID de config si la machine n'en donne pas.
QString DeviceAuthFlow::machineKey() {
  const QByteArray machine = QSysInfo::machineUniqueId();
  if (machine.isEmpty()) return Settings().instanceId();
  const QByteArray configDir = QFileInfo(Settings::configFilePath()).absolutePath().toUtf8();
  const QByteArray digest = QCryptographicHash::hash(
      QByteArrayLiteral("semirprod-machine-key-v1|") + machine + '|' + configDir,
      QCryptographicHash::Sha256);
  return QUuid::fromRfc4122(digest.left(16)).toString(QUuid::WithoutBraces);
}

void DeviceAuthFlow::requestDeviceCode() {
  QJsonObject body;
  body[QStringLiteral("client_id")] = QString::fromLatin1(Config::clientId);
  body[QStringLiteral("scope")] = QString::fromLatin1(Config::scope);
  body[QStringLiteral("device_label")] = deviceLabel();
  // Recopiés sur l'appareil à l'approbation : visibles dans le dashboard avant
  // même la première connexion.
  body[QStringLiteral("client_version")] = QStringLiteral(SEMIRPROD_PLUGIN_VERSION);
  body[QStringLiteral("platform")] = platformName();
  // Reconnait le PC au reappairage : le serveur retire l'appareil precedent
  // qui porte la meme cle, pour ce compte et cet espace.
  body[QStringLiteral("machine_key")] = machineKey();

  const quint64 generation = generation_;
  http_->postJson(
      Config::authBaseUrl() + QStringLiteral("/api/auth/obs/device/code"),
      body,
      QString(),
      [this, generation](const HttpResponse& response, const QJsonDocument& json) {
        if (generation != generation_) return;  // annule entre-temps
        if (!response.ok() || !json.isObject()) {
          blog(LOG_WARNING,
               "[semirprod-device] /device/code failed: status=%d body=%.*s",
               response.statusCode,
               static_cast<int>(std::min<qsizetype>(response.body.size(), 400)),
               response.body.constData());
          // Surface the real failure to the dock so the user can tell
          // a deployment miss (404) from a network outage from a server
          // 500 — saves a round-trip to the OBS log.
          QString detail;
          if (response.networkError != QNetworkReply::NoError) {
            detail = response.errorString;
          } else {
            const QString bodyPreview = QString::fromUtf8(response.body.left(300));
            detail = QStringLiteral("HTTP %1%2")
                         .arg(response.statusCode)
                         .arg(bodyPreview.isEmpty() ? QString() : QStringLiteral(" : %1").arg(bodyPreview));
          }
          emit deviceFlowFailed(loc("Auth.StartFailed").arg(detail));
          cancel();
          return;
        }

        const auto obj = json.object();
        deviceCode_ = obj.value(QStringLiteral("device_code")).toString();
        const QString userCode = obj.value(QStringLiteral("user_code")).toString();
        const QString verificationUri = obj.value(QStringLiteral("verification_uri")).toString();
        const QString verificationUriComplete =
            obj.value(QStringLiteral("verification_uri_complete")).toString();
        const int expiresIn = obj.value(QStringLiteral("expires_in")).toInt(1800);
        const int interval = obj.value(QStringLiteral("interval")).toInt(5);

        if (deviceCode_.isEmpty() || userCode.isEmpty() || verificationUriComplete.isEmpty()) {
          emit deviceFlowFailed(loc("Auth.IncompletePairing"));
          cancel();
          return;
        }

        // Validate that the URLs the server handed us actually point at the
        // Semirprod auth surface. Without this check a compromised or
        // maliciously-replaced auth endpoint could send the user to a phishing
        // page that asks them to type the user_code and then harvests the
        // resulting browser session. The verification URI MUST be HTTPS and
        // MUST be on the allowlist (typically auth.semirprod.fr).
        if (!isAllowedPairingUri(verificationUri) ||
            !isAllowedPairingUri(verificationUriComplete)) {
          blog(LOG_ERROR,
               "[semirprod-device] rejected /device/code response — "
               "verification_uri not on allowlist: base=%s complete=%s",
               verificationUri.toUtf8().constData(),
               verificationUriComplete.toUtf8().constData());
          emit deviceFlowFailed(loc("Auth.BadUrl"));
          cancel();
          return;
        }

        // Reject user_code values that look malformed. RFC 8628 §6.1 doesn't
        // pin a charset but the spec recommends excluding easily-confused
        // chars (0/O, 1/I/l). Most auth servers emit 8-12 chars from a sane
        // alphabet; anything outside that is either broken or hostile.
        if (!isPlausibleUserCode(userCode)) {
          emit deviceFlowFailed(loc("Auth.BadCode"));
          cancel();
          return;
        }

        session_.verificationUri = verificationUriComplete;
        session_.verificationUriBase = verificationUri;
        session_.userCode = userCode;
        session_.expiresAtMsSinceEpoch =
            QDateTime::currentMSecsSinceEpoch() + static_cast<qint64>(expiresIn) * 1000;
        intervalSeconds_ =
            std::clamp(interval, kMinPollIntervalSeconds, kMaxPollIntervalSeconds);

        emit deviceFlowStarted(session_);
        schedulePoll(intervalSeconds_);
      });
}

void DeviceAuthFlow::schedulePoll(int delaySeconds) {
  if (deviceCode_.isEmpty()) return;
  if (QDateTime::currentMSecsSinceEpoch() > session_.expiresAtMsSinceEpoch) {
    emit deviceFlowFailed(loc("Auth.CodeExpired"));
    cancel();
    return;
  }
  const int clamped = std::clamp(delaySeconds, kMinPollIntervalSeconds, kMaxPollIntervalSeconds);
  pollTimer_->start(clamped * 1000);
}

void DeviceAuthFlow::poll() {
  if (deviceCode_.isEmpty()) return;

  QJsonObject body;
  body[QStringLiteral("grant_type")] = QStringLiteral("urn:ietf:params:oauth:grant-type:device_code");
  body[QStringLiteral("device_code")] = deviceCode_;
  body[QStringLiteral("client_id")] = QString::fromLatin1(Config::clientId);

  const quint64 generation = generation_;
  http_->postJson(
      Config::authBaseUrl() + QStringLiteral("/api/auth/obs/device/token"),
      body,
      QString(),
      [this, generation](const HttpResponse& response, const QJsonDocument& json) {
        // Appairage annule (ou relance) pendant la requete : rien a enregistrer.
        if (generation != generation_) return;
        if (response.statusCode == 200 && json.isObject()) {
          const auto obj = json.object();
          const QString accessToken = obj.value(QStringLiteral("access_token")).toString();
          const QString refreshToken = obj.value(QStringLiteral("refresh_token")).toString();
          const int expiresIn = obj.value(QStringLiteral("expires_in")).toInt(3600);
          const QJsonObject device = obj.value(QStringLiteral("device")).toObject();
          const QJsonObject org = device.value(QStringLiteral("organization")).toObject();
          if (accessToken.isEmpty()) {
            emit deviceFlowFailed(loc("Auth.IncompleteOAuth"));
            cancel();
            return;
          }
          // Le secret de l'appareil n'est rendu qu'UNE fois, ici. Sans lui, le
          // serveur n'est pas à jour : aucune session ne pourra s'ouvrir.
          if (device.value(QStringLiteral("id")).toString().isEmpty() ||
              device.value(QStringLiteral("secret")).toString().isEmpty()) {
            emit deviceFlowFailed(loc("Auth.DeviceNotCreated"));
            cancel();
            return;
          }
          TokenBundle binding;
          binding.oauthAccessToken = accessToken;
          binding.oauthExpiresAt = QDateTime::currentDateTimeUtc().addSecs(expiresIn);
          binding.refreshToken = refreshToken;
          binding.deviceId = device.value(QStringLiteral("id")).toString();
          binding.deviceSecret = device.value(QStringLiteral("secret")).toString();
          binding.orgId = org.value(QStringLiteral("id")).toString();
          binding.orgName = org.value(QStringLiteral("name")).toString();
          binding.orgSlug = org.value(QStringLiteral("slug")).toString();
          openDeviceSession(binding, /*oauthJustRefreshed=*/true, generation);
          return;
        }

        // RFC 8628 §3.5 polling errors — only authorization_pending and
        // slow_down are recoverable.
        const QString error = json.isObject()
            ? json.object().value(QStringLiteral("error")).toString()
            : QString();
        if (error == QStringLiteral("authorization_pending")) {
          schedulePoll(intervalSeconds_);
          return;
        }
        if (error == QStringLiteral("slow_down")) {
          // Server may suggest the new interval explicitly.
          int suggested = json.object().value(QStringLiteral("interval")).toInt(0);
          if (suggested > 0) {
            intervalSeconds_ = std::clamp(suggested, kMinPollIntervalSeconds, kMaxPollIntervalSeconds);
          } else {
            intervalSeconds_ = std::min(intervalSeconds_ + 5, kMaxPollIntervalSeconds);
          }
          schedulePoll(intervalSeconds_);
          return;
        }
        if (error == QStringLiteral("access_denied")) {
          emit deviceFlowFailed(loc("Auth.Denied"));
          cancel();
          return;
        }
        if (error == QStringLiteral("expired_token")) {
          emit deviceFlowFailed(loc("Auth.CodeExpired"));
          cancel();
          return;
        }
        if (error == QStringLiteral("invalid_grant") || error == QStringLiteral("invalid_client")) {
          emit deviceFlowFailed(loc("Auth.Rejected"));
          cancel();
          return;
        }

        // Unknown / network error — don't burn the pairing, retry once
        // at the current interval. If the network is just flaky we'll
        // recover on the next tick; if it's a real server bug the
        // code's 30-min expiry will eventually bail us out.
        blog(LOG_WARNING,
             "[semirprod-device] /device/token unexpected response: status=%d error=\"%s\"",
             response.statusCode, error.toUtf8().constData());
        schedulePoll(intervalSeconds_);
      });
}

// Le serveur a-t-il REFUSE la session, ou n'a-t-il simplement pas repondu ?
//
// Un jeton de rafraichissement n'est mort que si le point d'entree OAuth le DIT :
// 4xx avec un code `error` de la RFC 6749 §5.2 (`invalid_grant` quand il est
// revoque ou expire, `invalid_client` s'il appartient a un autre client).
//
// Tout le reste est passager, 4xx sans ce code compris : un proxy ou un pod en
// plein deploiement peut repondre 400 ou 403 sans rapport avec le jeton. Effacer
// la liaison sur un simple statut obligeait a refaire l'appairage de tout le parc.
static bool isDefinitiveRefreshRefusal(const HttpResponse& response, const QJsonDocument& json) {
  if (response.statusCode < 400 || response.statusCode >= 500 || !json.isObject()) return false;
  const QString code = json.object().value(QStringLiteral("error")).toString();
  return code == QStringLiteral("invalid_grant") || code == QStringLiteral("invalid_client");
}

namespace {

// Texte affiché pour un refus DÉFINITIF de l'échange de session. Une phrase,
// un geste : l'état terminal ne boucle pas, il attend que la personne agisse.
QString terminalRefusalText(const QString& code) {
  if (code == QStringLiteral("device_revoked"))
    return loc("Auth.DeviceRevoked");
  if (code == QStringLiteral("not_member"))
    return loc("Auth.NotMember");
  if (code == QStringLiteral("device_credential_invalid"))
    return loc("Auth.CredentialInvalid");
  if (code == QStringLiteral("update_required"))
    return loc("Auth.UpdateRequired");
  return loc("Auth.SessionExpired");
}

bool isTerminalSessionCode(const QString& code) {
  return code == QStringLiteral("device_revoked") ||
         code == QStringLiteral("not_member") ||
         code == QStringLiteral("device_credential_invalid") ||
         code == QStringLiteral("invalid_client") ||
         code == QStringLiteral("missing_scope") ||
         code == QStringLiteral("update_required");
}

}  // namespace

QString DeviceAuthFlow::platformName() {
#if defined(Q_OS_WIN)
  return QStringLiteral("windows");
#elif defined(Q_OS_MACOS)
  return QStringLiteral("macos");
#else
  return QStringLiteral("linux");
#endif
}

void DeviceAuthFlow::refresh(const TokenBundle& bundle) {
  // UN renouvellement a la fois. Le suivant n'annule plus celui en vol (ce
  // qui jetait sa reponse et refaisait l'aller-retour) : il attend son
  // resultat, livre a tous par deviceFlowSucceeded / deviceFlowFailed.
  if (refreshInFlight_) {
    blog(LOG_INFO, "[semirprod-auth] renouvellement deja en cours : on attend son resultat");
    return;
  }
  cancel();
  if (!bundle.isBound()) {
    emit sessionRevoked(terminalRefusalText(QStringLiteral("device_credential_invalid")));
    return;
  }
  // Remis a faux par cancel(), que chaque fin de renouvellement appelle.
  refreshInFlight_ = true;
  running_ = true;
  emit pollingChanged(true);
  pairedOrgName_ = bundle.orgName;
  pairedOrgSlug_ = bundle.orgSlug;
  if (bundle.oauthUsable()) {
    openDeviceSession(bundle, /*oauthJustRefreshed=*/false, generation_);
    return;
  }
  if (bundle.refreshToken.isEmpty()) {
    emit sessionRevoked(terminalRefusalText(QString()));
    cancel();
    return;
  }
  refreshAccessToken(bundle);
}

void DeviceAuthFlow::refreshAccessToken(TokenBundle binding) {
  pendingRefreshToken_ = binding.refreshToken;

  QUrlQuery form;
  form.addQueryItem(QStringLiteral("grant_type"), QStringLiteral("refresh_token"));
  form.addQueryItem(QStringLiteral("client_id"), QString::fromLatin1(Config::clientId));
  form.addQueryItem(QStringLiteral("refresh_token"), binding.refreshToken);

  const quint64 generation = generation_;
  http_->postForm(
      Config::authBaseUrl() + QStringLiteral("/api/auth/oauth2/token"),
      form,
      {},
      [this, binding, generation](const HttpResponse& response,
                                  const QJsonDocument& json) mutable {
        // Annule entre-temps (deconnexion, nouvel appairage) : la reponse ne
        // doit rien enregistrer, sinon elle recreerait la liaison effacee.
        if (generation != generation_) return;
        if (!response.ok() || !json.isObject()) {
          if (isDefinitiveRefreshRefusal(response, json)) {
            blog(LOG_WARNING,
                 "[semirprod-auth] refresh refuse definitivement (HTTP %d) — session perdue",
                 response.statusCode);
            emit sessionRevoked(terminalRefusalText(QString()));
            cancel();
            return;
          }
          const QString detail = response.networkError != QNetworkReply::NoError
              ? response.errorString
              : QStringLiteral("HTTP %1").arg(response.statusCode);
          emit deviceFlowFailed(
              loc("Auth.RenewFailed").arg(detail));
          cancel();
          return;
        }

        const auto obj = json.object();
        const QString oauthAccessToken = obj.value(QStringLiteral("access_token")).toString();
        if (oauthAccessToken.isEmpty()) {
          emit deviceFlowFailed(loc("Auth.IncompleteRefresh"));
          cancel();
          return;
        }
        binding.oauthAccessToken = oauthAccessToken;
        binding.oauthExpiresAt = QDateTime::currentDateTimeUtc().addSecs(
            obj.value(QStringLiteral("expires_in")).toInt(3600));
        binding.refreshToken =
            obj.value(QStringLiteral("refresh_token")).toString(pendingRefreshToken_);
        emit refreshRotated(binding);
        openDeviceSession(binding, /*oauthJustRefreshed=*/true, generation);
      });
}

void DeviceAuthFlow::openDeviceSession(TokenBundle binding, bool oauthJustRefreshed,
                                       quint64 generation) {
  // Generation de l'appelant, pas la courante : une suite d'une requete
  // perimee ne doit pas se faire passer pour une requete fraiche.
  if (generation != generation_) return;
  QJsonObject body;
  body[QStringLiteral("deviceId")] = binding.deviceId;
  body[QStringLiteral("deviceSecret")] = binding.deviceSecret;
  body[QStringLiteral("clientVersion")] = QStringLiteral(SEMIRPROD_PLUGIN_VERSION);
  body[QStringLiteral("platform")] = platformName();

  http_->postJson(
      Config::authBaseUrl() + QStringLiteral("/api/auth/device/session"),
      body,
      binding.oauthAccessToken,
      [this, binding, oauthJustRefreshed, generation](const HttpResponse& response,
                                                      const QJsonDocument& json) mutable {
        if (generation != generation_) return;  // annule entre-temps
        const QJsonObject obj = json.isObject() ? json.object() : QJsonObject{};
        if (!response.ok()) {
          const QString code = obj.value(QStringLiteral("error")).toString();
          // Jeton OAuth refusé alors qu'on ne venait pas de le rafraîchir :
          // expiré plus tôt que prévu (horloge, révocation de session). Un
          // seul rafraîchissement, jamais une boucle.
          if (!oauthJustRefreshed &&
              (code == QStringLiteral("invalid_access_token") ||
               code == QStringLiteral("expired_access_token")) &&
              !binding.refreshToken.isEmpty()) {
            refreshAccessToken(binding);
            return;
          }
          if (code == QStringLiteral("plugin_blocked")) {
            blog(LOG_WARNING, "[semirprod-auth] version du plugin bloquee par le serveur");
            emit pluginBlocked();
            cancel();
            return;
          }
          // Seul un code explicite du serveur est definitif, jamais un statut
          // seul : le prochain essai repart du jeton enregistre par `refreshRotated`.
          if (isTerminalSessionCode(code)) {
            blog(LOG_WARNING, "[semirprod-auth] session d'appareil refusee : %s (HTTP %d)",
                 code.toUtf8().constData(), response.statusCode);
            emit sessionRevoked(terminalRefusalText(code));
            cancel();
            return;
          }
          const QString detail = response.networkError != QNetworkReply::NoError
              ? response.errorString
              : QStringLiteral("HTTP %1").arg(response.statusCode);
          emit deviceFlowFailed(
              loc("Auth.OpenFailed").arg(detail));
          cancel();
          return;
        }

        const QString jwt = obj.value(QStringLiteral("token")).toString();
        const JwtClaims claims = JwtClaims::parse(jwt);
        if (jwt.isEmpty() || !claims.valid() || claims.deviceId != binding.deviceId) {
          emit deviceFlowFailed(loc("Auth.IncompleteSession"));
          cancel();
          return;
        }
        binding.accessToken = jwt;
        binding.expiresAt = claims.expiresAt;
        binding.orgId = claims.orgId;
        if (binding.orgName.isEmpty()) binding.orgName = pairedOrgName_;
        if (binding.orgSlug.isEmpty()) binding.orgSlug = pairedOrgSlug_;

        emit deviceFlowSucceeded(binding);
        cancel();
      });
}

void DeviceAuthFlow::release(const TokenBundle& bundle, std::function<void()> done) {
  if (!bundle.isBound() || bundle.oauthAccessToken.isEmpty()) {
    if (done) done();
    return;
  }
  // Rappel unique : la réponse ou le délai, le premier des deux.
  auto fired = std::make_shared<bool>(false);
  auto finish = [fired, done]() {
    if (*fired) return;
    *fired = true;
    if (done) done();
  };
  QTimer::singleShot(3000, this, finish);

  QJsonObject body;
  body[QStringLiteral("deviceId")] = bundle.deviceId;
  body[QStringLiteral("deviceSecret")] = bundle.deviceSecret;
  http_->postJson(
      Config::authBaseUrl() + QStringLiteral("/api/auth/device/release"),
      body,
      bundle.oauthAccessToken,
      [finish](const HttpResponse& response, const QJsonDocument&) {
        if (!response.ok()) {
          blog(LOG_WARNING, "[semirprod-auth] retrait du poste non confirme (HTTP %d)",
               response.statusCode);
        }
        finish();
      });
}

} // namespace semirprod
