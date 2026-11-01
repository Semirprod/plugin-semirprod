#pragma once

#include <QDateTime>
#include <QString>

namespace semirprod {

/**
 * Lecture des claims d'un JWT de service, SANS vérification de signature.
 *
 * Le plugin n'a pas à vérifier un jeton que le serveur lui a remis sur une
 * liaison TLS épinglée : c'est le pont et les API qui le vérifient. Il le lit
 * seulement pour savoir QUAND le renouveler et à QUI il appartient. L'expiration
 * vient du JWT et de nulle part ailleurs : l'ancien échange annonçait 1 h pour un
 * jeton signé à 15 min, et les postes se faisaient couper toutes les 16 min.
 */
struct JwtClaims {
  QString subject;
  QString orgId;
  QString deviceId;
  QDateTime expiresAt;  // invalide si le claim `exp` est absent ou illisible

  bool valid() const { return expiresAt.isValid(); }

  static JwtClaims parse(const QString& jwt);
};

}  // namespace semirprod
