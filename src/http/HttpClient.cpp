#include "HttpClient.hpp"
#include "../Config.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QUrl>

#include <obs-module.h>

#include <memory>
#include "../Locale.hpp"

namespace semirprod {

namespace {

// Centralized URL sanitizer. Catches three attack classes:
//   1. non-HTTPS scheme (http://, file://, etc.) — would leak bearer tokens.
//   2. host outside the Semirprod allowlist (in release builds) — would
//      redirect to a hostile backend.
//   3. QNetworkRequest silently demotes https → http when redirectToHttps
//      is off (the default); we reject any resolution that didn't keep https.
bool sanitizeUrlOrLog(const QString& url, const char* method) {
  const QUrl parsed(url);
  if (!parsed.isValid()) {
    blog(LOG_WARNING, "[semirprod-http] %s refused: invalid URL %s",
         method, url.toUtf8().constData());
    return false;
  }
  const QString scheme = parsed.scheme().toLower();
  if (scheme != QStringLiteral("https") && scheme != QStringLiteral("wss")) {
    blog(LOG_WARNING, "[semirprod-http] %s refused: non-TLS scheme %s (%s)",
         method, scheme.toUtf8().constData(), url.toUtf8().constData());
    return false;
  }
  if (!Config::isAllowedHost(url)) {
    blog(LOG_WARNING, "[semirprod-http] %s refused: host not in allowlist (%s)",
         method, url.toUtf8().constData());
    return false;
  }
  return true;
}

void logRequest(const char* method, const QString& url) {
  blog(LOG_INFO, "[semirprod-http] -> %s %s", method, url.toUtf8().constData());
}

// `includeBody` : voir HttpClient::setLogResponseBodies. Le corps d'une reponse
// en echec est precieux pour deboguer, sauf quand la requete qui l'a provoquee
// portait un secret : un backend de validation renvoie souvent ce qu'il a recu,
// et le journal d'OBS se recopie dans les tickets de support.
void logResponse(const char* method, const QString& url, const HttpResponse& r,
                 bool includeBody) {
  if (r.networkError != QNetworkReply::NoError || r.statusCode < 200 || r.statusCode >= 300) {
    if (!includeBody) {
      blog(LOG_WARNING,
           "[semirprod-http] <- %s %s status=%d err=%d (%s) body=<omis>",
           method, url.toUtf8().constData(), r.statusCode,
           static_cast<int>(r.networkError),
           r.errorString.toUtf8().constData());
      return;
    }
    const QByteArray preview = r.body.left(400);
    blog(LOG_WARNING,
         "[semirprod-http] <- %s %s status=%d err=%d (%s) body=%s",
         method, url.toUtf8().constData(), r.statusCode,
         static_cast<int>(r.networkError),
         r.errorString.toUtf8().constData(),
         preview.constData());
  } else {
    blog(LOG_INFO, "[semirprod-http] <- %s %s status=%d bytes=%lld",
         method, url.toUtf8().constData(), r.statusCode,
         static_cast<long long>(r.body.size()));
  }
}

// Plafond des reponses JSON (API, manifeste de mise a jour, liste des
// sauvegardes). Les plus grosses font quelques dizaines de kilooctets ; sans
// borne, un serveur hostile ou detourne pouvait faire grossir la memoire d'OBS
// jusqu'a l'arret, `readAll()` gardant tout le corps.
constexpr qint64 kMaxJsonResponseBytes = 8 * 1024 * 1024;

// Coupe-circuit de taille : abort() des que le compteur depasse la borne, sans
// attendre la fin du transfert. Content-Length peut mentir ou manquer, donc on
// surveille l'octet recu, pas seulement l'annonce. Le `finished` qui suit
// porte une erreur (OperationCanceledError) et un corps tronque, borne.
void capResponseSize(QNetworkReply* reply, qint64 maxBytes, const char* method,
                     const QString& url) {
  QObject::connect(reply, &QNetworkReply::downloadProgress, reply,
                   [reply, maxBytes, method, url](qint64 received, qint64 total) {
                     if (received > maxBytes || total > maxBytes) {
                       blog(LOG_WARNING,
                            "[semirprod-http] %s %s abandonne : depasse %lld octets", method,
                            url.toUtf8().constData(), static_cast<long long>(maxBytes));
                       reply->abort();
                     }
                   });
}

// Reponse « refusee avant emission » : rien n'est parti sur le reseau, le
// rappel part de facon synchrone. Le drapeau evite que l'appelant confonde une
// erreur de configuration locale avec un serveur muet et se lance dans une
// reprise qui ne peut rien reparer.
HttpResponse locallyRefused() {
  HttpResponse r;
  r.refusedLocally = true;
  return r;
}

// Build a hardened QNetworkRequest: TLS only, modern cipher preference
// where supported by the backend, no redirect-following downgrade (Qt's
// default behaviour since 5.15 is to refuse http from https anyway, but
// we belt-and-suspender with strict transport security headers if we
// ever want to talk to a HSTS-preloaded endpoint).
QNetworkRequest buildRequest(const QString& url, const QString& bearerToken,
                             const QString& contentType) {
  QNetworkRequest request{QUrl(url)};
  request.setHeader(QNetworkRequest::ContentTypeHeader, contentType);
  request.setRawHeader("User-Agent",
                       QStringLiteral("SemirprodStudio/%1").arg(QStringLiteral(SEMIRPROD_PLUGIN_VERSION)).toUtf8());
  // Langue d'OBS (« fr » ou « en »). Les textes que l'API redige elle-meme (refus
  // d'acces, motifs de sauvegarde) reviennent ainsi dans la langue de l'interface.
  request.setRawHeader("Accept-Language", uiLocaleCode().toUtf8());
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                       QNetworkRequest::NoLessSafeRedirectPolicy);
  // Delai d'INACTIVITE, pas de duree totale : Qt n'interrompt que si RIEN ne
  // circule pendant l'intervalle. Un envoi de plusieurs centaines de megaoctets
  // sur une connexion domestique n'est donc pas coupe, alors qu'un serveur muet
  // rend la main au bout de trente secondes.
  //
  // Sans ce delai, une requete restait pendante indefiniment : le plugin
  // paraissait bloque, l'operateur relancait, et l'ecran ne disait rien de
  // vrai. Constate le 20/08/2026 sur les sauvegardes.
  request.setTransferTimeout(30000);
  if (!bearerToken.isEmpty()) {
    request.setRawHeader("Authorization", QStringLiteral("Bearer %1").arg(bearerToken).toUtf8());
  }
  // Don't follow redirects to non-TLS or non-allowlisted hosts. Qt's
  // NoLessSafeRedirectPolicy already enforces the TLS part; the host check
  // is enforced by sanitizeUrlOrLog() at request build time AND in the
  // redirect slot below — a redirect to http://evil.com is rejected there.
  QSslConfiguration ssl = QSslConfiguration::defaultConfiguration();
  ssl.setPeerVerifyMode(QSslSocket::VerifyPeer);
  request.setSslConfiguration(ssl);
  return request;
}

} // namespace

