#include "JwtClaims.hpp"

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <QTimeZone>

namespace semirprod {

JwtClaims JwtClaims::parse(const QString& jwt) {
  JwtClaims claims;
  const QStringList parts = jwt.split(QLatin1Char('.'));
  if (parts.size() != 3) return claims;

  const QByteArray payload = QByteArray::fromBase64(
      parts.at(1).toLatin1(),
      QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
  const QJsonDocument doc = QJsonDocument::fromJson(payload);
  if (!doc.isObject()) return claims;
  const QJsonObject obj = doc.object();

  claims.subject = obj.value(QStringLiteral("sub")).toString();
  claims.orgId = obj.value(QStringLiteral("org_id")).toString();
  claims.deviceId = obj.value(QStringLiteral("device_id")).toString();
  const double exp = obj.value(QStringLiteral("exp")).toDouble(0);
  if (exp > 0) {
    claims.expiresAt =
        // QTimeZone::utc() et non QTimeZone::UTC : l'enum n'existe qu'a partir
        // de Qt 6.5, et l'instance cloud (Ubuntu 24.04) compile avec Qt 6.4.2.
        QDateTime::fromSecsSinceEpoch(static_cast<qint64>(exp), QTimeZone::utc());
  }
  return claims;
}

}  // namespace semirprod
