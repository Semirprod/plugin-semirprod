#pragma once

#include <QString>

#include <obs-module.h>
#include <obs.h>

// Textes localises du module : data/locale/<langue>.ini, charges par OBS dans
// la langue de son interface (OBS_MODULE_USE_DEFAULT_LOCALE dans plugin.cpp).
//
// Une cle absente rend la cle elle-meme (comportement de obs_module_text), ce
// qui se voit a l'ecran sans rien casser. Les journaux techniques (blog) ne
// passent PAS par ici : ils restent en francais pour le support.

namespace semirprod {

/// Texte localise sous forme de QString, pour les widgets Qt et les messages
/// envoyes a l'interface web. Les parametres se posent avec .arg() comme avant.
inline QString loc(const char* key) {
  return QString::fromUtf8(obs_module_text(key));
}

/// Texte localise brut (UTF-8), pour les API d'OBS qui prennent un const char*
/// (titres de docks, proprietes de source). Le pointeur reste valide tant que
/// la table de traduction du module est chargee.
inline const char* locC(const char* key) {
  return obs_module_text(key);
}

/// Langue de l'interface web : « fr » si celle d'OBS commence par fr, « en »
/// sinon. Calculee une fois : OBS redemarre pour changer de langue.
inline const QString& uiLocaleCode() {
  static const QString code = []() {
    const char* raw = obs_get_locale();
    const QString locale = raw ? QString::fromUtf8(raw) : QString();
    return locale.startsWith(QStringLiteral("fr"), Qt::CaseInsensitive)
               ? QStringLiteral("fr")
               : QStringLiteral("en");
  }();
  return code;
}

}  // namespace semirprod