HttpClient::HttpClient(QObject* parent)
    : QObject(parent), manager_(new QNetworkAccessManager(this)) {
  // The QNetworkAccessManager only enforces TLS on the requests we build,
  // not globally — there's nothing else to lock down at construction time.
}

void HttpClient::attachGuards(QNetworkReply* reply) {
  // 1. SSL errors: never ignore in production. Log and let the reply fail.
  connect(reply, &QNetworkReply::sslErrors, this,
          [reply](const QList<QSslError>& errors) {
            for (const auto& e : errors) {
              blog(LOG_WARNING, "[semirprod-http] SSL error (will fail): %s",
                   e.errorString().toUtf8().constData());
            }
            reply->ignoreSslErrors({}); // explicit no-op list, not "ignore all"
          });

  // 2. Redirect guard. NoLessSafeRedirectPolicy already blocks https->http,
  //    but it still auto-follows https->https to ANY host, and Qt re-sends
  //    the Authorization header across that hop — leaking the bearer to a
  //    redirect target outside our infra. `redirected` fires BEFORE the
  //    follow-up request is dispatched; aborting here stops the bearer from
  //    ever leaving for a host we don't trust.
  //
  //    Requete porteuse du bearer : la redirection doit en plus rester sur le
  //    MEME hote. Un hote seulement « autorise » ne suffit pas : l'allowlist
  //    contient le site vitrine et l'hote live, qui n'ont pas a recevoir le
  //    jeton de l'API, et Qt le recopierait sur le saut.
  const QString originHost = reply->request().url().host().toLower();
  const bool carriesBearer = reply->request().hasRawHeader("Authorization");
  connect(reply, &QNetworkReply::redirected, this,
          [reply, originHost, carriesBearer](const QUrl& target) {
            const QString t = target.toString();
            if (target.scheme().toLower() != QStringLiteral("https") ||
                !Config::isAllowedHost(t)) {
              blog(LOG_WARNING,
                   "[semirprod-http] redirect blocked (scheme/host not allowed): %s "
                   "— aborting before bearer is re-sent",
                   t.toUtf8().constData());
              reply->abort();
              return;
            }
            if (carriesBearer && target.host().toLower() != originHost) {
              blog(LOG_WARNING,
                   "[semirprod-http] redirect blocked (bearer request, other host): %s "
                   "— aborting before bearer is re-sent",
                   t.toUtf8().constData());
              reply->abort();
            }
          });
}

