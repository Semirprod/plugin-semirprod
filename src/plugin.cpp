#include "Edition.hpp"
#include "mac/QtTlsBackend.hpp"
#include "obs/SemirprodLiveSource.hpp"
#include "ui/EventLogDock.hpp"
#include "ui/SemirprodDock.hpp"
#include "ui/StreamStatusDock.hpp"

#include <QAction>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QTimer>

#include <QtGlobal>

#include <obs-frontend-api.h>
#include <obs-module.h>
#include "Locale.hpp"

#ifndef SEMIRPROD_PLUGIN_VERSION
#define SEMIRPROD_PLUGIN_VERSION "0.0.0"
#endif

OBS_DECLARE_MODULE()

// Textes localises : la macro exporte obs_module_set_locale, qu'OBS appelle avec
// la langue de son interface. Elle charge locale/<langue>.ini depuis le dossier
// de donnees du module (data/obs-plugins/semirprod-studio/locale/ sous Windows,
// Contents/Resources/locale/ dans le bundle macOS), avec en-US en repli pour les
// cles absentes. Les .ini DOIVENT donc etre livres : sans eux OBS signale
// « Failed to load 'en-US' text for module » et l'ecran affiche les cles. Voir
// data/locale/ et les copies dans CMakeLists.txt, build_plugin.bat et
// packaging/windows/installer.iss. Lecture par loc() / locC() (Locale.hpp).
OBS_MODULE_USE_DEFAULT_LOCALE("semirprod-studio", "en-US")

using namespace semirprod;

namespace {
SemirprodDock* g_window = nullptr;
StreamStatusDock* g_statusDock = nullptr;
EventLogDock* g_eventLogDock = nullptr;
QAction* g_action = nullptr;
}

MODULE_EXPORT const char* obs_module_description(void) {
  return "Semirprod for OBS";
}

bool obs_module_load(void) {
  // EN PREMIER sur macOS, avant toute socket : OBS.app ne livre pas de backend
  // TLS Qt, et sans celui qu'on embarque, chaque requete HTTPS meurt en « TLS
  // initialization failed » sans que rien d'autre n'ait l'air casse. No-op
  // ailleurs. Voir src/mac/QtTlsBackend.hpp.
  semirprod::ensureQtTlsBackend();

  // Les textes localises servent des le chargement (noms de source). OBS fixe normalement la langue AVANT d'appeler
  // obs_module_load ; si la table n'est pas la, obs_module_text rend la cle
  // telle quelle et on la charge ici plutot que d'afficher des cles a l'ecran.
  if (loc("Menu.Open") == QStringLiteral("Menu.Open")) {
    obs_module_set_locale(obs_get_locale());
  }

  // Register the custom OBS source type so "Flux Semirprod" appears in
  // the add-source (+) menu. Safe to call before the Qt dock exists — the
  // source type reads the stream list from StreamRegistry on demand.
  semirprod::registerSemirprodLiveSource();

  // Les trois versions qui decident de la compatibilite, sur une ligne : sans
  // elles, le journal d'un poste tiers ne dit meme pas quelle version du plugin
  // tournait. Le SDK (major.minor) est le plancher impose par libobs, et le Qt
  // de compilation le plancher impose par la porte de chargement de Windows.
  blog(LOG_INFO,
       "[semirprod-studio] module loaded — plugin %s | compile: libobs %d.%d, "
       "Qt %s | execute: OBS %s, Qt %s",
       SEMIRPROD_PLUGIN_VERSION, LIBOBS_API_MAJOR_VER, LIBOBS_API_MINOR_VER,
       QT_VERSION_STR, obs_get_version_string(), qVersion());
  return true;
}

