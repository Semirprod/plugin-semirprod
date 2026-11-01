// Edition publique : le plugin installe par le streamer sur son propre poste.
// N'est compile que si SEMIRPROD_EDITION=public, la valeur par defaut (voir
// CMakeLists.txt). Pendant cloud : src/cloud/Edition.cpp.
//
// Les adresses et l'allowlist vivent dans PublicConfig.cpp.

#include "Edition.hpp"

#include <QJsonObject>
#include <QMainWindow>
#include <QObject>
#include <QWidget>

namespace semirprod::edition {

bool isCloud() { return false; }

// Pas d'appairage automatique : le compte se connecte par le flux interactif
// (DeviceAuthFlow), depuis le Studio.
void beforeUi(QMainWindow* mainWindow, std::function<void()> onTokensStored) {
  (void)mainWindow;
  (void)onTokensStored;
}

// L'OBS du client reste entier : aucun verrouillage.
void afterUi(QMainWindow* mainWindow) { (void)mainWindow; }

void onUnload() {}

void onFrontendLoaded(QObject* context) { (void)context; }

// Repli fichier d'un poste sans trousseau : un tokens.bin recopie ailleurs ne
// s'ouvre pas.
bool bindsTokensToMachine() { return true; }

// Un fichier ecrit avant l'ajout du MAC reste lisible : le refuser
// deconnecterait des postes clients qui fonctionnent.
bool acceptsTokenFileWithoutMac() { return true; }

bool allowsAccountCommands() { return true; }

bool showsBlockingPrompts() { return true; }

// L'operateur dimensionne sa fenetre : StudioWindow restaure celle qu'il a
// laissee.
bool imposeStudioWindowGeometry(QWidget* window, const QWidget* parent) {
  (void)window;
  (void)parent;
  return false;
}

// Sur le poste d'un client, Vertical et Multistream restent ses outils locaux :
// le Studio fait comme s'ils etaient absents.
bool bridgesEnabled() { return false; }

// Sa machine, sa bande passante, son choix : seul le plafond de 8000 kbps
// (SceneController::clampStreamingConfig) s'applique.
bool pinsStreamingBitrate() { return false; }

// Le client a ses plugins ici, et leurs canvas sont a lui : rien a retirer.
void filterImportedCollection(QJsonObject& collection,
                              const QStringList& availableSourceTypes) {
  (void)collection;
  (void)availableSourceTypes;
}

}  // namespace semirprod::edition