void HttpClient::getJson(const QString& url, const QString& bearerToken, JsonCallback cb) {
  if (!sanitizeUrlOrLog(url, "GET")) {
    if (cb) cb(locallyRefused(), QJsonDocument{});
    return;
  }
  auto request = buildRequest(url, bearerToken, QStringLiteral("application/json"));
  const bool logBody = logResponseBodies_;
  logRequest("GET", url);
  auto* reply = manager_->get(request);
  attachGuards(reply);
  capResponseSize(reply, kMaxJsonResponseBytes, "GET", url);
  connect(reply, &QNetworkReply::finished, this, [reply, url, logBody, cb = std::move(cb)]() mutable {
    HttpResponse response;
    response.statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    response.body = reply->readAll();
    response.networkError = reply->error();
    response.errorString = reply->errorString();

    logResponse("GET", url, response, logBody);
    const auto json = QJsonDocument::fromJson(response.body);
    if (cb) cb(response, json);

    reply->deleteLater();
  });
}

void HttpClient::getBytes(const QString& url, const QString& bearerToken,
                          qint64 maxBytes, BytesCallback cb) {
  if (!sanitizeUrlOrLog(url, "GET")) {
    if (cb) cb(locallyRefused(), QString());
    return;
  }
  // Pas de Content-Type sur une requete GET sans corps ; on annonce en revanche
  // ce qu'on accepte, pour ne pas recevoir une page d'erreur HTML.
  auto request = buildRequest(url, bearerToken, QString());
  request.setRawHeader("Accept", "image/*");
  const bool logBody = logResponseBodies_;
  logRequest("GET", url);
  auto* reply = manager_->get(request);
  attachGuards(reply);

  capResponseSize(reply, maxBytes, "GET", url);

  connect(reply, &QNetworkReply::finished, this, [reply, url, logBody, cb = std::move(cb)]() mutable {
    HttpResponse response;
    response.statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    response.body = reply->readAll();
    response.networkError = reply->error();
    response.errorString = reply->errorString();
    const QString contentType =
        reply->header(QNetworkRequest::ContentTypeHeader).toString();

    logResponse("GET", url, response, logBody);
    if (cb) cb(response, contentType);

    reply->deleteLater();
  });
}

