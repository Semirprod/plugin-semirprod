#pragma once

// ============================================================================
// Edition du plugin : « public » (le poste du streamer) ou « cloud » (l'OBS
// infogere, pilote a distance dans un conteneur).
//
// UNE interface, DEUX implementations : src/public/*.cpp et src/cloud/*.cpp.
// CMake n'en compile qu'une (option SEMIRPROD_EDITION, voir CMakeLists.txt),
// donc le code partage ne porte plus de #ifdef d'edition : il demande ici ce
// que l'edition veut, et l'edition repond.
//
// Pourquoi a la compilation et pas a l'execution : le code propre au cloud
// (appairage par jeton d'environnement, verrouillage de l'UI d'OBS) n'existe
// PAS dans le binaire public. Aucune variable d'environnement ni reglage ne
// peut donc l'activer sur le poste d'un client — un jeton d'appairage pose dans
// l'environnement d'un poste aurait sinon suffi a rattacher son OBS a une
// organisation qui n'est pas la sienne.
//
// Regle pour la suite : une difference de comportement entre editions se
// declare ICI, avec sa raison, plutot que par un #ifdef dans un fichier
// partage. Garder cette interface minimale : une fonction n'y entre que si du
// code partage en a besoin.
// ============================================================================

#include <QString>
#include <QStringList>

#include <functional>

class QJsonObject;
class QMainWindow;
class QObject;
class QWidget;

namespace semirprod::edition {

/// Vrai dans l'edition cloud. Expose a la page (`state.kiosk`) : elle masque
/// alors « Deconnecter », le compte d'une instance etant pose par l'appairage.
bool isCloud();

// --- Cycle de vie du module (plugin.cpp) -----------------------------------

/// Appele dans obs_module_post_load AVANT la construction du dock.
/// `onTokensStored` : a appeler quand un jeton vient d'arriver dans le magasin
/// sans passer par le dock (appairage automatique du cloud). Public : rien.
void beforeUi(QMainWindow* mainWindow, std::function<void()> onTokensStored);

/// Appele en fin d'obs_module_post_load, une fois notre interface en place.
/// Cloud : verrouillage de l'UI d'OBS. Public : rien.
void afterUi(QMainWindow* mainWindow);

/// obs_module_unload : oublier les pointeurs, ne RIEN detruire (Qt l'a fait).
void onUnload();

/// OBS_FRONTEND_EVENT_FINISHED_LOADING, sur le fil Qt. `context` borne la vie
/// des minuteurs eventuels. Cloud : fenetre principale maximisee.
void onFrontendLoaded(QObject* context);

// --- Adresses (Config.cpp) ---------------------------------------------------

/// Adresse effective d'un point d'acces. `prodUrl` : constante de production,
/// `envVar` : variable d'environnement correspondante. Public : production,
/// surcharge par l'environnement en build de dev seulement. Cloud : la variable
/// seule, chaine vide si elle manque (jamais de repli sur la production).
QString resolveUrl(const char* prodUrl, const char* envVar);

/// Allowlist des hotes distants. `host` est deja en minuscules ; la boucle
/// locale est traitee par Config avant l'appel, pour les deux editions.
bool isAllowedRemoteHost(const QString& host);

/// Les hotes autorises, lisibles, POUR LE JOURNAL UNIQUEMENT.
QString allowedHostsForLog();

// --- Magasin de jetons (TokenStore.cpp, repli fichier) ---------------------

/// Lier le fichier de jetons a l'identite de la machine.
bool bindsTokensToMachine();

/// Accepter un tokens.bin ecrit avant l'ajout du controle d'integrite.
bool acceptsTokenFileWithoutMac();

// --- Interface -----------------------------------------------------------

/// Commandes « connect » / « disconnect » du pont local.
bool allowsAccountCommands();

/// Popups bloquantes (mise a jour, version bloquee, acces refuse). Faux quand
/// personne n'est devant l'ecran.
bool showsBlockingPrompts();

/// Geometrie de la fenetre du Studio. Vrai si l'edition l'a imposee (ou a
/// decide de ne rien restaurer) ; faux : l'appelant restaure la geometrie
/// enregistree.
bool imposeStudioWindowGeometry(QWidget* window, const QWidget* parent);

// --- OBS -----------------------------------------------------------------

/// Pilotage de Semirprod Vertical et Multistream par le proc handler.
bool bridgesEnabled();

/// Debit de diffusion impose (SceneController::pinStreamingBitrate).
bool pinsStreamingBitrate();

/// Nettoyage d'une collection importee, apres reecriture des chemins et
/// avant ecriture du fichier. Public : rien.
void filterImportedCollection(QJsonObject& collection,
                              const QStringList& availableSourceTypes);

}  // namespace semirprod::edition