// Tout ce qui touche a l'interface d'OBS vit ICI, pas dans obs_module_load.
//
// obs_module_load est appele PENDANT le chargement des modules, alors que la
// fenetre principale d'OBS n'a pas fini de se construire ; obs_module_post_load
// ne l'est qu'une fois TOUS les modules charges (libobs/obs-module.c, export
// optionnel `post_load`). C'est la convention que suivent les plugins a
// interface livres avec OBS : decklink-output-ui cree son interface et pose son
// callback d'evenements dans obs_module_post_load
// (frontend/plugins/decklink-output-ui/decklink-ui-main.cpp:472-480), et son
// addOutputUI() y appelle obs_frontend_get_main_window() sans precaution.
//
// Les enregistrements de types de source, eux, DOIVENT rester dans
// obs_module_load : libobs refuse un enregistrement fait en dehors.
void obs_module_post_load(void) {
  auto* mainWindow = static_cast<QMainWindow*>(obs_frontend_get_main_window());
  if (!mainWindow) {
    blog(LOG_WARNING, "[semirprod-studio] OBS main window unavailable");
    return;
  }

  // AVANT le dock, volontairement. Cloud : c'est la seule facon de lancer la
  // requete d'appairage au plus tot (cloud/Edition.cpp). Elle reste asynchrone,
  // donc le dock se construit toujours sur un magasin vide sur une instance
  // neuve ; c'est ce rappel qui lui apprend le jeton des qu'il arrive, au lieu
  // de laisser l'instance non authentifiee jusqu'au redemarrage suivant.
  // Public : rien, le rappel n'est jamais appele.
  edition::beforeUi(mainWindow, [] {
    // Le dock peut ne pas encore exister : ce rappel part parfois AVANT le
    // retour de beforeUi(), quand TokenStore::load rappelle de maniere
    // synchrone (repli fichier) et qu'une session valide est deja stockee.
    // Rien a faire alors, le constructeur du dock lit le magasin quelques
    // lignes plus bas.
    if (!g_window) return;
    g_window->onExternalTokenStored();
  });

  g_window = new SemirprodDock(mainWindow);
  g_window->setAttribute(Qt::WA_DeleteOnClose, false);
  g_window->hide();

  // ─── INSTANCE CLOUD : LA MEME INTERFACE QUE SUR LE POSTE DU CLIENT ────────
  //
  // Une premiere version ne montrait RIEN dans une instance cloud, au motif
  // que le pilotage se fait depuis le site et qu'une interface embarquee
  // donnerait deux verites pour un seul OBS.
  //
  // Ce raisonnement tenait pour le direct — destination, demarrage, quotas :
  // le site connait l'abonnement, le dock non. Il ne tient pas pour le reste.
  // La SAUVEGARDE DES COLLECTIONS de scenes n'existe que par le pont local :
  // `backup_collection`, `refresh_backups` et `import_backup` sont servies par
  // WebUiServer, aucune ne passe par LiveClient. Sans le Studio, le moteur de
  // `src/backup/` reste compile dans le binaire mais n'a plus aucun point
  // d'entree : le client d'une instance infogeree ne pouvait ni sauvegarder
  // ses scenes, ni les recuperer sur un autre poste.
  //
  // On rend donc la meme interface que sur Windows. Et l'appairage ne change
  // rien a l'experience : PairingFlow a deja depose le jeton au demarrage,
  // donc `state.connected` est vrai et la page ouvre directement sur ses
  // onglets — l'ecran « Connecte ton compte » n'apparait jamais.
  //
  // Le cout est assume : le Studio embarque un navigateur (CEF), deja present
  // dans l'image (obs-browser.so et libcef.so sont dans /opt/obs) et deja
  // compte dans les 2 Gio demandes par le pod.
  // Dock OBS « Direct » : alerte santé, état des trois liaisons et chiffres
  // du direct. Il écoute le LiveClient et les instantanés d'état portés par
  // g_window — il sera parenté à OBS, qui se charge de sa destruction au
  // shutdown comme pour les autres docks natifs.
  g_statusDock = new StreamStatusDock(g_window, g_window->liveClient());
  obs_frontend_add_dock_by_id(
      "semirprod_status_dock",
      locC("Dock.Live"),
      g_statusDock);

  // Journal des evenements metier. Meme cycle de vie que le dock Direct :
  // parente a OBS, qui le detruit au shutdown avec ses autres docks.
  g_eventLogDock = new EventLogDock(g_window);
  obs_frontend_add_dock_by_id(
      "semirprod_event_log_dock",
      locC("Dock.Journal"),
      g_eventLogDock);

  // Le Studio est un dock comme les deux autres, et il s'enregistre ICI, au
  // demarrage — pas au premier clic.
  //
  // C'est ce qui le fait apparaitre dans le menu « Docks » d'OBS, et surtout
  // c'est la seule facon que son ANCRAGE survive. OBS restaure l'agencement des
  // docks tres tot au lancement : un dock cree plus tard rate ce train, donc la
  // position que l'operateur avait ancree ne pouvait pas etre retrouvee, et le
  // dock semblait se refermer tout seul d'une session a l'autre.
  g_window->registerStudioDock();

  // L'entree « Semirprod » de la barre de menus est le SEUL chemin vers le
  // Studio sur Windows.
  //
  // Elle avait ete retiree une fois, en pariant que le menu « Docks » d'OBS
  // suffisait. C'etait faux : ce menu est une longue liste de cases a cocher ou
  // notre entree se noyait parmi celles de tous les autres plugins. Et depuis
  // que le Studio est une fenetre et non plus un dock, il n'y figure meme plus
  // (voir SemirprodDock::registerStudioDock) — cette entree-ci est tout ce
  // qu'il reste.
  QMenuBar* menuBar = mainWindow->menuBar();
  if (menuBar) {
    auto ouvreLeStudio = []() {
      if (!g_window) return;
      g_window->showWebUi();
    };

#ifdef _WIN32
    // Windows : « Semirprod » est un bouton de la barre de menus, un clic ouvre
    // le Studio. Qt rend une action de premier niveau comme un bouton, sans
    // sous-menu a deplier. Aucun autre plugin Semirprod ne range d'entree dans
    // ce menu, il n'y a donc rien a heberger dessous.
    g_action = menuBar->addAction(QStringLiteral("Semirprod"));
    QObject::connect(g_action, &QAction::triggered, g_window, ouvreLeStudio);
#else
    // macOS (et Linux, par prudence) : menu « Semirprod » a une entree.
    //
    // Deux montages plus directs ont ete essayes, et macOS refuse les deux :
    //
    //   * `menuBar->addAction(...)` — AppKit n'affiche pas une action de
    //     premier niveau sans sous-menu : l'entree disparait de la barre.
    //   * un QMenu VIDE dont on declenche l'action a l'ouverture — AppKit
    //     n'affiche pas davantage un menu sans entree.
    //
    // Les deux ont ete constates a l'ecran le 18/09 : plus de « Semirprod »
    // dans la barre, donc plus aucun moyen d'ouvrir le Studio. Sur macOS, un
    // menu doit contenir au moins une entree pour exister.
    QMenu* menu = nullptr;
    for (QAction* action : menuBar->actions()) {
      if (action && action->menu() &&
          action->text().contains("Semirprod", Qt::CaseInsensitive)) {
        menu = action->menu();
        break;
      }
    }
    if (!menu) menu = menuBar->addMenu(QStringLiteral("Semirprod"));

    // « Ouvrir Semirprod » et non « Semirprod Studio » : l'entree est deja sous
    // un menu nomme « Semirprod », repeter le mot ne dit rien de plus. Un verbe
    // dit ce que le clic fait.
    g_action = menu->addAction(loc("Menu.Open"));
    QObject::connect(g_action, &QAction::triggered, g_window, ouvreLeStudio);
#endif
  }

  blog(LOG_INFO, "[semirprod-studio] interface enregistree (3 docks + menu)");

  // En dernier, une fois notre propre interface en place. Cloud : on ne
  // verrouille que ce qui appartient a OBS, et le faire ici garantit que la
  // fenetre est entierement construite (post_load n'est appele qu'apres tous
  // les modules). Public : rien.
  edition::afterUi(mainWindow);

#ifdef SEMIRPROD_DEV_BUILD
  // Banc d'essai : ouvrir le Studio sans cliquer, pour reproduire un defaut
  // d'affichage sur une machine de test. Compile UNIQUEMENT dans un build dev,
  // et inerte sans la variable — un binaire livre n'a pas ce crochet.
  if (qgetenv("SEMIRPROD_AUTOOPEN_STUDIO") == "1") {
    QTimer::singleShot(4000, g_window, []() {
      if (g_window) g_window->showWebUi();
    });
  }
#endif
}

void obs_module_unload(void) {
  // Don't manually delete Qt objects here — obs_module_unload is called from
  // obs_shutdown, at which point Qt has already torn down the main window
  // and its child widgets. g_action (parente a la barre de menus), g_window
  // (parente a mainWindow) et les docks sont deja liberes ; y toucher
  // dereferences dangling memory and crashes in free_module. Just drop our
  // cached pointers.
  g_action = nullptr;
  g_window = nullptr;
  g_statusDock = nullptr;
  g_eventLogDock = nullptr;
  edition::onUnload();
}