void HttpClient::postJson(const QString& url, const QJsonObject& payload, const QString& bearerToken, JsonCallback cb) {
  if (!sanitizeUrlOrLog(url, "POST")) {
    if (cb) cb(locallyRefused(), QJsonDocument{});
    return;
  }
  auto request = buildRequest(url, bearerToken, QStringLiteral("application/json"));
  const bool logBody = logResponseBodies_;
  logRequest("POST", url);
  auto* reply = manager_->post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
  attachGuards(reply);
  capResponseSize(reply, kMaxJsonResponseBytes, "POST", url);
  connect(reply, &QNetworkReply::finished, this, [reply, url, logBody, cb = std::move(cb)]() mutable {
    HttpResponse response;
    response.statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    response.body = reply->readAll();
    response.networkError = reply->error();
    response.errorString = reply->errorString();

    logResponse("POST", url, response, logBody);
    const auto json = QJsonDocument::fromJson(response.body);
    if (cb) cb(response, json);

    reply->deleteLater();
  });
}

void HttpClient::getToFile(const QString& url, const QString& bearerToken,
                           const QString& destPath, const QString& expectedSha256,
                           qint64 maxBytes, ProgressCallback onProgress, FileCallback cb) {
  if (!sanitizeUrlOrLog(url, "GET(file)")) {
    if (cb) cb(locallyRefused(), QString());
    return;
  }

  QDir().mkpath(QFileInfo(destPath).absolutePath());
  auto* file = new QFile(destPath);
  // ExistingOnly : le fichier a ete reserve par l'appelant (voir l'en-tete).
  if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::ExistingOnly)) {
    blog(LOG_WARNING, "[semirprod-http] GET(file) refuse : ecriture impossible %s",
         destPath.toUtf8().constData());
    delete file;
    if (cb) cb(locallyRefused(), QString());
    return;
  }

  auto request = buildRequest(url, bearerToken, QString());
  const bool logBody = logResponseBodies_;
  logRequest("GET(file)", url);
  auto* reply = manager_->get(request);
  attachGuards(reply);
  file->setParent(reply);

  auto hash = std::make_shared<QCryptographicHash>(QCryptographicHash::Sha256);
  auto received = std::make_shared<qint64>(0);

  // Ecriture au fil de l'eau : rien n'est garde en memoire, et l'empreinte se
  // calcule sur les octets qui partent effectivement sur le disque.
  connect(reply, &QNetworkReply::readyRead, reply, [reply, file, hash, received, maxBytes, url]() {
    const QByteArray chunk = reply->readAll();
    if (chunk.isEmpty()) return;
    *received += chunk.size();
    if (*received > maxBytes) {
      blog(LOG_WARNING, "[semirprod-http] GET(file) %s abandonne : depasse %lld octets",
           url.toUtf8().constData(), static_cast<long long>(maxBytes));
      reply->abort();
      return;
    }
    hash->addData(chunk);
    file->write(chunk);
  });

  if (onProgress) {
    connect(reply, &QNetworkReply::downloadProgress, reply,
            [onProgress](qint64 done, qint64 total) { onProgress(done, total); });
  }

  connect(reply, &QNetworkReply::finished, this,
          [reply, file, hash, expectedSha256, destPath, url, logBody, cb = std::move(cb)]() mutable {
            HttpResponse response;
            response.statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            response.networkError = reply->error();
            response.errorString = reply->errorString();
            // Le corps est deja sur le disque : ne rien remettre dans
            // `response.body`, sinon on annule tout l'interet du flux.
            file->flush();
            file->close();

            logResponse("GET(file)", url, response, logBody);

            const QString digest = QString::fromUtf8(hash->result().toHex());
            const bool empreinteOk =
                expectedSha256.isEmpty() || digest.compare(expectedSha256, Qt::CaseInsensitive) == 0;

            if (!response.ok() || !empreinteOk) {
              if (!empreinteOk && response.ok()) {
                blog(LOG_WARNING,
                     "[semirprod-http] GET(file) %s : empreinte inattendue, fichier efface",
                     url.toUtf8().constData());
                response.statusCode = 0;
                response.errorString =
                    loc("Http.HashMismatch");
              }
              // Un fichier partiel ou altere ne reste PAS sur le disque de
              // l'operateur : il le prendrait pour une sauvegarde valide.
              QFile::remove(destPath);
              if (cb) cb(response, QString());
            } else if (cb) {
              cb(response, destPath);
            }

            reply->deleteLater();
          });
}

