#pragma once

#include <QString>
#include <QStringList>

namespace semirprod {

/**
 * Vrai si un NOM de clef (JSON, reglage) a l'allure d'un secret. UNE regle,
 * partagee par le masquage du journal (LiveClient) et le nettoyage des
 * sauvegardes exportees (CollectionPackager).
 *
 * Le nom est coupe en mots (separateurs non alphanumeriques et passage
 * minuscule -> majuscule : `streamKey` -> stream, key ; `api_key` -> api, key).
 * Un mot est sensible s'il CONTIENT l'un des fragments (`apikey`, `authToken`,
 * `authorization`…), sauf s'il est EXACTEMENT un faux positif connu
 * (`keyframe`, `hotkeys`, `author`). L'exclusion par sous-chaine laissait
 * passer `authorization`, qui contient `author`.
 */
inline bool isSecretLookingKey(const QString& key) {
  static const QStringList kFragments = {
      QStringLiteral("key"),      QStringLiteral("token"),      QStringLiteral("secret"),
      QStringLiteral("password"), QStringLiteral("passphrase"), QStringLiteral("auth"),
      QStringLiteral("cookie"),   QStringLiteral("credential"), QStringLiteral("bearer"),
      QStringLiteral("jwt"),      QStringLiteral("pwd"),        QStringLiteral("webhook"),
  };
  static const QStringList kFalsePositives = {
      QStringLiteral("keyframe"), QStringLiteral("keyframes"), QStringLiteral("hotkey"),
      QStringLiteral("hotkeys"),  QStringLiteral("author"),    QStringLiteral("authors"),
      QStringLiteral("keyboard"),
  };
  // Noms exacts qui ne contiennent aucun fragment mais portent un secret.
  static const QStringList kExact = {QStringLiteral("whipurl"), QStringLiteral("device_code")};
  if (kExact.contains(key.toLower())) return true;

  QStringList mots;
  QString mot;
  QChar precedent;
  for (const QChar c : key) {
    if (!c.isLetterOrNumber()) {
      if (!mot.isEmpty()) mots.append(mot.toLower());
      mot.clear();
    } else {
      if (c.isUpper() && !mot.isEmpty() && precedent.isLower()) {
        mots.append(mot.toLower());
        mot.clear();
      }
      mot.append(c);
    }
    precedent = c;
  }
  if (!mot.isEmpty()) mots.append(mot.toLower());

  for (const QString& m : mots) {
    if (kFalsePositives.contains(m)) continue;
    for (const QString& fragment : kFragments) {
      if (m.contains(fragment)) return true;
    }
  }
  return false;
}

}  // namespace semirprod