void HttpClient::deleteJson(const QString& url, const QString& bearerToken, JsonCallback cb) {
  if (!sanitizeUrlOrLog(url, "DELETE")) {
    if (cb) cb(locallyRefused(), QJsonDocument{});
    return;
  }
  auto request = buildRequest(url, bearerToken, QStringLiteral("application/json"));
  const bool logBody = logResponseBodies_;
  logRequest("DELETE", url);
  auto* reply = manager_->deleteResource(request);
  attachGuards(reply);
  capResponseSize(reply, kMaxJsonResponseBytes, "DELETE", url);
  connect(reply, &QNetworkReply::finished, this,
          [reply, url, logBody, cb = std::move(cb)]() mutable {
            HttpResponse response;
            response.statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            response.body = reply->readAll();
            response.networkError = reply->error();
            response.errorString = reply->errorString();

            logResponse("DELETE", url, response, logBody);
            const auto json = QJsonDocument::fromJson(response.body);
            if (cb) cb(response, json);

            reply->deleteLater();
          });
}

void HttpClient::putFile(const QString& url, const QString& filePath,
                         const QString& contentType, const QString& bearerToken,
                         ProgressCallback onProgress, JsonCallback cb) {
  if (!sanitizeUrlOrLog(url, "PUT")) {
    if (cb) cb(locallyRefused(), QJsonDocument{});
    return;
  }

  auto* body = new QFile(filePath);
  if (!body->open(QIODevice::ReadOnly)) {
    blog(LOG_WARNING, "[semirprod-http] PUT refuse : fichier illisible %s",
         filePath.toUtf8().constData());
    delete body;
    if (cb) cb(locallyRefused(), QJsonDocument{});
    return;
  }

  auto request = buildRequest(url, bearerToken, contentType);
  const bool logBody = logResponseBodies_;
  logRequest("PUT", url);
  auto* reply = manager_->put(request, body);
  // Le fichier appartient desormais a la reponse : il vit exactement aussi
  // longtemps que le transfert qui le lit.
  body->setParent(reply);
  attachGuards(reply);
  capResponseSize(reply, kMaxJsonResponseBytes, "PUT", url);

  if (onProgress) {
    connect(reply, &QNetworkReply::uploadProgress, reply,
            [onProgress](qint64 sent, qint64 total) { onProgress(sent, total); });
  }

  connect(reply, &QNetworkReply::finished, this,
          [reply, url, logBody, cb = std::move(cb)]() mutable {
            HttpResponse response;
            response.statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            response.body = reply->readAll();
            response.networkError = reply->error();
            response.errorString = reply->errorString();

            logResponse("PUT", url, response, logBody);
            const auto json = QJsonDocument::fromJson(response.body);
            if (cb) cb(response, json);

            reply->deleteLater();
          });
}

void HttpClient::postForm(const QString& url, const QUrlQuery& form, const QString& bearerToken, JsonCallback cb) {
  if (!sanitizeUrlOrLog(url, "POST(form)")) {
    if (cb) cb(locallyRefused(), QJsonDocument{});
    return;
  }
  auto request = buildRequest(url, bearerToken, QStringLiteral("application/x-www-form-urlencoded"));
  const bool logBody = logResponseBodies_;
  logRequest("POST(form)", url);
  auto* reply = manager_->post(request, form.query(QUrl::FullyEncoded).toUtf8());
  attachGuards(reply);
  capResponseSize(reply, kMaxJsonResponseBytes, "POST(form)", url);
  connect(reply, &QNetworkReply::finished, this, [reply, url, logBody, cb = std::move(cb)]() mutable {
    HttpResponse response;
    response.statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    response.body = reply->readAll();
    response.networkError = reply->error();
    response.errorString = reply->errorString();

    logResponse("POST(form)", url, response, logBody);
    const auto json = QJsonDocument::fromJson(response.body);
    if (cb) cb(response, json);

    reply->deleteLater();
  });
}

} // namespace semirprod
