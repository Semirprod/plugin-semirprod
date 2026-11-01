#include "SemirprodDock.hpp"

#include "mac/StudioDockPolicy.hpp"
#include "StudioWindow.hpp"
#include "WebUiServer.hpp"
#include "WebUiWindow.hpp"

#include "../Config.hpp"
#include "../Edition.hpp"
#include "../Settings.hpp"
#include "../StreamRegistry.hpp"
#include "../auth/DeviceAuthFlow.hpp"
#include "../backup/BackupService.hpp"
#include "../auth/TokenStore.hpp"
#include "../http/HttpClient.hpp"
#include "../obs/AudioLevels.hpp"
#include "../obs/PreviewSceneMix.hpp"
#include "../obs/ProgramPreviewOutput.hpp"
#include "../obs/SceneController.hpp"
#include "../obs/VerticalBridge.hpp"
#include "../obs/MultistreamBridge.hpp"
#include "../obs/SemirprodLiveSource.hpp"
#include "../update/UpdateChecker.hpp"
#include "../ws/LiveClient.hpp"

// Plus aucun en-tete de widget (QLabel, QComboBox, QCheckBox, QPlainTextEdit,
// les layouts, QScrollArea, QStackedWidget…) : le dock ne construit plus
// d'interface. QPushButton et QMessageBox restent pour les quelques modales
// natives (mise a jour, acces refuse), QClipboard pour la remise du jeton.
#include <QClipboard>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QDockWidget>
#include <QVersionNumber>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <obs-module.h>

#include <algorithm>
#include "../Locale.hpp"

namespace semirprod {

// ============================================================================
// SemirprodDock — le CONTRÔLEUR du plugin. Il n'affiche plus rien.
//
// L'interface vue par l'opérateur est `webui/` (React), embarquée dans la DLL
// et servie par WebUiServer sur 127.0.0.1. Ce QWidget n'est jamais ajouté aux
// docks d'OBS ni montré : il construit l'instantané d'état diffusé à cette
// interface (broadcastToWeb), exécute les commandes qu'elle envoie
// (WebUiServer::onMessageReceived), et porte l'authentification, les jetons,
// le client live, le contrôleur de scènes et la session caméra.
//
// Il portait auparavant tout un arbre de widgets Qt (en-tête, barre latérale,
// pile de pages, bandeaux, journal) que personne ne pouvait voir. Cet arbre a
// été retiré ; les deux libellés qui servaient de STOCKAGE — le code et
// l'adresse d'appairage — sont devenus des champs (accountLabel_ /
// accountUrl_). Il reste un QWidget parce que c'est un QObject parenté à la
// fenêtre principale d'OBS, qui en assure la destruction.
// ============================================================================

SemirprodDock::SemirprodDock(QWidget* parent)
    : QWidget(parent),
      settings_(new Settings(this)),
      tokenStore_(new TokenStore(this)),
      http_(new HttpClient(this)),
      authFlow_(new DeviceAuthFlow(http_, this)),
      liveClient_(new LiveClient(settings_, this)),
      sceneController_(new SceneController(this)),
      verticalBridge_(new VerticalBridge(this)),
      multistreamBridge_(new MultistreamBridge(this)),
      programPreview_(new ProgramPreviewOutput(this)),
      studioPreview_(
          new ProgramPreviewOutput(ProgramPreviewOutput::Mix::PreviewScene, this)),
      studioMix_(new PreviewSceneMix()),
      audioLevels_(new AudioLevels()),
      sceneWatchTimer_(new QTimer(this)),
      webServer_(new WebUiServer(this, this)),
      updateChecker_(new UpdateChecker(http_, this)),
      backupService_(new BackupService(http_, this)),
      pendingFlipTimer_(new QTimer(this)),
      tokenRefreshTimer_(new QTimer(this)) {
  pendingFlipTimer_->setInterval(200);
  pendingFlipTimer_->setSingleShot(false);
  canvasPreview_ =
      new ProgramPreviewOutput(ProgramPreviewOutput::Mix::Canvas, this);

  // Bind the local UI server FIRST, before loadToken() and any other code that
  // opens outbound sockets — those can otherwise claim the server port as an
  // ephemeral source port and make the listen() fail (see WebUiServer::start).
  // Le retour de start() etait IGNORE. Si le port est deja pris (autre logiciel,
  // instance OBS precedente pas encore fermee), le serveur ne demarre pas et le
  // token est vide — mais showWebUi() naviguait quand meme vers 127.0.0.1:47615,
  // affichant DANS une fenetre OBS intitulee « Semirprod » le contenu du
  // processus qui detient le port. On memorise l etat pour refuser d ouvrir.
  webServerReady_ = webServer_->start();
  if (!webServerReady_) {
    blog(LOG_ERROR,
         "[semirprod-studio] pont local indisponible : port deja utilise");
  }

  loadSessionHistory();
  bindSignals();
  refreshSceneChoices();
  loadToken();

  connect(sceneWatchTimer_, &QTimer::timeout, this, &SemirprodDock::updateSourcePanel);
  // Poll the source panel every 200 ms so ingest-readiness (waiting → frames
  // received, WxH, oversize notice) and the derived source_state push to the
  // backend update when the feed starts delivering frames — not only on OBS
  // frontend events (scene/profile changes). Without this the panel stays
  // stuck on "En attente du flux" whenever a stream starts without the
  // operator switching scenes.
  sceneWatchTimer_->setInterval(200);
  sceneWatchTimer_->start();
  obs_frontend_add_event_callback(&SemirprodDock::obsFrontendEvent, this);
  connectCoreSignals();

  connect(updateChecker_, &UpdateChecker::updateAvailable, this,
          [this](const QString& version, const QString& url, const QString& notes,
                 const QString& severity) {
            // `version` vient du serveur. Elle etait injectee dans des libelles
            // Qt en TEXTE ENRICHI, ou un manifeste hostile pouvait glisser son
            // propre HTML (un <a href> cliquable, un <img src> qui declenche une
            // requete sortante) dans une fenetre native que le streamer croit de
            // confiance. Le filtre reste : la version part maintenant dans
            // l'instantané JSON, et rien n'empeche une version fantaisiste
            // d'aller polluer l'affichage.
            static const QRegularExpression kUpdateVersionRe(
                QStringLiteral("^[0-9]+(\\.[0-9]+){0,3}$"));
            if (!kUpdateVersionRe.match(version).hasMatch()) {
              blog(LOG_WARNING, "[semirprod-studio] update manifest: version rejetee");
              return;
            }

            blog(LOG_INFO, "[semirprod-dock] update available v%s (%s)",
                 version.toUtf8().constData(), severity.toUtf8().constData());

            // Plus de modale. Elle interrompait l'operateur en volant le focus,
            // et surtout elle se dedupait par version (`lastUpdateNotified_`) :
            // vue une fois, elle ne revenait jamais, meme si la mise a jour
            // n'etait pas faite. L'information part a l'interface web, qui en
            // fait un bandeau persistant — il reste tant que la version n'a pas
            // change, ce qui insiste plus qu'une pop-up qu'on chasse d'un clic.
            //
            // L'URL de telechargement ne quitte PAS le plugin : la page demande
            // l'ouverture par `open_update`, elle ne choisit pas la destination.
            pendingUpdateUrl_ = url;
            pendingUpdateVersion_ = version;
            pendingUpdateSeverity_ = severity;
            // Notes tronquees : c'est un texte serveur affiche tel quel, on ne
            // laisse pas un manifeste pousser des kilo-octets dans l'ecran.
            pendingUpdateNotes_ = notes.left(500);
            broadcastToWeb();
            if (openUpdateWhenKnown_) {
              openUpdateWhenKnown_ = false;
              openUpdateDownload();
            }
            maybePromptUpdate();
          });
  updateChecker_->start();

  // Toute progression de sauvegarde repart en instantane : l'interface ne sonde
  // jamais, elle recoit. Un envoi de 400 Mio sur une connexion domestique dure
  // plusieurs minutes, et un ecran muet pendant ce temps se lit comme une panne.
  connect(backupService_, &BackupService::changed, this, [this]() { broadcastToWeb(); });
  connect(backupService_, &BackupService::notice, this,
          [this](const QString& level, const QString& message) { logEvent(level, message); });

  // Renouvellement du jeton pour les sauvegardes et les imports.
  //
  // Le jeton du compte vit environ une heure ; un import lance apres son
  // expiration se faisait refuser en 401 et rendait « Compte non reconnu.
  // Reconnectez le plugin a votre compte. » Sur une instance infogeree, ce
  // geste n'existe pas : le jeton vient de l'appairage au demarrage, il n'y a
  // pas d'ecran de connexion, et le client restait bloque jusqu'au redemarrage.
  //
  // Le rappel est appele EXACTEMENT une fois, avec le jeton frais ou une chaine
  // vide. Les deux branches partagent un meme demontage : sans le crochet
  // d'echec, une tentative refusee laisserait la connexion et le rappel en
  // suspens, et un `tokenChanged` ulterieur sans rapport rejouerait la requete.
  backupService_->setTokenRenewer([this](std::function<void(const QString&)> termine) {
    const TokenBundle courant = tokenStore_->cached();
    if (courant.refreshToken.isEmpty() || authFlow_->isRunning()) {
      termine(QString());
      return;
    }
    auto* succes = new QMetaObject::Connection;
    auto* echec = new QMetaObject::Connection;
    auto demontage = [succes, echec]() {
      QObject::disconnect(*succes);
      QObject::disconnect(*echec);
      delete succes;
      delete echec;
    };
    *succes = connect(tokenStore_, &TokenStore::tokenChanged, this,
                      [this, demontage, termine]() {
                        demontage();
                        termine(tokenStore_->cached().accessToken);
                      });
    *echec = connect(authFlow_, &DeviceAuthFlow::deviceFlowFailed, this,
                     [demontage, termine](const QString&) {
                       demontage();
                       termine(QString());
                     });
    authFlow_->refresh(courant);
  });
}

SemirprodDock::~SemirprodDock() {
  obs_frontend_remove_event_callback(&SemirprodDock::obsFrontendEvent, this);
  // Ceinture + bretelles : si EXIT n'a pas été vu (unload direct), coupe quand
  // même tout ce qui tourne avant que Qt ne démonte les enfants.
  prepareShutdown();
  // Le mix de préparation n'est pas un QObject : Qt ne le détruit pas avec
  // l'arbre. prepareShutdown() a déjà arrêté la sortie qui le consommait, donc
  // l'ordre producteur/consommateur est respecté.
  delete studioMix_;
  studioMix_ = nullptr;
  // Idem : pas un QObject. prepareShutdown() a déjà détruit les volmètres.
  delete audioLevels_;
  audioLevels_ = nullptr;
}

void SemirprodDock::prepareShutdown() {
  if (shuttingDown_) return;
  shuttingDown_ = true;
  blog(LOG_INFO, "[semirprod-dock] OBS exiting — shutting down plugin work");

  // LE NAVIGATEUR EN PREMIER, tant que la boucle d'evenements de Qt est encore
  // saine.
  //
  // Sans ca, le QCefWidget n'etait ferme que par le destructeur de la fenetre
  // principale d'OBS, c'est-a-dire pendant le demontage de l'arbre de widgets.
  // Or ce destructeur fait tourner une boucle d'evenements imbriquee (voir
  // WebUiWindow::closeBrowser) : pomper des evenements pendant qu'OBS se
  // detruit, c'est le meme plantage que celui constate a la fermeture du dock,
  // deplace a la sortie du logiciel — ou il se lit comme « OBS a plante en
  // quittant » et ou plus personne ne le rattache au plugin.
  //
  // Le Studio en premier, tant que la boucle d'evenements de Qt est saine —
  // et SANS condition de plateforme.
  //
  // Sinon le QCefWidget n'est ferme que par le destructeur de la fenetre
  // principale d'OBS, c'est-a-dire pendant le demontage de l'arbre de widgets.
  // Or closeBrowser() fait tourner une boucle d'evenements imbriquee
  // (obs-browser, browser-panel.cpp:220) : pomper des evenements pendant qu'OBS
  // se detruit, c'est livrer ces evenements a des objets a demi detruits.
  //
  // Mesure a l'appui : cette ligne a d'abord ete ecrite `if (webUiWindow_ &&
  // !kDestroyBrowserOnClose)`, et le jour ou kDestroyBrowserOnClose est passe a
  // vrai sur macOS, la fermeture propre a disparu en silence. OBS s'est remis a
  // planter — mais A LA SORTIE, ou ca se lit « OBS a plante en quittant » et ou
  // plus personne ne le rattache au plugin :
  //
  //   Thread 0 CrBrowserMain  EXC_BAD_ACCESS (SIGSEGV)
  //     libobjc objc_msgSend
  //     obs-browser
  //     QtCore QObject::event(QEvent*)
  //
  // Il n'y a aucune raison de conditionner ce nettoyage a une politique
  // d'affichage : quelle que soit la plateforme, quitter OBS avec un navigateur
  // vivant est a eviter.
  //
  // `studioTearingDown_` reste VRAI jusqu'a la fin du processus : plus aucun
  // signal de visibilite ne doit rouvrir quoi que ce soit maintenant.
  // Fermer TOUS les navigateurs — le Studio courant et ceux mis de cote.
  //
  // C'est le seul moment ou closeBrowser() est non seulement sans danger mais
  // OBLIGATOIRE. Sans danger : son mode d'echec connu est un rappel de CEF qui
  // arrive trois a quatre secondes plus tard, et il n'y aura pas de « plus
  // tard », OBS s'arrete. Obligatoire : laisser un navigateur vivant fait
  // tomber CEF quand OBS decharge obs-browser, et ca s'affiche a l'ecran comme
  // « OBS a quitte de maniere inattendue » :
  //
  //   obs_shutdown -> free_module -> obs-browser obs_module_unload
  //     -> Chromium Embedded Framework  <- SIGSEGV
  //
  // `studioTearingDown_` reste VRAI ensuite : plus aucun signal de visibilite
  // ne doit rouvrir quoi que ce soit maintenant.
  // Fenetre modale encore ouverte alors qu'OBS s'arrete.
  //
  // En principe impossible — une modale bloque la fenetre principale, donc sa
  // croix. Mais rien n'oblige l'arret a venir de la : un `obs_frontend_quit`,
  // une session Windows qui se ferme, et cet evenement tombe pendant notre
  // `exec()`. `done()` la fait sortir de sa boucle et ferme son navigateur par
  // le chemin normal, avant que la suite ne coupe tout le reste.
  if (studioWindow_) studioWindow_->done(QDialog::Rejected);

  studioTearingDown_ = true;
  if (studioDock_) studioDock_->hide();
  if (webUiWindow_) {
    webUiWindow_->closeBrowser();
    webUiWindow_ = nullptr;
  }
  for (WebUiWindow* abandoned : abandonedStudios_) {
    if (abandoned) abandoned->closeBrowser();
  }
  abandonedStudios_.clear();

  // Signaux libobs d'abord : une source désactivée pendant le démontage
  // rappellerait ce dock alors qu'on est en train de tout couper.
  disconnectCoreSignals();

  // Timers périodiques d'abord : plus aucun tick (poll 200 ms, heartbeat 5 s,
  // stats 2 s, screenshots, leases, refresh JWT) ne doit repartir pendant le
  // teardown.
  QTimer* const timers[] = {
      sceneWatchTimer_,    heartbeatTimer_, outputStatsTimer_,
      sessionRetryTimer_,  previewTimer_,   pendingFlipTimer_,
      tokenRefreshTimer_,  programPreviewLeaseTimer_,
      programPreviewWarmTimer_, studioPreviewLeaseTimer_,
      verticalPreviewLeaseTimer_, canvasPreviewLeaseTimer_, audioSyncTimer_,
      audioLevelsTimer_, audioLevelsLeaseTimer_, initialFetchTimer_};
  for (QTimer* t : timers) {
    if (t) t->stop();
  }

  // Diffusion en cours a la fermeture : on consigne quand meme la session,
  // sinon fermer OBS sans arreter le stream la perdrait.
  finalizeSessionRecord();

  // Screenshots : plus d'envoi (les captures en vol se vident via le garde
  // previewExpiresAtMs_ == 0).
  stopPreviewStream();

  // Output WHIP : arrêt FORCÉ maintenant, pendant que libobs est pleinement
  // vivant. L'arrêt gracieux attend la clôture réseau du WHIP (DELETE +
  // ICE/DTLS) — plusieurs secondes que l'utilisateur passe à regarder OBS
  // se fermer. À la sortie, on coupe sec ; MediaMTX expire la session.
  stopProgramPreview(/*force=*/true);
  // Aperçu de préparation : même urgence, et il faut EN PLUS rendre le
  // deuxième mix vidéo avant qu'obs_shutdown ne démonte le pipeline.
  stopStudioPreview(/*force=*/true);
  // Aperçu vertical : la sortie est chez Vertical, qui la coupe aussi de son
  // côté à la fermeture ; on rend simplement le bail.
  stopVerticalPreview();
  // Aperçu canvas : arrêt forcé, et la référence au canvas rendue tout de
  // suite, avant qu'obs_shutdown ne démonte les canvas.
  stopCanvasPreview(/*force=*/true);

  // Volmètres : rendus maintenant, tant que libobs est vivant. Ils s'abonnent
  // au thread audio d'OBS, donc ils doivent partir avant son démontage.
  stopAudioLevels();

  // WS temps réel : fermeture propre + annulation des reconnexions/backoff
  // pour qu'aucune socket ni timer ne survive à la boucle d'événements.
  if (liveClient_) liveClient_->disconnectClient();
}

void SemirprodDock::showWebUi() {
  // Windows / Linux : le Studio est une FENETRE MODALE, pas un dock.
  //
  // Tout ce qui suit cette ligne est le chemin du DOCK, et il n'appartient plus
  // qu'a macOS (mac/StudioDockPolicy.hpp, kStudioInModalWindow). On ne l'a pas
  // reecrit pour autant : aucune des pannes macOS sur lesquelles il est bati
  // n'a pu etre rejouee depuis la fenetre, faute de Mac pour l'essai.
  if constexpr (kStudioInModalWindow) {
    openStudioWindow();
    return;
  }

  // Plus de fenetre principale a recuperer ici : la creation du dock est partie
  // dans registerStudioDock(), cette fonction ne fait plus que l'afficher.

  // FERMETURE EN COURS : NE PAS ROUVRIR MAINTENANT.
  //
  // unmountStudioNow() ferme le navigateur, et closeBrowser() fait tourner une
  // boucle d'evenements imbriquee (obs-browser, browser-panel.cpp : jusqu'a une
  // seconde). Un clic sur le menu Semirprod pendant cette seconde arrive ICI,
  // au beau milieu du demontage, et le `setVisible(true)` plus bas promenait
  // alors Qt dans l'arbre du dock pendant que son contenu se detruisait :
  //
  //   ~WebUiWindow -> QCefWidgetInternal::closeBrowser   <- boucle imbriquee
  //     OBSApp::notify -> SemirprodDock::showWebUi
  //       QWidget::setVisible -> qt6core   <- ACCESS VIOLATION (c0000005)
  //
  // Vecu le 18/09 : Studio ouvert a 15:22:17, referme a 15:22:18, demontage a
  // 15:22:19.8, clic de reouverture, OBS mort a 15:22:21. Le meme crash figure
  // au 14/09 avec le meme offset — ce n'est pas une regression, c'est un trou
  // qui n'avait jamais ete bouche sur Windows.
  //
  // Le clic n'est pas jete pour autant : il est REJOUE a la fin du demontage
  // (voir unmountStudioNow). L'avaler silencieusement se lirait comme un menu
  // mort — l'operateur reclique, et c'est ce double clic qui tuait OBS.
  if (studioTearingDown_) {
    // Sauf si OBS s'arrete : la, `studioTearingDown_` reste vrai jusqu'a la fin
    // du processus et plus rien ne doit rouvrir quoi que ce soit.
    studioReopenPending_ = !shuttingDown_;
    return;
  }

  // Refus d ouvrir si le pont local n ecoute pas : sans cette garde on chargeait
  // http://127.0.0.1:47615 quoi qu il arrive, donc potentiellement la page d un
  // squatteur de port, affichee dans une fenetre au nom de Semirprod.
  if (!webServerReady_ || webServer_->sessionToken().isEmpty()) {
    // Le message dit ce que l'operateur peut FAIRE, pas ce qui a echoue
    // techniquement. « Pont local », « port deja utilise » : c'est du
    // vocabulaire d'implementation, il reste dans le journal — qui part au
    // support — et ne monte jamais a l'ecran.
    QMessageBox::warning(
        this, QStringLiteral("Semirprod"),
        loc("Box.AnotherInstance"));
    return;
  }

  // Le dock est enregistre au demarrage (registerStudioDock) : ici on ne fait
  // que le montrer. C'est le meme chemin que la case a cocher du menu Docks
  // d'OBS, donc l'operateur a un seul comportement, quel que soit son point
  // d'entree.
  if (!studioDock_) return;
  studioDock_->setVisible(true);
  studioDock_->raise();
}

// Ouvre le Studio en fenetre modale, et rend la main quand elle se ferme.
//
// Le corps tient en quinze lignes parce que la fenetre porte elle-meme son cycle
// de vie : elle naît ici, elle meurt a sa fermeture, et son navigateur avec. Il
// n'y a plus rien a surveiller entre les deux — c'est tout l'interet du format,
// et ce que le dock ne permettait pas.
void SemirprodDock::openStudioWindow() {
  // UNE SEULE a la fois. `exec()` fait tourner une boucle d'evenements : le
  // menu Semirprod reste cliquable pendant que le Studio est ouvert (la
  // modalite bloque la fenetre principale, pas la barre de menus sur toutes les
  // plateformes), et un second clic construirait un second navigateur.
  if (studioWindowOpen_) {
    if (studioWindow_) studioWindow_->raise();
    return;
  }

  // Meme garde qu'avant : sans pont local qui ecoute, charger 127.0.0.1 revient
  // a afficher la page d'un squatteur de port sous le nom de Semirprod.
  if (!webServerReady_ || !webServer_ || webServer_->sessionToken().isEmpty()) {
    QMessageBox::warning(
        this, QStringLiteral("Semirprod"),
        loc("Box.AnotherInstance"));
    return;
  }

  auto* mainWindow = static_cast<QWidget*>(obs_frontend_get_main_window());

  // Le drapeau s'arme AVANT la construction, pas apres : le constructeur de
  // WebUiWindow attend le demarrage de CEF dans une boucle d'evenements
  // imbriquee, qui redelivre les evenements en attente — dont un second clic
  // sur le menu. Meme raison que l'ancien `studioMounting_`, en un seul endroit.
  studioWindowOpen_ = true;
  auto relache = qScopeGuard([this]() {
    studioWindowOpen_ = false;
    studioWindow_ = nullptr;
  });

  StudioWindow fenetre(mainWindow);
  if (!fenetre.isAvailable()) {
    // obs-browser absent de cette installation : repli navigateur systeme.
    blog(LOG_WARNING,
         "[semirprod-webui] CEF indisponible — repli navigateur systeme");
    openWebUiInSystemBrowser();
    return;
  }

  // Connue de l'arret d'OBS, qui doit pouvoir la faire sortir de sa boucle
  // modale (voir shutdownPluginWork).
  studioWindow_ = &fenetre;

  // Le port vient du serveur, jamais d'une constante : avec une seconde
  // instance d'OBS, ce n'est plus 47615 (voir WebUiServer::start).
  fenetre.load(QStringLiteral("http://127.0.0.1:%1").arg(webServer_->port()),
               webServer_->sessionToken());

  blog(LOG_INFO, "[semirprod-webui] ouverture du Studio (fenetre modale)");
  fenetre.exec();
  blog(LOG_INFO, "[semirprod-webui] Studio ferme");
}

// Enregistre le dock du Studio aupres d'OBS, une fois, au demarrage.
//
// Au demarrage et pas au premier clic : c'est ce qui le fait APPARAITRE dans le
// menu « Docks » d'OBS, a cote de Direct et Journal — comme n'importe quel dock
// de plugin. Tant qu'il n'etait cree qu'a la demande, il fallait un menu
// « Semirprod » dedie pour l'ouvrir, une indirection que le menu Docks rend
// inutile.
//
// Le contenu (le navigateur CEF) n'est PAS construit ici : il l'est quand le
// dock devient visible, et detruit quand il se ferme. Deux raisons — ne pas
// demarrer un navigateur dont personne ne veut, et parce qu'un QCefWidget
// masque revient blanc a l'affichage suivant : sa surface de rendu ne survit
// pas au masquage.
void SemirprodDock::closeStudioWindow() {
  if (!studioWindow_) return;
  QMetaObject::invokeMethod(studioWindow_, "reject", Qt::QueuedConnection);
}

void SemirprodDock::registerStudioDock() {
  // Windows / Linux : pas de dock du tout. Le Studio est une fenetre, ouverte
  // par le menu Semirprod. Consequence assumee : il ne figure plus dans le menu
  // « Docks » d'OBS — un dock qu'on enregistrerait pour l'y voir devrait exister
  // en permanence, donc etre masque le reste du temps, et c'est precisement ce
  // masquage qui rendait le navigateur blanc.
  if constexpr (kStudioInModalWindow) return;

  if (studioDock_) return;

  auto* mainWindow = static_cast<QWidget*>(obs_frontend_get_main_window());
  if (!mainWindow) return;

  // Le Studio vit dans un DOCK, pas dans une fenetre a part.
  //
  // Il etait un QWidget de premier niveau (Qt::Dialog) portant le widget CEF.
  // C'est la seule piece de l'ecosysteme OBS a heberger CEF ainsi : les docks
  // navigateur d'OBS, SE.Live, Streamlabs, vertical-canvas — tous passent par un
  // QDockWidget. Et ca compte, parce que le contournement d'obs-browser pour la
  // regression de focus de Qt 6.8 (QTBUG-136165) ne couvre QUE les docks :
  //
  //   #if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0) && QT_VERSION < QT_VERSION_CHECK(6, 11, 1)
  //       return false;   // QCefBrowserClient::OnSetFocus, jamais reprendre le focus
  //
  // Une fenetre de premier niveau affichee a la demande prend le focus en
  // s'affichant et sort de cette protection. Constate sur deux postes en
  // OBS 32.0.2 / Qt 6.8.3 : OBS s'arretait quatre secondes apres l'ouverture.
  //
  // Le dock est aussi ce que l'operateur attend : OBS retient sa position, sa
  // taille et sa visibilite d'une session a l'autre.
  {
    studioDock_ = new QDockWidget(QStringLiteral("Semirprod"), mainWindow);
    // Nom d'objet obligatoire : c'est par lui qu'OBS sauvegarde et restaure la
    // geometrie du dock.
    studioDock_->setObjectName(QStringLiteral("semirprod_studio_dock"));

    // JAMAIS ancrable dans la fenetre d'OBS — flottant, et rien d'autre.
    //
    // Le Studio est une interface web large (1152x648 par defaut). Ancre dans
    // un bord d'OBS, il ecrase la mise en page de l'operateur : les scenes, les
    // sources et le melangeur se retrouvent comprimes sur quelques dizaines de
    // pixels, et revenir en arriere demande de tout re-agencer a la main.
    //
    // `NoDockWidgetArea` est ce qui l'interdit reellement. Retirer
    // DockWidgetMovable ne suffirait pas : Qt laisserait encore la fenetre
    // s'accrocher au survol d'un bord. Sans zone autorisee, le glisser ne
    // propose plus aucune cible d'ancrage.
    studioDock_->setAllowedAreas(Qt::NoDockWidgetArea);
    studioDock_->setFeatures(QDockWidget::DockWidgetFloatable |
                             QDockWidget::DockWidgetClosable);

    // Il reste un QDockWidget malgre tout, et ce n'est pas un detail : le
    // contournement d'obs-browser pour la regression de focus de Qt 6.8
    // (QTBUG-136165) ne couvre QUE les docks. Une vraie fenetre de premier
    // niveau sortirait de cette protection et rendrait a OBS l'arret au bout de
    // quatre secondes constate sur deux postes. Flottant non ancrable = on
    // garde la protection sans subir l'ancrage.
    // MASQUE AVANT L'ENREGISTREMENT, ET REMASQUE APRES. Les deux sont
    // necessaires, et c'est ce qui corrige l'ouverture spontanee du Studio.
    //
    // obs_frontend_add_custom_qdock() appelle, cote OBS :
    //     addDockWidget(Qt::RightDockWidgetArea, dock);
    // en dur, et sans aucun hide(). Deux consequences qu'on ne peut pas eviter
    // depuis le plugin autrement qu'ici :
    //
    //   1. Le dock est ANCRE a droite. setAllowedAreas(NoDockWidgetArea)
    //      ci-dessus n'y change rien : allowedAreas ne contraint que les cibles
    //      du glisser-deposer de l'operateur, jamais un addDockWidget()
    //      programmatique.
    //   2. Le dock est AFFICHE. Construit avec un parent, il n'a jamais ete
    //      masque explicitement : ajoute a une fenetre principale visible, Qt
    //      le montre. L'operateur voyait donc le Studio s'ouvrir au demarrage
    //      d'OBS sans avoir rien demande.
    //
    // Et setFloating(true) juste apres reparente le dock dans une fenetre de
    // premier niveau, ce que Qt realise par un hide() suivi d'un show() : d'ou
    // la DEUXIEME ouverture, ancree a droite puis flottante.
    //
    // Le hide() final laisse OBS seul maitre de la visibilite : il restaurera
    // le dock si l'operateur l'avait laisse ouvert, et le laissera ferme sinon.
    // Le connect() de visibilityChanged vient APRES, pour qu'aucun de ces
    // mouvements de montage ne declenche le CEF.
    studioDock_->hide();

    // OBS prend la propriete et le detruira avec sa fenetre principale. On garde
    // le pointeur uniquement pour l'afficher depuis le menu.
    obs_frontend_add_custom_qdock(QStringLiteral("semirprod_studio_dock").toUtf8().constData(),
                                  studioDock_);
    // Flottant par defaut au premier affichage : ancre, il ecraserait la mise en
    // page de l'operateur sans prevenir.
    studioDock_->setFloating(true);
    studioDock_->resize(1152, 648);
    studioDock_->hide();
  }


  connect(studioDock_, &QDockWidget::visibilityChanged, this,
          &SemirprodDock::onStudioDockVisibilityChanged);
}

// Detruit le Studio (navigateur compris), en respectant l'ordre qui evite le
// plantage sur macOS.
//
// Extraite parce qu'elle a maintenant DEUX appelants : la fermeture qui dure
// (le timer de demontage) et la reouverture d'un Studio dont la vue native a
// disparu. Ce second cas est le plus frequent a l'usage — il suffit de
// refermer et rouvrir en moins d'une seconde et demie.
void SemirprodDock::unmountStudioNow() {
  if (!webUiWindow_) return;

  auto* window = webUiWindow_;
  webUiWindow_ = nullptr;
  if (studioDock_) studioDock_->setWidget(nullptr);

  if constexpr (!kDestroyBrowserOnClose) {
    // macOS : on ABANDONNE le widget. Ni fermeture, ni destruction.
    //
    // Ce n'est pas un oubli, c'est le seul etat qui ne plante pas. Toute
    // tentative de fermer ce navigateur — closeBrowser(), ou la destruction
    // qui l'appelle — tue OBS trois a quatre secondes plus tard, parce que
    // detachBrowserWindow() relache la vue native pendant que CEF s'en sert
    // encore (cf. mac/StudioDockPolicy.hpp). Differer la destruction de dix
    // secondes n'y change rien : c'est l'appel qui tue, pas le calendrier.
    //
    // Le widget est donc sorti de l'ecran et laisse en vie. Il coute un
    // navigateur en arriere-plan jusqu'a la fermeture d'OBS, qui le liberera
    // avec le reste. C'est un prix connu et borne, paye a chaque reouverture
    // du Studio — contre un plantage certain dans l'autre cas.
    window->hide();
    window->setParent(nullptr);
    // Garde une trace : son navigateur devra etre ferme a la sortie d'OBS.
    abandonedStudios_.append(window);
    return;
  }

  if constexpr (false) {  // sequence de fermeture DESACTIVEE : voir ci-dessous
    // macOS : l'ORDRE fait tout. Detail dans mac/StudioDockPolicy.hpp.
    //
    //   1. le sortir de toute hierarchie visible et le masquer, pour qu'aucun
    //      evenement d'affichage ne l'atteigne plus. C'est ce qui empeche le
    //      showEvent d'obs-browser de relancer la creation d'un navigateur sur
    //      une vue morte.
    //   2. fermer le navigateur ICI, hors du destructeur : la boucle
    //      d'evenements imbriquee de closeBrowser() tourne alors sur un objet
    //      vivant et coherent, pas au milieu d'une destruction.
    //   3. detruire seulement apres. Le destructeur n'aura plus rien a fermer,
    //      donc plus de boucle imbriquee a ce moment-la.
    //
    // `studioTearingDown_` couvre l'etape 2 : cette boucle imbriquee delivre
    // des signaux de visibilite, qui sans lui remonteraient un navigateur
    // pendant qu'on ferme celui-ci.
    studioTearingDown_ = true;
    window->hide();
    window->setParent(nullptr);
    window->closeBrowser();
    studioTearingDown_ = false;

    // 4. DETRUIRE PLUS TARD, BEAUCOUP PLUS TARD.
    //
    // closeBrowser() n'attend pas vraiment la fermeture : il pompe les
    // evenements jusqu'a `readyToClose` OU 1 seconde, selon ce qui vient en
    // premier (browser-panel.cpp:222, `QTimer::singleShot(1000, ...quit)`).
    // Passe ce delai il rend la main alors que CEF ferme encore.
    //
    // Or obs-browser ne coupe le lien retour que dans ce meme closeBrowser :
    //
    //   CefRefPtr<CefClient> client{host->GetClient()};
    //   if (client) { browserClient->widget = nullptr; }
    //
    // et ce `if` peut ne pas passer. Quand CEF appelle ensuite OnBeforeClose,
    // il fait `widget->finishCloseBrowser()` — sur notre widget detruit entre
    // temps. C'est exactement le plantage observe, et son delai le trahit :
    // demontage a 18:51:31, mort a 18:51:35, quatre secondes plus tard.
    //
    //   Thread 0 CrBrowserMain  EXC_BAD_ACCESS (SIGSEGV) at 0x0
    //     Chromium Embedded Framework
    //     CoreFoundation ___forwarding___
    //     obs-browser
    //     QtCore QObject::event(QEvent*)
    //
    // Le signal qui dirait « CEF a fini » (readyToClose) n'est pas expose par
    // l'interface QCefWidget : impossible de s'y connecter depuis un plugin.
    // On laisse donc une marge large. Le widget est deja hors ecran et sans
    // parent : il ne coute qu'un peu de memoire pendant ce delai, et plus rien
    // ne l'affiche.
    //
    // `window` est capture par valeur et teste : si OBS se ferme avant
    // l'echeance, le timer meurt avec son parent et OBS detruit l'arbre —
    // le navigateur, lui, est deja ferme, c'est tout ce qui compte.
    QTimer::singleShot(kBrowserDestroyDelayMs, window,
                       [window]() { window->deleteLater(); });
    return;
  }

  // Windows / Linux : SORTIR LE MOURANT DE L'ARBRE DU DOCK, PUIS FERMER LE
  // NAVIGATEUR ICI — pas dans le destructeur.
  //
  // Le `setWidget(nullptr)` plus haut ne suffit PAS. Qt retire le widget du
  // LAYOUT du dock, il n'en change pas le parent : le WebUiWindow reste enfant
  // du QDockWidget jusqu'a ~QObject. Un `studioDock_->setVisible(true)` pendant
  // sa destruction fait donc parcourir a Qt un arbre ou il figure encore, a
  // demi detruit. C'est le plantage du 18/09 (et deja du 14/09), detaille dans
  // showWebUi().
  //
  // Et le simple `deleteLater()` d'avant laissait ce trou grand ouvert : la
  // destruction tombait plus tard, dans la boucle d'evenements, et c'est le
  // destructeur qui appelait closeBrowser() — donc une boucle d'evenements
  // imbriquee AU MILIEU d'une destruction, sans que rien ne soit leve pour
  // repousser les signaux qu'elle delivre.
  //
  // Trois gestes, dans cet ordre :
  //   1. hide() + setParent(nullptr) — plus aucun evenement d'affichage ne
  //      l'atteint, et il ne fait plus partie de l'arbre du dock. Ce sont les
  //      deux memes lignes que la branche macOS juste au-dessus.
  //   2. closeBrowser() ici, `studioTearingDown_` leve — la boucle imbriquee
  //      tourne sur un objet VIVANT et coherent, et les signaux qu'elle
  //      delivre (visibilite, clic sur le menu) trouvent le drapeau a vrai.
  //   3. deleteLater() — le destructeur n'a plus de navigateur a fermer, donc
  //      plus de boucle imbriquee a ce moment-la.
  //
  // Le drapeau est REMIS DANS L'ETAT OU IL ETAIT, jamais abaisse de force :
  // a l'arret d'OBS il est leve pour de bon (shutdownPluginWork), et le rabatre
  // ici rouvrirait la porte a une reouverture pendant que tout se ferme.
  const bool demontageDejaEnCours = studioTearingDown_;
  studioTearingDown_ = true;
  window->hide();
  window->setParent(nullptr);
  window->closeBrowser();
  studioTearingDown_ = demontageDejaEnCours;

  window->deleteLater();

  // Le clic arrive pendant la fermeture est rejoue MAINTENANT : navigateur
  // ferme, pile revenue au repos. Sans ca, refermer puis rouvrir le Studio
  // dans la meme seconde ne faisait rien du tout — un menu mort, et un
  // operateur qui reclique.
  //
  // singleShot(0) et pas un appel direct : on sort d'abord de unmountStudioNow
  // (elle est appelee depuis un timer et depuis onStudioDockVisibilityChanged),
  // pour ne pas remonter un navigateur depuis la pile qui vient d'en detruire un.
  if (studioReopenPending_) {
    studioReopenPending_ = false;
    QTimer::singleShot(0, this, [this]() { showWebUi(); });
  }
}

// Remet le Studio dans son etat de depart, une fois OBS charge.
//
// POURQUOI CETTE FONCTION EXISTE
//
// registerStudioDock() termine par un hide(), et ca ne suffit pas : OBS relit
// ensuite son `DockState` (frontend/widgets/OBSBasic.cpp, restoreState ligne
// 1254) et emet FINISHED_LOADING seulement apres (ligne 1397). Entre les deux,
// il rouvre le Studio dans l'etat ou il etait a la fermeture precedente. Le
// plugin n'a donc aucun moyen d'obtenir un demarrage ferme depuis
// registerStudioDock() : le dernier mot revient a OBS, et ce mot arrive plus
// tard. D'ou une correction ICI, apres la restauration, plutot qu'une enieme
// tentative de la devancer.
//
// Ce que ca annule volontairement : la memorisation de la visibilite du Studio
// d'une session a l'autre. C'est un choix assume et non un effet de bord — le
// Studio est une interface web de 1152x648 avec un navigateur complet derriere,
// pas un panneau d'OBS. Qu'il s'ouvre tout seul au demarrage, et parfois ancre
// a droite en ecrasant la mise en page de l'operateur, coute plus cher que la
// commodite de le retrouver ouvert. Les deux autres docks (Direct, Journal),
// eux, gardent la memoire d'OBS : ils sont legers et sans navigateur.
//
// setFloating(true) en plus du hide() : le `DockState` peut avoir memorise le
// Studio ANCRE. Le masquer sans le detacher laisserait un dock ancre invisible,
// qui reapparaitrait ancre au premier clic sur le menu Semirprod — exactement
// la mise en page ecrasee qu'on veut eviter.
void SemirprodDock::settleStudioDockAfterStartup() {
  if (studioDock_) {
    // Detache, meme si le `DockState` sauvegarde l'avait memorise ancre : le
    // Studio ne doit jamais s'ancrer dans un bord d'OBS, et une installation
    // ayant connu une version ancrable en garderait sinon la trace.
    studioDock_->setFloating(true);
    studioDock_->hide();
  }
  // APRES le hide() : celui-ci emet visibilityChanged(false), qu'on veut voir
  // ignore par la garde de onStudioDockVisibilityChanged. Lever le drapeau
  // avant ferait demarrer un demontage differe pour un navigateur qui n'a
  // jamais ete monte.
  frontendReady_ = true;
}

// Contenu du dock, cree a l'affichage et detruit a la fermeture.
void SemirprodDock::onStudioDockVisibilityChanged(bool visible) {
  if (!studioDock_) return;

  // Trace de transition. Ce signal est la seule chose qui pilote le cycle de
  // vie du navigateur : sans la voir, une ouverture en double se lit dans le
  // journal comme deux « ouverture » sans dire ce qui s'est passe entre.
  blog(LOG_INFO,
       "[semirprod-webui] dock visible=%d monte=%d montage_en_cours=%d pret=%d",
       visible ? 1 : 0, webUiWindow_ ? 1 : 0, studioMounting_ ? 1 : 0,
       frontendReady_ ? 1 : 0);

  // Tant qu'OBS n'a pas fini de charger, ce signal ne vient PAS de l'operateur.
  //
  // Il vient de la restauration des docks (QMainWindow::restoreState sur le
  // `DockState` sauvegarde), qui rouvre le Studio tel qu'il etait a la
  // fermeture precedente. On ne monte donc rien : settleStudioDockAfterStartup()
  // remettra le dock ferme et flottant juste apres, et c'est le clic de
  // l'operateur — menu Semirprod ou menu Docks — qui montera le navigateur.
  if constexpr (kForceStudioClosedAtStartup) {
    if (!frontendReady_) return;
  }

  // Echo arrive pendant la fermeture du navigateur : closeBrowser() pompe les
  // evenements, donc ce signal peut tomber au milieu du demontage. Il n'y a
  // alors rien de sensé a faire — ni monter (le navigateur qu'on ferme serait
  // aussitot reconstruit), ni demonter (c'est deja en cours).
  if (studioTearingDown_) return;

  // Echo de reentrance : le montage en cours fait tourner une boucle
  // d'evenements imbriquee (voir plus bas), qui redelivre ce signal avant que
  // le montage soit fini. On ignore ces echos dans les DEUX sens — il n'y a
  // rien a detruire tant que `webUiWindow_` n'est pas affecte, et rien a
  // monter puisqu'un montage est deja en vol. L'etat reel est reconcilie par
  // l'affichage ou la fermeture suivants.
  if (studioMounting_) return;

  if (!visible) {
    // Fermeture du dock : la politique depend de la plateforme, et le detail
    // de chaque choix est dans mac/StudioDockPolicy.hpp.
    if (!webUiWindow_) return;

    if constexpr (!kDestroyBrowserOnClose) {
      // macOS : on ne ferme et on ne detruit RIEN a chaud. Toute tentative
      // faisait tomber OBS trois a quatre secondes plus tard, y compris en
      // differant la destruction de dix secondes — c'est closeBrowser() qui
      // tue, pas la destruction. Le navigateur reste donc vivant, masque.
      return;
    }

    // Windows / Linux : comportement d'origine, inchange.
    //
    // Monter le navigateur dans le dock provoque lui-meme un masquage suivi
    // d'un reaffichage : WebUiWindow impose un minimum de 960x540, la fenetre
    // flottante doit donc se redimensionner, et Qt la recree. Le demontage
    // immediat repondait a ce masquage en detruisant le navigateur, le
    // reaffichage en reconstruisait un — trois cycles complets en moins d'une
    // seconde, et libcef tombait (STATUS_BREAKPOINT sur CrBrowserMain,
    // constate le 15 aout a 18:42 en boucle au demarrage).
    //
    // On attend donc avant de demonter. Si le dock revient — echo de notre
    // propre montage, onglet ramene au premier plan, OBS restaure sa mise en
    // page — le demontage est annule et le navigateur vit sa vie. Seule une
    // fermeture qui DURE le detruit.
    if (!studioTeardownTimer_) {
      studioTeardownTimer_ = new QTimer(this);
      studioTeardownTimer_->setSingleShot(true);
      connect(studioTeardownTimer_, &QTimer::timeout, this, [this]() {
        if (!studioDock_ || !webUiWindow_) return;
        if (studioDock_->isVisible()) return;  // revenu entre-temps
        blog(LOG_INFO, "[semirprod-webui] demontage du Studio (dock ferme)");

        unmountStudioNow();
      });
    }
    studioTeardownTimer_->start(1500);
    return;
  }

  // Retour a l'affichage : annule un demontage en attente. Cas nominal sur
  // Windows/Linux pendant la salve de masquage/reaffichage du montage ; sans
  // objet sur macOS, ou ce timer n'existe jamais.
  if (studioTeardownTimer_) studioTeardownTimer_->stop();

  // Deja monte : on ne touche a RIEN et on ressort.
  //
  // Surtout pas un load() « pour rafraichir » : c'etait la premiere version de
  // cette branche, et elle plantait OBS a la reouverture du Studio, pile sur
  // cette ligne —
  //
  //   Thread 0 CrBrowserMain  EXC_BAD_ACCESS (SIGSEGV) at 0x0
  //     obs-browser
  //     semirprod::WebUiWindow::load(QString const&, QString const&)
  //     semirprod::SemirprodDock::onStudioDockVisibilityChanged(bool)
  //
  // setURL() d'obs-browser fait `cefBrowser->GetMainFrame()->LoadURL(url)`
  // SANS verifier GetMainFrame(), et une frame principale ne survit pas au
  // masquage du widget. C'est un bug connu d'obs-browser, ouvert :
  // https://github.com/obsproject/obs-browser/issues/375 — « QCefWidget misses
  // CEF main frame after being hidden once ».
  //
  // La page reste donc telle qu'elle etait. Ce n'est pas un probleme : elle est
  // vivante en arriere-plan, son WebSocket avec le plugin n'a jamais ete coupe,
  // et c'est lui qui la tient a jour — pas le rechargement.
  if (webUiWindow_) {
    // Le Studio est encore monte : c'est le cas quand on le rouvre AVANT que
    // le demontage differe n'ait tire (1,5 s), ce qui est le cas courant — on
    // referme et on rouvre en une seconde, pas plus.
    //
    // Mais sa vue native, elle, n'a pas survecu au masquage : sur macOS
    // obs-browser n'en rattache aucune apres la creation. Le navigateur est
    // donc vivant et invisible — l'ecran gris. On verifie, et s'il n'y a plus
    // de vue on recree tout de suite au lieu d'attendre un timer qui, dans ce
    // scenario, ne tirera jamais.
    //
    // Differe d'un tour de boucle : a cet instant le dock vient tout juste de
    // redevenir visible et la geometrie du widget n'est pas encore etablie.
    QTimer::singleShot(0, this, [this]() {
      if (!webUiWindow_) return;
      if (webUiWindow_->refitBrowserView()) return;  // vue intacte, rien a faire

      blog(LOG_INFO,
           "[semirprod-webui] vue du navigateur perdue au masquage — "
           "recreation du Studio");
      unmountStudioNow();

      // Remonte par le chemin normal : webUiWindow_ est nul, donc cet appel
      // reconstruit le navigateur exactement comme une premiere ouverture.
      if (studioDock_ && studioDock_->isVisible()) {
        onStudioDockVisibilityChanged(true);
      }
    });
    return;
  }

  // Meme garde qu'avant : sans pont local qui ecoute, charger 127.0.0.1:47615
  // afficherait la page d'un squatteur de port sous le nom de Semirprod.
  if (!webServerReady_ || !webServer_ || webServer_->sessionToken().isEmpty()) {
    blog(LOG_WARNING,
         "[semirprod-webui] pont local indisponible — Studio non ouvert");
    return;
  }

  blog(LOG_INFO, "[semirprod-webui] ouverture du Studio (dock CEF)");

  // La garde s'arme AVANT la construction, pas apres.
  //
  // Le constructeur de WebUiWindow attend le demarrage de CEF dans une boucle
  // d'evenements imbriquee (WebUiWindow.cpp, waitForBrowserWithoutFreezing) —
  // volontairement, pour qu'OBS continue de se peindre. Mais cette boucle
  // redelivre les evenements en attente, dont un second visibilityChanged, et
  // `webUiWindow_` n'est encore que nul : le test « deja monte » plus haut ne
  // retenait donc rien. Deux WebUiWindow se construisaient en reentrance, donc
  // deux CefBrowserHost::CreateBrowserSync en vol, et libcef tombait sur un
  // CHECK (STATUS_BREAKPOINT sur CrBrowserMain, ~4 s apres le demarrage).
  //
  // Relachee a la sortie quel que soit le chemin : le repli ci-dessous rappelle
  // setVisible(false), qui re-entre ici.
  studioMounting_ = true;
  auto relacheMontage = qScopeGuard([this]() { studioMounting_ = false; });

  auto* contenu = new WebUiWindow(studioDock_);
  if (!contenu->isAvailable()) {
    // obs-browser absent de cette installation : repli navigateur systeme, et
    // on referme le dock qui resterait vide.
    blog(LOG_WARNING,
         "[semirprod-webui] CEF indisponible — repli navigateur systeme");
    contenu->deleteLater();
    studioDock_->setVisible(false);
    openWebUiInSystemBrowser();
    return;
  }

  webUiWindow_ = contenu;
  // Le port vient du serveur, jamais d'une constante : avec une seconde
  // instance d'OBS, ce n'est plus 47615 (voir WebUiServer::start).
  webUiWindow_->load(
      QStringLiteral("http://127.0.0.1:%1").arg(webServer_->port()),
      webServer_->sessionToken());
  studioDock_->setWidget(webUiWindow_);
  blog(LOG_INFO, "[semirprod-webui] Studio monte dans le dock");
}

// Repli : le Studio dans le navigateur du systeme.
//
// Deux appelants, deux raisons differentes — obs-browser absent de cette
// installation, ou Qt de l'hote touche par QTBUG-136165 — d'ou l'extraction ici
// plutot qu'un bloc duplique.
//
// La page n'a pas le jeton : il est normalement injecte dans localStorage par le
// script de demarrage CEF, qui n'existe pas ici. On le remet par le
// PRESSE-PAPIERS, jamais a l'ecran ni dans le journal (un streamer partage sa
// fenetre), et l'ecran d'acces de la page attend justement qu'on l'y colle.
void SemirprodDock::openWebUiInSystemBrowser() {
  QGuiApplication::clipboard()->setText(webServer_->sessionToken());
  QDesktopServices::openUrl(
      QUrl(QStringLiteral("http://127.0.0.1:%1").arg(webServer_->port())));
  QMessageBox::information(
      this, QStringLiteral("Semirprod"),
      loc("Box.BrowserOpened"));
}

void SemirprodDock::obsFrontendEvent(enum obs_frontend_event event, void* data) {
  auto* self = static_cast<SemirprodDock*>(data);
  if (!self) return;

  switch (event) {
    case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
    case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP:
      // Marqueur posé de façon synchrone (l'événement arrive sur le thread Qt
      // principal) : pendant que OBS démonte une collection, il pompe la boucle
      // d'événements, donc un slot en attente peut tourner au milieu du
      // démontage, sur une liste de scènes partielle. refreshSceneChoices ne
      // persiste rien tant que ce drapeau est levé.
      //
      // CLEANUP est indispensable en plus de CHANGING : il est aussi émis au
      // DÉBUT du vidage de fin de session, or EXIT (qui lève shuttingDown_)
      // n'arrive qu'APRÈS « All scene data cleared » — vérifié dans le log OBS.
      // Sans lui, chaque fermeture d'OBS voyait défiler des listes de scènes
      // amputées et effaçait le mapping de la collection comme « périmé ».
      self->sceneCollectionChanging_ = true;
      return;
    case OBS_FRONTEND_EVENT_FINISHED_LOADING:
    case OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED:
    case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
    case OBS_FRONTEND_EVENT_PROFILE_CHANGED:
      if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
        QMetaObject::invokeMethod(
            self,
            [self]() {
              self->obsLoaded_ = true;
              // OBS cloud : fenetre principale maximisee (voir
              // cloud/Edition.cpp). Public : rien.
              edition::onFrontendLoaded(self);
              if (self->pluginBlocked_) self->showBlockedPrompt();
              self->maybePromptUpdate();
              if (self->studioAccessKnown_ && !self->studioAccess_ &&
                  !self->accessToken().isEmpty()) {
                self->maybePromptAccessDenied();
              }
            },
            Qt::QueuedConnection);
      }
      // La bascule est terminée : la liste de scènes publiée par OBS
      // correspond de nouveau à la collection annoncée. SCENE_LIST_CHANGED est
      // exclu à dessein — il est justement émis pendant le chargement.
      if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED ||
          event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
        self->sceneCollectionChanging_ = false;
      }
      if constexpr (kForceStudioClosedAtStartup) {
        if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
          self->settleStudioDockAfterStartup();
        }
      }
      QMetaObject::invokeMethod(
          self,
          [self]() {
            self->refreshSceneChoices();
            self->updateSourcePanel();
            // Enforce the 1080p canvas cap at startup and whenever the profile
            // changes (this lambda also runs on scene-list/collection changes —
            // harmless: the clamp is idempotent and only fires once).
            if (self->sceneController_->clampVideoCanvas()) {
              self->setStatus(QStringLiteral(
                  "Résolution du canevas ajustée à 1080p maximum."));
              blog(LOG_INFO,
                   "[semirprod-dock] canvas/output resolution clamped to 1080p max");
            }
            self->pushObsState();
            self->pushSourcesState();
            self->pushAudioState();
            // La WebUI est l'interface principale et c'est elle qui affiche les
            // sélecteurs Live/Bug : elle DOIT être poussée ici. Sinon rien ne la
            // prévient qu'on a changé de collection — pushObsState ne parle qu'au
            // backend, et le seul broadcast périodique (pushOutputStats) ne
            // s'exécute que sortie active. Hors diffusion, la page gardait donc
            // la liste de scènes de l'ancienne collection indéfiniment, et un
            // set_config depuis cette page réécrivait ces noms périmés.
            self->broadcastToWeb();
          },
          Qt::QueuedConnection);
      return;
    case OBS_FRONTEND_EVENT_SCENE_CHANGED:
      QMetaObject::invokeMethod(
          self,
          [self]() {
            self->pushObsState();
            self->pushSourcesState();
            // Le mixer suit la scène programme (sources actives) : pousser tout
            // de suite plutôt que d'attendre le heartbeat 5 s.
            self->pushAudioState();
          },
          Qt::QueuedConnection);
      return;
    case OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED:
    case OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED:
    case OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED:
      // Le mode studio et la scene de preparation se pilotent AUSSI dans OBS :
      // l'operateur devant la machine reste maitre, la regie ne fait que
      // refleter. D'ou un push a chaque changement, quelle que soit l'origine.
      QMetaObject::invokeMethod(
          self,
          [self, event]() {
            if (event == OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED) {
              // Repointe la vue du deuxieme mix sans toucher a la sortie :
              // l'encodeur et la session WHIP survivent, donc le spectateur du
              // dashboard voit la nouvelle preparation sans coupure.
              if (self->studioMix_) self->studioMix_->syncToPreviewScene();
            } else if (event == OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED) {
              // Plus de preparation : la sortie d'apercu n'a plus de sujet.
              self->stopStudioPreview();
            }
            self->pushObsState();
            // Les sources inspectees par la regie suivent la scene montree ;
            // en mode studio c'est la preparation qui change.
            self->pushSourcesState();
            self->broadcastToWeb();
          },
          Qt::QueuedConnection);
      return;
    case OBS_FRONTEND_EVENT_EXIT:
      // Synchronous on purpose: EXIT is dispatched on the Qt main thread and
      // it's the last moment where libobs + la boucle d'événements sont encore
      // pleinement vivants. Une invocation queued pourrait ne jamais tourner.
      self->prepareShutdown();
      return;
    case OBS_FRONTEND_EVENT_STREAMING_STARTING:
      // The moment entitlement actually matters: re-validate Studio access
      // exactly when the user goes live (event-driven, no polling). If it's
      // lapsed the banner surfaces immediately; the server gateway is still
      // the hard gate that refuses the publish.
      QMetaObject::invokeMethod(
          self,
          [self]() {
            self->checkStudioAccess();
            // Re-check the canvas cap right before going live too. Note this
            // only rewrites config (applies at next video reset); the encoder
            // scaled-size clamp below is the hard 1080p guarantee for the wire.
            self->sceneController_->clampVideoCanvas();
            // Enforce the platform ceiling before the output actually starts.
            // Fires for every start path (dashboard, web mirror, manual OBS
            // button) since OBS emits STARTING for all of them.
            const OutputClampResult clamp =
                self->sceneController_->clampStreamingConfig();
            if (clamp.any()) {
              self->setStatus(QStringLiteral(
                  "Paramètres ajustés : sortie limitée à 1080p / 8000 kbps max."));
              blog(LOG_INFO,
                   "[semirprod-dock] output clamped to 1080p / 8000 kbps max "
                   "(bitrate=%d resolution=%d)",
                   clamp.bitrateClamped ? 1 : 0, clamp.resolutionClamped ? 1 : 0);
            }
          },
          Qt::QueuedConnection);
      return;
    case OBS_FRONTEND_EVENT_STREAMING_STARTED:
      QMetaObject::invokeMethod(
          self,
          [self]() {
            // Record start time on the Qt main thread so it stays in sync
            // with the next pushObsState() — we don't rely on OBS's own
            // output-side timestamps because they aren't exposed via the
            // frontend API and racing them would risk a bogus elapsed.
            self->streamingStartedAt_ =
                QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
            self->logEvent(QStringLiteral("success"),
                           loc("Journal.StreamStarted"));
            // Fresh go-live: drop any stale delta baseline so the first
            // output_stats tick doesn't emit a bogus kbps against pre-stream
            // counters.
            self->outputStatsBaselineValid_ = false;
            // Belt-and-braces: if the encoder came up above the bitrate ceiling
            // despite the pre-start clamp (config read too early, encoder built
            // late), pull it down live. Resolution can't change on a running
            // encoder, so only the bitrate is re-checked here.
            if (self->sceneController_->clampStreamingEncoderLive()) {
              self->setStatus(QStringLiteral(
                  "Paramètres ajustés : sortie limitée à 1080p / 8000 kbps max."));
              blog(LOG_INFO,
                   "[semirprod-dock] output clamped to 8000 kbps max (live)");
            }
            self->pushObsState();
          },
          Qt::QueuedConnection);
      return;
    case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
      QMetaObject::invokeMethod(
          self,
          [self]() {
            self->streamingStartedAt_.clear();
            self->outputStatsBaselineValid_ = false;
            self->logEvent(QStringLiteral("info"),
                           loc("Journal.StreamStopped"));
            self->pushObsState();
            self->maybePromptUpdate();
          },
          Qt::QueuedConnection);
      return;
    case OBS_FRONTEND_EVENT_RECORDING_STARTED:
      QMetaObject::invokeMethod(
          self,
          [self]() {
            self->recordingStartedAt_ =
                QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
            self->logEvent(QStringLiteral("info"),
                           loc("Journal.RecordingStarted"));
            self->pushObsState();
          },
          Qt::QueuedConnection);
      return;
    case OBS_FRONTEND_EVENT_RECORDING_STOPPED:
      QMetaObject::invokeMethod(
          self,
          [self]() {
            self->recordingStartedAt_.clear();
            self->pushObsState();
            self->maybePromptUpdate();
          },
          Qt::QueuedConnection);
      return;
    default:
      return;
  }
}

void SemirprodDock::bindSignals() {
  // Aucun branchement de widget ici : les commandes de l'interface arrivent par
  // WebUiServer::onMessageReceived, qui appelle directement startAuth(),
  // disconnectAccount(), selectStream(), setSceneConfig()…

  connect(authFlow_, &DeviceAuthFlow::deviceFlowStarted, this, [this](const DeviceFlowSession& session) {
    // Le code lisible par l'humain — celui qu'on tape sur un telephone. Coupe
    // en deux groupes de quatre pour la lecture, comme l'affiche le serveur.
    const QString uiCode = session.userCode.size() == 8
        ? QStringLiteral("%1-%2").arg(session.userCode.left(4), session.userCode.right(4))
        : session.userCode;
    // TEXTE SIMPLE, pas de HTML. Ces deux champs etaient des QLabel en texte
    // enrichi ; ils partent maintenant a une interface React qui les rend en
    // texte, ou des balises s afficheraient telles quelles. L interface met en
    // forme, le plugin envoie la donnee.
    accountLabel_ = uiCode;
    pairingFailed_ = false;
    accountUrl_ = session.verificationUriBase.isEmpty()
                      ? session.verificationUri
                      : session.verificationUriBase;
    setStatus(QStringLiteral("Appairage en cours. Autorise-le depuis ton navigateur."));
    // Sans cette diffusion le code restait dans le dock : l'interface web ne
    // recevait `accountLabel` qu'au prochain instantane declenche par un autre
    // evenement — il n'y a aucune diffusion periodique.
    broadcastToWeb();
    // Best-effort: try to open the local browser. On a kiosk / RDP host
    // where this fails, the user just types the URL into their phone
    // manually — the code on screen still works.
    QDesktopServices::openUrl(QUrl(session.verificationUri));
  });

  connect(authFlow_, &DeviceAuthFlow::deviceFlowSucceeded, this, [this](const TokenBundle& bundle) {
    tokenStore_->save(bundle, [this](bool ok) {
      // NE PAS conditionner la suite a `ok` : `ok` ne dit QUE si le jeton a pu
      // etre ecrit sur le support persistant. La session, elle, est valide en
      // memoire des le retour de save() (TokenStore::save met a jour cached_
      // avant meme d'ecrire, et le documente : « persistence is fire-and-log »).
      //
      // La suite etait dans un `if (ok)`, et ca donnait une panne muette :
      // l'appairage reussissait — jeton obtenu, serveur satisfait — puis le
      // plugin ne connectait RIEN. Ni WebSocket live, ni acces Studio, ni
      // identite du compte. L'operateur voyait l'ecran de code disparaitre et
      // plus rien se produire.
      //
      // Invisible sous Windows, ou DPAPI reussit toujours. Revele par le port
      // Linux : dans un conteneur il n'y a pas de service de trousseau
      // (QtKeychain sans Secret Service), donc l'ecriture echoue a chaque fois
      // et l'appairage ne servait a rien.
      //
      // La seule consequence d'un echec d'ecriture est qu'il faudra se
      // reconnecter au prochain demarrage d'OBS — ce que TokenStore a deja
      // journalise. Ca ne justifie pas de refuser la session en cours.
      liveClient_->setDeviceId(tokenStore_->cached().deviceId);
      // Session rouverte : la reprise apres echec repart de son plus court delai.
      if (sessionRetryTimer_) sessionRetryTimer_->stop();
      sessionRetryDelayMs_ = 30000;
      // Liaison ouverte : renouvelée sur place (`auth_renew`), jamais rouverte.
      liveClient_->connectWithToken(accessToken());
      // Liaison deja enregistree = simple renouvellement du jeton (toutes les
      // ~13 min) : acces, flux, fonctions et identite sont pousses par le
      // serveur, rien a relire. Sinon l'instantane de l'enregistrement les
      // apporte ; le filet HTTP ne part que s'il n'arrive pas.
      if (!liveClient_->isRegistered()) armInitialFetchFallback();
      if (!ok) {
        blog(LOG_WARNING,
             "[semirprod] session active mais jeton non persiste — "
             "reconnexion necessaire au prochain demarrage d'OBS");
      }
    });
  });

  // deviceFlowFailed fires for both interactive pairing failures (user
  // clicked Connect, server unreachable / endpoint 404 / etc.) and silent
  // refresh failures from loadToken. We do NOT clear the cached bundle —
  // the refresh_token is usually still valid; the failure is typically a
  // network blip. armSessionRetry() retries with a growing delay.
  // For interactive failures we also surface the message inline in the
  // pairing area so the user actually sees that something went wrong
  // instead of perceiving "the button does nothing".
  // Session refusee DEFINITIVEMENT (jeton revoque, expire, ou emis par un autre
  // environnement). C'est le seul cas ou l'on efface : sans ca, le plugin
  // gardait des jetons morts, reessayait en boucle, et l'interface restait sur
  // « connecte » — puisque cet etat ne dit que « une chaine de jeton existe ».
  //
  // disconnectAccount() est le meme chemin que le bouton Deconnecter : il vide
  // le coffre, coupe le client live (donc la boucle de reconnexion en 401), et
  // remet l'etat a zero. L'interface web retombe alors sur l'ecran d'appairage.
  connect(authFlow_, &DeviceAuthFlow::sessionRevoked, this, [this](const QString& reason) {
    blog(LOG_WARNING, "[semirprod-dock] session revoquee — deconnexion et retour a l'appairage");
    disconnectAccount();
    // APRES disconnectAccount : il ecrit « Plugin deconnecte. », or ce qu'il
    // faut lire ici c'est pourquoi.
    setStatus(reason);
    updateUiState();
    broadcastToWeb();
  });

  connect(authFlow_, &DeviceAuthFlow::pluginBlocked, this, [this]() { enterPluginBlocked(); });

  connect(authFlow_, &DeviceAuthFlow::deviceFlowFailed, this, [this](const QString& message) {
    // Echec d'un renouvellement silencieux (liaison deja enregistree) : on
    // retente plus tard. Un appairage interactif rate n'a pas de liaison, il
    // attend le prochain geste.
    if (tokenStore_->cached().isBound()) armSessionRetry();
    setStatus(message);
    // updateUiState() reecrit accountLabel_, donc on ecrit APRES. Ainsi un
    // echec de rafraichissement silencieux sur une session connectee garde le
    // libelle « Compte connecte », tandis qu'un echec d'appairage interactif
    // affiche l'erreur a la place de l'etat vide par defaut.
    updateUiState();
    if (accessToken().isEmpty()) {
      // Texte simple : l'interface web le rend tel quel. Seule la premiere
      // ligne est conservee — les suivantes portent l'apercu du corps HTTP,
      // utile au support mais illisible pour l'operateur, et le log OBS les a
      // deja (DeviceAuthFlow les y ecrit).
      const QString headline = message.split(QChar('\n')).first().trimmed();
      accountLabel_ = headline.isEmpty()
          ? loc("Account.PairingFailed")
          : loc("Account.PairingFailedDetail").arg(headline);
      accountUrl_.clear();
      pairingFailed_ = true;
      broadcastToWeb();
    }
  });

  connect(liveClient_, &LiveClient::connectionChanged, this, [this](bool connected) {
    // updateUiState() diffuse l'etat : l'interface web derive sa pastille de
    // liaison de `wsConnected`, il n'y a plus de pastille native a peindre.
    updateUiState();
    if (connected) {
      logEvent(QStringLiteral("success"),
               loc("Journal.LinkUp"));
      registerSelectedStream();
      // L'acces, les flux, les fonctions et l'identite arrivent dans
      // l'instantane de `register_instance_result`. Le filet HTTP ne part que
      // s'il ne vient pas (pont ancien, enregistrement refuse).
      armInitialFetchFallback();
    } else {
      logEvent(QStringLiteral("warn"),
               loc("Journal.LinkDown"));
    }
  });
  // The WS reconnect loop stalled — almost always an expired/rejected bearer
  // being re-sent verbatim. Refresh the token now (idempotent, gated on a
  // present refresh_token + no in-flight flow) so the socket reopens with a
  // fresh credential via deviceFlowSucceeded → connectWithToken. This shrinks
  // recovery from the old 5-minute watchdog down to a couple of seconds,
  // which is the "backend blocked while logged in, had to deco/reco" case.
  connect(liveClient_, &LiveClient::reconnectStalled, this, [this]() {
    if (authFlow_->isRunning()) return;
    const TokenBundle b = tokenStore_->cached();
    if (b.refreshToken.isEmpty()) return;  // nothing to refresh — let backoff ride
    blog(LOG_INFO, "[semirprod-dock] WS reconnect stalled — refreshing token");
    authFlow_->refresh(b);
  });
  connect(liveClient_, &LiveClient::statusMessage, this, &SemirprodDock::setStatus);

  // Le pont demande un jeton neuf (filet 4001 ou `auth_renew` refusé).
  connect(liveClient_, &LiveClient::registrationChanged, this, [this](bool registered) {
    updateUiState();
    broadcastToWeb();
    // Poste (re)enregistre : LiveClient a oublie ses empreintes, ces trois
    // envois partent donc en entier. C'est l'etat complet que le pont attend
    // apres une reconnexion ; ensuite, seuls les changements partent.
    if (registered) {
      pushObsState();
      pushSourcesState();
      pushAudioState();
    }
  });

  connect(liveClient_, &LiveClient::registrationResult, this,
          &SemirprodDock::applyRegistrationResult);

  connect(liveClient_, &LiveClient::tokenRenewalNeeded, this, [this]() {
    if (authFlow_->isRunning()) return;
    const TokenBundle b = tokenStore_->cached();
    if (b.isBound()) authFlow_->refresh(b);
  });

  // Refus DÉFINITIF du pont : aucune boucle, la raison à l'écran, un geste.
  connect(liveClient_, &LiveClient::sessionRefused, this, [this](const QString& reason) {
    blog(LOG_WARNING, "[semirprod-dock] poste refuse par le serveur : %s",
         reason.toUtf8().constData());
    if (reason == QStringLiteral("identity_changed")) {
      // Jeton d'une autre identité sur cette liaison : on rouvre une session
      // propre avec la liaison enregistrée.
      const TokenBundle b = tokenStore_->cached();
      if (b.isBound() && !authFlow_->isRunning()) authFlow_->refresh(b);
      return;
    }
    if (reason == QStringLiteral("plugin_blocked")) {
      enterPluginBlocked();
      return;
    }
    if (reason == QStringLiteral("update_required")) {
      setStatus(QStringLiteral("Cette version du plugin n'est plus prise en charge. Mets-le à jour."));
      enterPluginBlocked(BlockReason::UpdateRequired);
      updateUiState();
      return;
    }
    // device_revoked : la liaison locale ne sert plus à rien.
    clearLocalBinding();
    setStatus(QStringLiteral("Ce poste a été retiré de ton espace Semirprod. Reconnecte ton compte."));
    updateUiState();
    broadcastToWeb();
  });
  // LiveClient::wsTraffic n'a plus d'abonne : son unique lecteur etait le
  // journal de trames du dock, invisible. Les evenements metier, eux, partent
  // toujours par logEvent() vers le dock Journal et le log OBS.
  connect(liveClient_, &LiveClient::publishStateChanged, this, [this](const QString& id, bool pub) {
    // Toutes les sources du flux, punaisees comprises, pas seulement celle du
    // poste. 500 ms : le serveur annonce la publication a l'instant ou il
    // l'accepte, une lecture ouverte trop tot serait encore refusee.
    if (pub) {
      const std::string streamId = id.toStdString();
      QTimer::singleShot(500, this, [streamId]() { kickStalledInstances(streamId); });
    }
    if (id == settings_->selectedStreamId()) {
      const bool wasPublishing = lastPublishing_;
      lastPublishing_ = pub;
      if (pub) {
        maybeFlipToLive();
      } else if (wasPublishing) {
        cancelPendingFlipToLive();
        maybeFlipToBug();
      }
    }
  });
  // Quality monitor pushes a quality_state event whenever the ingest's
  // bitrate or freeze profile crosses threshold. Treat it the same way as a
  // publish drop so the dock flips to Bug while the publish session is still
  // technically alive — degraded ingest is just as bad on-air as no ingest.
  connect(liveClient_, &LiveClient::qualityStateChanged, this,
          [this](const QString& id, bool ok, const QString& reason, int bitrateKbps) {
            if (id != settings_->selectedStreamId()) return;
            blog(LOG_INFO,
                 "[semirprod] quality %s reason=%s bitrate=%dkbps",
                 ok ? "ok" : "degraded",
                 reason.toUtf8().constData(),
                 bitrateKbps);
            if (ok) {
              if (lastPublishing_) maybeFlipToLive();
            } else {
              cancelPendingFlipToLive();
              maybeFlipToBug();
            }
          });

  connect(pendingFlipTimer_, &QTimer::timeout, this, [this]() {
    if (!lastPublishing_) {
      cancelPendingFlipToLive();
      return;
    }
    // Si l'opérateur est passé sur une scène autre que Live/Bug pendant
    // l'attente du flux SRT, on annule la bascule auto.
    const QString current = sceneController_->currentScene();
    if (current != settings_->sceneLive() && current != settings_->sceneBug()) {
      cancelPendingFlipToLive();
      return;
    }
    if (sceneController_->isSemirprodSourceReadyInScene(settings_->sceneLive())) {
      pendingFlipTimer_->stop();
      pendingFlipDeadlineMs_ = 0;
      sceneController_->switchToScene(settings_->sceneLive());
      setStatus(QStringLiteral("Flux reçu, bascule sur la scène live."));
      return;
    }
    if (QDateTime::currentMSecsSinceEpoch() > pendingFlipDeadlineMs_) {
      pendingFlipTimer_->stop();
      pendingFlipDeadlineMs_ = 0;
      setStatus(QStringLiteral("Délai dépassé : aucun flux reçu sur la scène live."));
    }
  });

  // Stream-Deck control plane: dashboard publishes commands via Redis ->
  // bridge forwards them as WS messages -> LiveClient emits these signals.
  // Filtering by targetInstanceId already happens inside LiveClient.
  connect(liveClient_, &LiveClient::switchSceneRequested,
          this, &SemirprodDock::onSwitchSceneRequested);
  connect(liveClient_, &LiveClient::controlRequested,
          this, &SemirprodDock::onControlRequested);
  connect(liveClient_, &LiveClient::destinationRequested,
          this, &SemirprodDock::onDestinationRequested);
  connect(liveClient_, &LiveClient::verticalDestinationRequested,
          this, &SemirprodDock::onVerticalDestinationRequested);
  connect(verticalBridge_, &VerticalBridge::stateChanged,
          this, &SemirprodDock::pushObsState);
  connect(liveClient_, &LiveClient::multistreamDestinationsRequested,
          this, &SemirprodDock::onMultistreamDestinationsRequested);
  connect(liveClient_, &LiveClient::multistreamControlRequested,
          this, &SemirprodDock::onMultistreamControlRequested);
  connect(multistreamBridge_, &MultistreamBridge::stateChanged,
          this, &SemirprodDock::pushObsState);
  connect(liveClient_, &LiveClient::previewStreamStartRequested,
          this, &SemirprodDock::onPreviewStreamStartRequested);
  connect(liveClient_, &LiveClient::previewStreamStopRequested,
          this, &SemirprodDock::onPreviewStreamStopRequested);
  connect(liveClient_, &LiveClient::programPreviewStartRequested,
          this, &SemirprodDock::onProgramPreviewStartRequested);
  connect(liveClient_, &LiveClient::programPreviewStopRequested,
          this, &SemirprodDock::onProgramPreviewStopRequested);
  connect(liveClient_, &LiveClient::studioPreviewStartRequested,
          this, &SemirprodDock::onStudioPreviewStartRequested);
  connect(liveClient_, &LiveClient::studioPreviewStopRequested,
          this, &SemirprodDock::onStudioPreviewStopRequested);
  connect(liveClient_, &LiveClient::verticalPreviewStartRequested,
          this, &SemirprodDock::onVerticalPreviewStartRequested);
  connect(liveClient_, &LiveClient::verticalPreviewStopRequested,
          this, &SemirprodDock::onVerticalPreviewStopRequested);
  connect(liveClient_, &LiveClient::canvasPreviewStartRequested,
          this, &SemirprodDock::onCanvasPreviewStartRequested);
  connect(liveClient_, &LiveClient::canvasPreviewStopRequested,
          this, &SemirprodDock::onCanvasPreviewStopRequested);
  connect(liveClient_, &LiveClient::audioLevelsStartRequested,
          this, &SemirprodDock::onAudioLevelsStartRequested);
  connect(liveClient_, &LiveClient::audioLevelsStopRequested,
          this, &SemirprodDock::onAudioLevelsStopRequested);
  connect(liveClient_, &LiveClient::setStreamRequested,
          this, &SemirprodDock::onSetStreamRequested);
  connect(liveClient_, &LiveClient::setSceneMappingRequested,
          this, &SemirprodDock::onSetSceneMappingRequested);
  connect(liveClient_, &LiveClient::setProfileRequested,
          this, &SemirprodDock::onSetProfileRequested);
  connect(liveClient_, &LiveClient::createSceneCollectionRequested,
          this, &SemirprodDock::onCreateSceneCollectionRequested);
  connect(liveClient_, &LiveClient::setSceneCollectionRequested,
          this, &SemirprodDock::onSetSceneCollectionRequested);
  connect(liveClient_, &LiveClient::setStudioModeRequested,
          this, &SemirprodDock::onSetStudioModeRequested);
  connect(liveClient_, &LiveClient::setPreviewSceneRequested,
          this, &SemirprodDock::onSetPreviewSceneRequested);
  connect(liveClient_, &LiveClient::triggerTransitionRequested,
          this, &SemirprodDock::onTriggerTransitionRequested);
  // Source / audio control plane: dashboard publishes set_source_visible /
  // set_audio_mute / etc. via Redis -> bridge -> LiveClient. The dock applies
  // each command via SceneController and replies with sendSourceControlResult.
  // Demande EXPLICITE : l'empreinte est oubliee pour que l'etat parte meme
  // inchange (celui qui demande n'a peut-etre jamais recu le precedent).
  connect(liveClient_, &LiveClient::sourcesRefreshRequested, this, [this]() {
    liveClient_->forgetStateFingerprint(QStringLiteral("sources_state"));
    pushSourcesState();
  });
  connect(liveClient_, &LiveClient::audioRefreshRequested, this, [this]() {
    liveClient_->forgetStateFingerprint(QStringLiteral("audio_state"));
    pushAudioState();
  });
  connect(liveClient_, &LiveClient::stateRefreshRequested, this, [this]() {
    liveClient_->forgetStateFingerprint(QStringLiteral("obs_state"));
    pushObsState();
  });
  connect(liveClient_, &LiveClient::setSceneItemVisibleRequested, this,
          &SemirprodDock::onSetSceneItemVisible);
  connect(liveClient_, &LiveClient::setSceneItemLockedRequested, this,
          &SemirprodDock::onSetSceneItemLocked);
  connect(liveClient_, &LiveClient::setSourceMutedRequested, this,
          &SemirprodDock::onSetSourceMuted);
  connect(liveClient_, &LiveClient::setSourceVolumeRequested, this,
          &SemirprodDock::onSetSourceVolume);
  connect(liveClient_, &LiveClient::setSourceMonitoringRequested, this,
          &SemirprodDock::onSetSourceMonitoring);
  connect(liveClient_, &LiveClient::checkStudioAccessRequested, this,
          &SemirprodDock::checkStudioAccess);
  // Flux créé, renommé ou supprimé sur le site : relecture de la liste, une
  // seule pour une rafale de changements (250 ms).
  streamsRefreshTimer_ = new QTimer(this);
  streamsRefreshTimer_->setSingleShot(true);
  streamsRefreshTimer_->setInterval(250);
  connect(streamsRefreshTimer_, &QTimer::timeout, this, &SemirprodDock::refreshStreams);
  connect(liveClient_, &LiveClient::streamsChanged, this, [this]() {
    blog(LOG_INFO, "[semirprod-dock] streams_changed received, refreshing stream list");
    streamsRefreshTimer_->start();
  });
  connect(liveClient_, &LiveClient::streamDeleted, this, &SemirprodDock::onStreamDeleted);
  connect(liveClient_, &LiveClient::featuresChanged, this, &SemirprodDock::checkPluginFeatures);
  connect(liveClient_, &LiveClient::orgRenamed, this, [this](const QString& name) {
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty()) {
      refreshAccountIdentity();
      return;
    }
    if (trimmed == serverOrgName_) return;
    serverOrgName_ = trimmed;
    broadcastToWeb();
  });
  connect(liveClient_, &LiveClient::identityChanged, this, &SemirprodDock::refreshAccountIdentity);
  connect(liveClient_, &LiveClient::instanceRenamed, this, &SemirprodDock::onInstanceRenamed);
  // The leash also fires on connection drop: a closed WS means no one is
  // listening, so keep the screenshot pipeline idle until reconnect + a
  // fresh start_preview from the dashboard.
  connect(liveClient_, &LiveClient::connectionChanged, this, [this](bool ok) {
    if (!ok) {
      stopPreviewStream();
      // A closed WS means no one can refresh the lease — tear the WHIP output
      // down now instead of waiting for the lease timer, so a disconnect
      // immediately frees the regie uplink.
      stopProgramPreview();
      stopStudioPreview();
      stopVerticalPreview();
      stopCanvasPreview();
      // Plus personne pour rafraîchir le bail : on rend les volmètres tout de
      // suite plutôt que d'analyser chaque piste dans le vide.
      stopAudioLevels();
    }
  });

  // Signe de vie toutes les 5 s. Les trois etats sont RECALCULES localement a
  // chaque battement (lecture libobs, aucune E/S) mais ne partent sur le
  // reseau que si leur empreinte a change (LiveClient::sendStateIfChanged) :
  // le battement lui-meme ne porte plus d'etat.
  if (!heartbeatTimer_) {
    heartbeatTimer_ = new QTimer(this);
    heartbeatTimer_->setInterval(5000);
    connect(heartbeatTimer_, &QTimer::timeout, this, [this]() {
      liveClient_->sendInstanceHeartbeat();
      pushObsState();
      pushSourcesState();
      pushAudioState();
    });
    heartbeatTimer_->start();
  }

  // Streaming-output telemetry every 2 s. Always running; pushOutputStats()
  // itself gates on WS-connected + streaming-active and resets the delta
  // baseline when idle, so an idle timer tick is essentially free.
  if (!outputStatsTimer_) {
    outputStatsTimer_ = new QTimer(this);
    outputStatsTimer_->setInterval(2000);
    connect(outputStatsTimer_, &QTimer::timeout, this,
            &SemirprodDock::pushOutputStats);
    outputStatsTimer_->start();
  }

  // Plus de chien de garde periodique. Ce qu'il couvrait :
  //  - l'acces refuse relu toutes les 5 min : le serveur pousse desormais
  //    `check_studio_access` / `permissions_changed` quand l'abonnement change ;
  //  - la liaison qui ne revient pas : LiveClient se reconnecte seul (reprise
  //    exponentielle plafonnee a 30 s) et redemande un jeton neuf tant qu'il
  //    echoue (reconnectStalled) ; un renouvellement de session rate est
  //    retente par armSessionRetry().

  // Proactive token refresh (audit C2 fix). The previous build only refreshed
  // on a 401 mid-request, which left a gap where the user's first request
  // after expiry would fail with a cryptic "Impossible de récupérer les
  // flux" message. Now we check the cached bundle's expiry every 60 s and
  // trigger a refresh when we're within Config::refreshThresholdSeconds of
  // the deadline — well before the next /api/streams call. The 401 retry
  // path stays as a safety net for clock skew / unexpected early expiry.
  // Poste v2 : l'expiration est lue DANS le JWT (15 min), et le jeton neuf
  // part sur la liaison ouverte (`auth_renew`). Un seul coup, arme a
  // l'echeance par armTokenRefresh() et rearme a chaque nouveau jeton.
  tokenRefreshTimer_->setSingleShot(true);
  connect(tokenStore_, &TokenStore::tokenChanged, this, &SemirprodDock::armTokenRefresh);
  // Trousseau : essais termines sans liaison, ou de nouveau lisible mais vide.
  connect(tokenStore_, &TokenStore::storeStateChanged, this, [this]() { updateUiState(); });
  connect(tokenRefreshTimer_, &QTimer::timeout, this, [this]() {
    // Un flux d'auth en cours se termine par save() -> tokenChanged, qui
    // rearme ce minuteur.
    if (authFlow_->isRunning()) return;
    const TokenBundle b = tokenStore_->cached();
    if (!b.isValid()) return;
    if (!b.isExpired(Config::sessionRenewBeforeSeconds)) {
      // Reveil en avance (horloge systeme ajustee) : on revise l'echeance.
      armTokenRefresh();
      return;
    }
    if (b.isBound()) {
      authFlow_->refresh(b);
      return;
    }
    if (b.refreshToken.isEmpty()) {
      // No way to refresh — surface it via status. The user has to manually
      // re-authenticate via the Connect button. Keep this quiet — we don't
      // want a nag every minute when the user is offline.
      if (liveClient_->isConnected()) {
        blog(LOG_WARNING,
             "[semirprod-dock] token expired and no refresh_token — re-auth required");
      }
      return;
    }
    blog(LOG_INFO,
         "[semirprod-dock] proactive token refresh (within %ds of expiry)",
         Config::refreshThresholdSeconds);
    authFlow_->refresh(b);
  });
  armTokenRefresh();

  // Filet de la liaison live. Constaté en prod le 31/10/2026 : un poste dont la
  // liaison était tombée ne se reconnectait plus jamais, compte toujours affiché
  // « connecté », et la régie ne le voyait plus. Chaque chemin de reprise
  // (fermeture 4001, reprise de session, reconnexion bloquée) abandonne sa
  // relance quand une authentification est déjà en cours, et la fermeture 4001
  // ne programme aucune reconnexion : il suffisait qu'un de ces relais se perde
  // pour que plus rien ne ramène la liaison.
  //
  // Ce minuteur ne lit rien et n'envoie rien tant que la liaison est vivante ou
  // qu'une reprise est en cours. Il n'agit que dans l'état sans issue : compte
  // lié, liaison coupée, aucune reconnexion programmée.
  if (!liveSupervisorTimer_) {
    liveSupervisorTimer_ = new QTimer(this);
    liveSupervisorTimer_->setInterval(30000);
    connect(liveSupervisorTimer_, &QTimer::timeout, this, &SemirprodDock::superviseLiveLink);
    liveSupervisorTimer_->start();
  }
}

void SemirprodDock::superviseLiveLink() {
  if (shuttingDown_) return;
  const TokenBundle b = tokenStore_->cached();
  if (!b.isBound() || liveClient_->isConnected() || liveClient_->isHalted() ||
      liveClient_->isRefused()) {
    liveStuckTicks_ = 0;
    return;
  }
  // Une reconnexion est programmée ou un socket s'ouvre : la boucle de
  // LiveClient s'en occupe, et redemande elle-même un jeton si elle cale.
  if (liveClient_->isReconnecting()) {
    liveStuckTicks_ = 0;
    return;
  }
  // Un appairage interactif attend le streamer, qui tape son code : on ne
  // l'interrompt jamais.
  if (authFlow_->isPairing()) {
    liveStuckTicks_ = 0;
    return;
  }
  ++liveStuckTicks_;
  if (authFlow_->isRunning()) {
    // Une authentification se termine en 30 s au pire (délai d'inactivité du
    // client HTTP). Au delà de deux minutes, elle ne rendra plus la main :
    // on l'abandonne pour en relancer une.
    if (liveStuckTicks_ < 4) return;
    blog(LOG_WARNING,
         "[semirprod-dock] authentification bloquee depuis %d s, liaison live absente : relance",
         liveStuckTicks_ * 30);
    authFlow_->cancel();
  }
  // Premier constat, puis toutes les deux minutes tant que ça ne revient pas :
  // jamais une rafale contre le serveur d'auth.
  if (liveStuckTicks_ != 1 && liveStuckTicks_ % 4 != 0) return;
  blog(LOG_WARNING,
       "[semirprod-dock] liaison live coupee sans reprise en cours : nouvelle session");
  authFlow_->refresh(b);
}

void SemirprodDock::armTokenRefresh() {
  if (!tokenRefreshTimer_ || shuttingDown_) return;
  tokenRefreshTimer_->stop();
  const TokenBundle b = tokenStore_->cached();
  if (!b.isValid() || !b.expiresAt.isValid()) return;
  // La plus grande des deux marges : celle du plugin et celle annoncee par le
  // pont dans `hello`.
  const qint64 renewBeforeMs =
      static_cast<qint64>(std::max(Config::sessionRenewBeforeSeconds,
                                   liveClient_->renewBeforeSeconds())) * 1000;
  const qint64 dueMs =
      QDateTime::currentDateTimeUtc().msecsTo(b.expiresAt) - renewBeforeMs;
  // Borne basse : un jeton deja dans la fenetre part tout de suite (1 s).
  // Borne haute : QTimer prend un int.
  const qint64 delayMs = std::clamp<qint64>(dueMs, 1000, 24LL * 3600 * 1000);
  tokenRefreshTimer_->start(static_cast<int>(delayMs));
}

void SemirprodDock::armSessionRetry() {
  if (shuttingDown_) return;
  if (!sessionRetryTimer_) {
    sessionRetryTimer_ = new QTimer(this);
    sessionRetryTimer_->setSingleShot(true);
    connect(sessionRetryTimer_, &QTimer::timeout, this, [this]() {
      if (shuttingDown_ || authFlow_->isRunning()) return;
      const TokenBundle b = tokenStore_->cached();
      if (!b.isBound()) return;
      if (liveClient_->isConnected()) {
        // Liaison vivante : seul le jeton est a renouveler, s'il le faut encore.
        if (b.isExpired(Config::sessionRenewBeforeSeconds)) authFlow_->refresh(b);
        return;
      }
      // Liaison deja en memoire : on repart d'elle, sans relire le trousseau.
      blog(LOG_INFO, "[semirprod-dock] reprise de session apres echec");
      resumeWithBundle(b);
    });
  }
  if (sessionRetryTimer_->isActive()) return;
  const int delayMs = sessionRetryDelayMs_;
  sessionRetryDelayMs_ = std::min(sessionRetryDelayMs_ * 2, 300000);
  sessionRetryTimer_->start(delayMs);
}

void SemirprodDock::onSwitchSceneRequested(
    const QString& requestId,
    const QString& streamId,
    const QString& target,
    const QDateTime& /*expiresAt*/) {
  const qint64 startMs = QDateTime::currentMSecsSinceEpoch();
  const QString activeBefore = sceneController_->currentScene();
  const bool ok = sceneController_->switchToScene(target);
  const QString activeAfter = sceneController_->currentScene();
  logEvent(ok ? QStringLiteral("info") : QStringLiteral("error"),
           ok ? loc("Journal.RemoteScene").arg(target)
              : loc("Journal.RemoteSceneRefused").arg(target));
  const qint64 latency = QDateTime::currentMSecsSinceEpoch() - startMs;
  liveClient_->sendSwitchResult(
      requestId,
      streamId,
      target,
      ok ? QStringLiteral("success") : QStringLiteral("failed"),
      ok ? QString() : QStringLiteral("scene_not_found"),
      activeAfter,
      ok ? target : activeBefore,
      latency);
  pushObsState();
}

void SemirprodDock::onDestinationRequested(const QString& requestId,
                                           const QString& rtmpUrl,
                                           const QString& streamKey) {
  const QString action = QStringLiteral("set_destination");

  // Hote en place AVANT la commande : sert a dire, sur le poste, si elle l'a
  // change. Pas de reference prise (obs_frontend_get_streaming_service).
  QString previousServer;
  if (obs_service_t* current = obs_frontend_get_streaming_service()) {
    const char* url =
        obs_service_get_connect_info(current, OBS_SERVICE_CONNECT_INFO_SERVER_URL);
    previousServer = QString::fromUtf8(url ? url : "");
  }

  // L'ordre compte. La destination d'abord : elle refuse en direct, et c'est
  // ce refus qui doit remonter au site. Poser le débit avant rendrait un
  // « débit appliqué, destination refusée » que rien à l'écran ne sait dire.
  const bool applied = sceneController_->applyStreamingService(rtmpUrl, streamKey);
  if (!applied) {
    // Le seul motif de refus de applyStreamingService est la diffusion en
    // cours (l'absence de service ne se produit que sur un OBS cassé). Le site
    // affiche « arrête le direct pour changer de destination », pas une erreur
    // technique que personne ne peut traiter.
    sendDestinationResult(requestId, action, false,
                          QStringLiteral("streaming_active"));
    return;
  }

  // Le débit est reposé À CHAQUE application de destination, et pas seulement
  // au démarrage de l'instance (édition cloud seulement : ailleurs l'appel ne
  // fait rien, voir edition::pinsStreamingBitrate). Une collection de scènes importée par le client
  // apporte son propre profil, donc son propre débit : sans ce rappel, un
  // import suffirait à repartir à 2500 kbps ou à 12000 sans que personne ne le
  // demande.
  sceneController_->pinStreamingBitrate();

  const QString newHost = QUrl(rtmpUrl).host().toLower();
  noticeRemoteDestination("Journal.RemoteDestination", QStringList{newHost},
                          newHost != QUrl(previousServer).host().toLower());

  sendDestinationResult(requestId, action, true, QString());
  pushObsState();
}

void SemirprodDock::onVerticalDestinationRequested(const QString& requestId,
                                                   const QString& rtmpUrl,
                                                   const QString& streamKey,
                                                   bool followStream) {
  // Même débit que la sortie principale : 6000 kbps (pinStreamingBitrate).
  constexpr int kVerticalBitrateKbps = 6000;
  const QString previousHost = QUrl(verticalBridge_->state().server).host().toLower();
  QString error;
  const bool ok = verticalBridge_->setDestination(
      rtmpUrl, streamKey, kVerticalBitrateKbps, followStream, &error);
  if (ok) {
    const QString newHost = QUrl(rtmpUrl).host().toLower();
    noticeRemoteDestination("Journal.RemoteVerticalDestination", QStringList{newHost},
                            newHost != previousHost);
  }
  // Le motif part au site tel quel : `vertical_unavailable` (plugin absent)
  // ou le message de Vertical, qui ne contient jamais la clé.
  sendDestinationResult(requestId, QStringLiteral("set_vertical_destination"),
                        ok, ok ? QString() : error);
  pushObsState();
}

void SemirprodDock::onMultistreamDestinationsRequested(const QString& requestId,
                                                       const QJsonArray& destinations) {
  QString error;
  const bool ok = multistreamBridge_->setDestinations(destinations, &error);
  if (ok) {
    // Hotes seulement, lus sous les noms de champ possibles ; jamais la cle.
    static const char* const kUrlFields[] = {"server", "rtmpUrl", "url"};
    QStringList hosts;
    for (const QJsonValue& value : destinations) {
      const QJsonObject dest = value.toObject();
      for (const char* field : kUrlFields) {
        const QString host =
            QUrl(dest.value(QLatin1String(field)).toString()).host().toLower();
        if (!host.isEmpty()) {
          if (!hosts.contains(host)) hosts.append(host);
          break;
        }
      }
    }
    noticeRemoteDestination("Journal.RemoteMultistreamDestinations", hosts,
                            /*hostChanged=*/true);
  }
  // Le motif part tel quel : multistream_unavailable, streaming_active ou le
  // message de Multistream, qui ne contient jamais de cle.
  sendDestinationResult(requestId, QStringLiteral("set_multistream_destinations"), ok,
                        ok ? QString() : error);
  pushObsState();
}

void SemirprodDock::onMultistreamControlRequested(const QString& requestId,
                                                  const QString& action,
                                                  const QString& destinationId) {
  QString error;
  bool ok = true;
  if (action == QStringLiteral("start_multistream")) {
    ok = multistreamBridge_->start(destinationId, &error);
  } else {
    multistreamBridge_->stop(destinationId);
  }
  liveClient_->sendControlResult(requestId, action,
                                 ok ? QStringLiteral("success") : QStringLiteral("failed"),
                                 ok ? QString() : error);
  pushObsState();
}

void SemirprodDock::sendDestinationResult(const QString& requestId,
                                          const QString& action,
                                          bool ok,
                                          const QString& reason) {
  liveClient_->sendControlResult(
      requestId,
      action,
      ok ? QStringLiteral("success") : QStringLiteral("failed"),
      reason);
}

void SemirprodDock::onControlRequested(const QString& action, const QString& requestId) {
  bool ok = false;
  QString reason;
  if (action == QStringLiteral("start_stream")) {
    ok = sceneController_->startStreaming();
  } else if (action == QStringLiteral("stop_stream")) {
    ok = sceneController_->stopStreaming();
  } else if (action == QStringLiteral("start_record")) {
    ok = sceneController_->startRecording();
  } else if (action == QStringLiteral("stop_record")) {
    ok = sceneController_->stopRecording();
  } else if (action == QStringLiteral("start_vertical_stream")) {
    ok = verticalBridge_->startStream(&reason);
  } else if (action == QStringLiteral("stop_vertical_stream")) {
    verticalBridge_->stopStream();
    ok = true;
  } else if (action == QStringLiteral("get_preview")) {
    // Meme regle que les cinq autres chemins d'apercu : l'operateur qui a coupe
    // l'apercu ne doit pas voir partir une capture du programme pour autant.
    if (!settings_->previewEnabled()) {
      liveClient_->sendControlResult(requestId, action, QStringLiteral("failed"),
                                     QStringLiteral("preview_disabled"));
      return;
    }
    // Une seule capture a la fois : chacune coute trois rendus d'OBS et un
    // encodage JPEG, et des demandes empilees se paieraient sur le direct.
    if (previewCaptureInFlight_) {
      liveClient_->sendControlResult(requestId, action, QStringLiteral("failed"),
                                     QStringLiteral("preview_busy"));
      return;
    }
    previewCaptureInFlight_ = true;
    markRemoteCapture();
    // Screenshot is async (3 OBS render ticks + thread hop). Bail out of the
    // shared sync result/state-push below — the lambda owns finalization.
    const QString streamId = settings_->selectedStreamId();
    QPointer<SemirprodDock> self(this);
    sceneController_->takeScreenshot(
        1280,
        80,
        [self, action, requestId, streamId](const QString& base64) {
          if (!self) return;
          self->previewCaptureInFlight_ = false;
          const bool jobOk = !base64.isEmpty();
          if (jobOk) self->liveClient_->sendPreviewFrame(streamId, base64);
          self->liveClient_->sendControlResult(
              requestId,
              action,
              jobOk ? QStringLiteral("success") : QStringLiteral("failed"),
              jobOk ? QString() : QStringLiteral("screenshot_failed"));
          self->pushObsState();
        });
    return;
  } else {
    reason = QStringLiteral("unknown_action");
  }
  liveClient_->sendControlResult(
      requestId,
      action,
      ok ? QStringLiteral("success") : QStringLiteral("failed"),
      reason);
  // Frontend events also push state, but emit one immediately so the
  // dashboard reflects the click even if the OBS event lags a tick.
  pushObsState();
}

void SemirprodDock::onPreviewStreamStartRequested(
    const QString& requestId, int fps, int quality, int width, int ttlMs) {
  if (!settings_->previewEnabled()) {
    liveClient_->sendControlResult(requestId, QStringLiteral("start_preview"),
                                   QStringLiteral("failed"),
                                   QStringLiteral("preview_disabled"));
    return;
  }
  // Cap the parameters defensively even if the dashboard sent absurd values.
  // The GPU-scaled capture pipeline sustains 24-30 fps at preview widths, so we
  // allow up to 30 (dashboard default 24). Quality below ~50 looks awful, above
  // ~90 doubles the bytes for no win.
  fps = std::clamp(fps, 1, 30);
  quality = std::clamp(quality, 50, 90);
  width = std::clamp(width, 320, 1920);
  ttlMs = std::clamp(ttlMs, 1000, 120000);

  previewQuality_ = quality;
  previewWidth_ = width;
  previewExpiresAtMs_ = QDateTime::currentMSecsSinceEpoch() + ttlMs;

  if (!previewTimer_) {
    previewTimer_ = new QTimer(this);
    connect(previewTimer_, &QTimer::timeout, this, &SemirprodDock::onPreviewTick);
  }
  // setInterval restarts the timer cleanly even when called on a running
  // timer, so repeated start_preview calls (the dashboard refreshes the
  // lease) just adjust cadence without skipping a tick.
  const int intervalMs = std::max(33, 1000 / fps);
  if (previewTimer_->interval() != intervalMs) {
    previewTimer_->setInterval(intervalMs);
  }
  if (!previewTimer_->isActive()) {
    previewTimer_->start();
    // Send one frame immediately so the dashboard doesn't sit on a blank
    // panel for up to 1/fps seconds after the user enables the preview.
    onPreviewTick();
  }
  refreshRemotePreviewIndicator();

  liveClient_->sendControlResult(
      requestId,
      QStringLiteral("start_preview"),
      QStringLiteral("success"),
      QString());
}

void SemirprodDock::onPreviewStreamStopRequested(const QString& requestId) {
  stopPreviewStream();
  liveClient_->sendControlResult(
      requestId,
      QStringLiteral("stop_preview"),
      QStringLiteral("success"),
      QString());
}

void SemirprodDock::stopPreviewStream() {
  if (previewTimer_) previewTimer_->stop();
  previewExpiresAtMs_ = 0;
  refreshRemotePreviewIndicator();
  // previewInFlightCount_ is left to drain naturally as the in-flight captures
  // return — we don't want a late callback to fire sendPreviewFrame after stop.
  // The callback's `previewExpiresAtMs_ == 0` guard handles that path.
}

void SemirprodDock::onPreviewTick() {
  // Lease expired (dashboard closed without sending stop_preview, or just
  // never refreshed). Bail out — re-enabling the panel will issue a fresh
  // start_preview and we'll resume.
  if (QDateTime::currentMSecsSinceEpoch() > previewExpiresAtMs_) {
    stopPreviewStream();
    return;
  }
  if (!liveClient_->isConnected()) return;
  // A capture spans ~3 OBS render ticks. Allowing 2 in flight lets that latency
  // pipeline (tick N+1 starts while tick N is still staging/encoding) instead of
  // halving throughput; past 2 we'd just amplify latency, so back-pressure here.
  constexpr int kMaxPreviewInFlight = 2;
  if (previewInFlightCount_ >= kMaxPreviewInFlight) return;

  const QString streamId = settings_->selectedStreamId();
  if (streamId.isEmpty()) return;

  const quint64 seq = ++previewSeqNext_;
  ++previewInFlightCount_;
  QPointer<SemirprodDock> self(this);
  sceneController_->takeScreenshot(
      previewWidth_,
      previewQuality_,
      [self, streamId, seq](const QString& base64) {
        if (!self) return;
        if (self->previewInFlightCount_ > 0) --self->previewInFlightCount_;
        // The user (or the leash) may have stopped the stream while this
        // capture was in flight — don't push a frame nobody asked for.
        if (self->previewExpiresAtMs_ == 0) return;
        if (base64.isEmpty()) return;
        // Ordered delivery: with 2 captures in flight the JPEG encodes can
        // complete out of order. Only forward a frame newer than the last one
        // sent; drop late arrivals so the dashboard never jumps backwards.
        if (seq <= self->previewSeqLastSent_) return;
        self->previewSeqLastSent_ = seq;
        self->liveClient_->sendPreviewFrame(streamId, base64);
      });
}

void SemirprodDock::onProgramPreviewStartRequested(
    const QString& requestId, const QString& whipUrl,
    const QString& publishToken, int ttlMs, int width, int height, int fps,
    int bitrateKbps) {
  if (!settings_->previewEnabled()) {
    liveClient_->sendControlResult(
        requestId, QStringLiteral("start_program_preview"),
        QStringLiteral("failed"), QStringLiteral("preview_disabled"));
    return;
  }

  // Guard even though the capability isn't advertised on unsupported builds:
  // a stale/misrouted command must never half-start an output we can't honour.
  if (!ProgramPreviewOutput::isSupported()) {
    liveClient_->sendControlResult(
        requestId, QStringLiteral("start_program_preview"),
        QStringLiteral("failed"), QStringLiteral("whip_unsupported"));
    return;
  }

  // Clamp the lease the same way the screenshot leash does, so a bogus ttl
  // can't pin the output on forever or expire instantly.
  ttlMs = std::clamp(ttlMs, 1000, 120000);

  const bool ok =
      programPreview_->start(whipUrl, publishToken, width, height, fps, bitrateKbps);

  if (ok) {
    // (Re)arm the lease. Single-shot: a refresh (same params, new start) just
    // pushes the deadline out; no refresh within ttlMs stops the output.
    if (!programPreviewLeaseTimer_) {
      programPreviewLeaseTimer_ = new QTimer(this);
      programPreviewLeaseTimer_->setSingleShot(true);
      connect(programPreviewLeaseTimer_, &QTimer::timeout, this, [this]() {
        warmStopProgramPreview("lease expired");
      });
    }
    programPreviewLeaseTimer_->start(ttlMs);
    // Un start/refresh annule le teardown differe : l'output repart pour un
    // cycle de lease complet.
    if (programPreviewWarmTimer_) programPreviewWarmTimer_->stop();
  }
  refreshRemotePreviewIndicator();

  liveClient_->sendControlResult(
      requestId, QStringLiteral("start_program_preview"),
      ok ? QStringLiteral("success") : QStringLiteral("failed"),
      ok ? QString() : QStringLiteral("whip_start_failed"));
}

void SemirprodDock::onProgramPreviewStopRequested(const QString& requestId) {
  warmStopProgramPreview("stop requested");
  liveClient_->sendControlResult(
      requestId, QStringLiteral("stop_program_preview"),
      QStringLiteral("success"), QString());
}

void SemirprodDock::onStudioPreviewStartRequested(
    const QString& requestId, const QString& whipUrl,
    const QString& publishToken, int ttlMs, int width, int height, int fps,
    int bitrateKbps) {
  const QString action = QStringLiteral("start_studio_preview");
  if (!settings_->previewEnabled()) {
    liveClient_->sendControlResult(requestId, action, QStringLiteral("failed"),
                                   QStringLiteral("preview_disabled"));
    return;
  }
  if (!ProgramPreviewOutput::isSupported()) {
    liveClient_->sendControlResult(requestId, action, QStringLiteral("failed"),
                                   QStringLiteral("whip_unsupported"));
    return;
  }
  // Le deuxieme mix n'a de sens qu'en mode studio : sans lui il n'y a pas de
  // scene de preparation, et l'apercu doublerait le programme pour rien.
  if (!sceneController_->isStudioModeEnabled()) {
    liveClient_->sendControlResult(requestId, action, QStringLiteral("failed"),
                                   QStringLiteral("studio_mode_off"));
    return;
  }

  video_t* mix = studioMix_ ? studioMix_->ensure() : nullptr;
  if (!mix) {
    liveClient_->sendControlResult(requestId, action, QStringLiteral("failed"),
                                   QStringLiteral("preview_mix_unavailable"));
    return;
  }
  studioPreview_->setVideoMix(mix);

  ttlMs = std::clamp(ttlMs, 1000, 120000);
  const bool ok = studioPreview_->start(whipUrl, publishToken, width, height,
                                        fps, bitrateKbps);
  if (ok) {
    if (!studioPreviewLeaseTimer_) {
      studioPreviewLeaseTimer_ = new QTimer(this);
      studioPreviewLeaseTimer_->setSingleShot(true);
      connect(studioPreviewLeaseTimer_, &QTimer::timeout, this, [this]() {
        blog(LOG_INFO,
             "[semirprod-dock] bail apercu preparation expire - arret");
        stopStudioPreview();
      });
    }
    studioPreviewLeaseTimer_->start(ttlMs);
  } else {
    // Echec du demarrage : rendre le mix tout de suite, sinon une vue composee
    // en continu resterait derriere une sortie qui ne tourne pas.
    stopStudioPreview();
  }
  refreshRemotePreviewIndicator();

  liveClient_->sendControlResult(requestId, action,
                                 ok ? QStringLiteral("success")
                                    : QStringLiteral("failed"),
                                 ok ? QString()
                                    : QStringLiteral("whip_start_failed"));
}

void SemirprodDock::onStudioPreviewStopRequested(const QString& requestId) {
  stopStudioPreview();
  liveClient_->sendControlResult(requestId,
                                 QStringLiteral("stop_studio_preview"),
                                 QStringLiteral("success"), QString());
}

void SemirprodDock::onVerticalPreviewStartRequested(
    const QString& requestId, const QString& whipUrl,
    const QString& publishToken, int ttlMs, int width, int height, int fps,
    int bitrateKbps) {
  const QString action = QStringLiteral("start_vertical_preview");
  if (!settings_->previewEnabled()) {
    liveClient_->sendControlResult(requestId, action, QStringLiteral("failed"),
                                   QStringLiteral("preview_disabled"));
    return;
  }
  if (!VerticalBridge::previewSupported()) {
    liveClient_->sendControlResult(requestId, action, QStringLiteral("failed"),
                                   QStringLiteral("vertical_unavailable"));
    return;
  }

  ttlMs = std::clamp(ttlMs, 1000, 120000);
  QString reason;
  const bool ok = verticalBridge_->startPreview(whipUrl, publishToken, width,
                                                height, fps, bitrateKbps,
                                                &reason);
  if (ok) {
    verticalPreviewActive_ = true;
    if (!verticalPreviewLeaseTimer_) {
      verticalPreviewLeaseTimer_ = new QTimer(this);
      verticalPreviewLeaseTimer_->setSingleShot(true);
      connect(verticalPreviewLeaseTimer_, &QTimer::timeout, this, [this]() {
        blog(LOG_INFO, "[semirprod-dock] bail apercu vertical expire - arret");
        stopVerticalPreview();
      });
    }
    verticalPreviewLeaseTimer_->start(ttlMs);
  } else {
    stopVerticalPreview();
  }
  refreshRemotePreviewIndicator();

  liveClient_->sendControlResult(
      requestId, action, ok ? QStringLiteral("success") : QStringLiteral("failed"),
      ok ? QString()
         : (reason.isEmpty() ? QStringLiteral("whip_start_failed") : reason));
}

void SemirprodDock::onVerticalPreviewStopRequested(const QString& requestId) {
  stopVerticalPreview();
  liveClient_->sendControlResult(requestId,
                                 QStringLiteral("stop_vertical_preview"),
                                 QStringLiteral("success"), QString());
}

void SemirprodDock::stopVerticalPreview() {
  if (verticalPreviewLeaseTimer_) verticalPreviewLeaseTimer_->stop();
  // Rien a dire a Vertical si on ne lui a rien demande : evite un appel de
  // proc (et un aller-retour vers son fil UI) a chaque deconnexion.
  if (!verticalPreviewActive_) return;
  verticalPreviewActive_ = false;
  if (verticalBridge_) verticalBridge_->stopPreview();
  refreshRemotePreviewIndicator();
}

void SemirprodDock::onCanvasPreviewStartRequested(
    const QString& requestId, const QString& canvas, const QString& whipUrl,
    const QString& publishToken, int ttlMs, int width, int height, int fps,
    int bitrateKbps) {
  const QString action = QStringLiteral("start_canvas_preview");
  if (!settings_->previewEnabled()) {
    liveClient_->sendControlResult(requestId, action, QStringLiteral("failed"),
                                   QStringLiteral("preview_disabled"));
    return;
  }
  if (!ProgramPreviewOutput::isSupported()) {
    liveClient_->sendControlResult(requestId, action, QStringLiteral("failed"),
                                   QStringLiteral("whip_unsupported"));
    return;
  }
  if (canvas.isEmpty()) {
    liveClient_->sendControlResult(requestId, action, QStringLiteral("failed"),
                                   QStringLiteral("canvas_not_found"));
    return;
  }

  // Un AUTRE canvas que celui en cours : on coupe l'ancien (sa référence part
  // en différé) et on repart de zéro. Même canvas : simple rafraîchissement,
  // start() garde la session WHIP si rien n'a changé.
  if (previewCanvas_ && previewCanvasName_ != canvas) stopCanvasPreview();

  if (!previewCanvas_) {
    obs_canvas_t* found = obs_get_canvas_by_name(canvas.toUtf8().constData());
    // Un canvas retiré (collection en cours de fermeture) reste trouvable tant
    // qu'une référence le tient : on ne s'y branche pas.
    if (found && obs_canvas_removed(found)) {
      obs_canvas_release(found);
      found = nullptr;
    }
    if (!found) {
      liveClient_->sendControlResult(requestId, action,
                                     QStringLiteral("failed"),
                                     QStringLiteral("canvas_not_found"));
      return;
    }
    previewCanvas_ = found;
    previewCanvasName_ = canvas;
    if (signal_handler_t* sh = obs_canvas_get_signal_handler(previewCanvas_))
      signal_handler_connect(sh, "remove", onPreviewCanvasRemovedSignal, this);
  }

  video_t* video = obs_canvas_get_video(previewCanvas_);
  if (!video) {
    stopCanvasPreview();
    liveClient_->sendControlResult(requestId, action, QStringLiteral("failed"),
                                   QStringLiteral("canvas_unavailable"));
    return;
  }

  // Taille absente : celle du canvas, ramenée à 1280 px sur le grand côté.
  // Sinon ProgramPreviewOutput retomberait sur 1280×720 et écraserait un 9:16.
  if (width <= 0 || height <= 0) {
    obs_video_info ovi{};
    if (obs_canvas_get_video_info(previewCanvas_, &ovi) && ovi.base_width > 0 &&
        ovi.base_height > 0) {
      const double scale = std::min(
          1.0, 1280.0 / static_cast<double>(std::max(ovi.base_width, ovi.base_height)));
      width = static_cast<int>(ovi.base_width * scale);
      height = static_cast<int>(ovi.base_height * scale);
    }
  }

  canvasPreview_->setVideoMix(video);
  ttlMs = std::clamp(ttlMs, 1000, 120000);
  const bool ok = canvasPreview_->start(whipUrl, publishToken, width, height,
                                        fps, bitrateKbps);
  if (ok) {
    if (!canvasPreviewLeaseTimer_) {
      canvasPreviewLeaseTimer_ = new QTimer(this);
      canvasPreviewLeaseTimer_->setSingleShot(true);
      connect(canvasPreviewLeaseTimer_, &QTimer::timeout, this, [this]() {
        blog(LOG_INFO, "[semirprod-dock] bail apercu canvas expire - arret");
        stopCanvasPreview();
      });
    }
    canvasPreviewLeaseTimer_->start(ttlMs);
  } else {
    stopCanvasPreview();
  }
  refreshRemotePreviewIndicator();

  liveClient_->sendControlResult(requestId, action,
                                 ok ? QStringLiteral("success")
                                    : QStringLiteral("failed"),
                                 ok ? QString()
                                    : QStringLiteral("whip_start_failed"));
}

void SemirprodDock::onCanvasPreviewStopRequested(const QString& requestId) {
  stopCanvasPreview();
  liveClient_->sendControlResult(requestId,
                                 QStringLiteral("stop_canvas_preview"),
                                 QStringLiteral("success"), QString());
}

void SemirprodDock::onPreviewCanvasRemovedSignal(void* data, calldata_t* cd) {
  // Émis sur le fil qui retire le canvas : on repasse par la boucle Qt. Le
  // canvas est comparé à l'arrivée, l'aperçu a pu changer de canvas entre-temps.
  auto* self = static_cast<SemirprodDock*>(data);
  auto* removed = static_cast<obs_canvas_t*>(calldata_ptr(cd, "canvas"));
  QPointer<SemirprodDock> guard(self);
  QMetaObject::invokeMethod(
      self,
      [guard, removed]() {
        if (!guard || guard->previewCanvas_ != removed) return;
        blog(LOG_INFO, "[semirprod-dock] canvas '%s' retire - arret de l'apercu",
             guard->previewCanvasName_.toUtf8().constData());
        guard->stopCanvasPreview();
      },
      Qt::QueuedConnection);
}

void SemirprodDock::stopCanvasPreview(bool force) {
  if (canvasPreviewLeaseTimer_) canvasPreviewLeaseTimer_->stop();

  // Même ordre que stopStudioPreview : la sortie (consommatrice) d'abord, le
  // mix (producteur) ensuite, et pas à la ligne suivante. stop() est
  // asynchrone, l'encodeur reste branché sur le mix du canvas pendant la
  // retraite de sa génération ; relâcher le canvas maintenant pouvait détruire
  // ce mix sous lui (obs_canvas_destroy -> obs_canvas_clear_mix).
  if (canvasPreview_) {
    canvasPreview_->stop(force);
    canvasPreview_->setVideoMix(nullptr);
  }
  refreshRemotePreviewIndicator();
  if (!previewCanvas_) return;

  if (signal_handler_t* sh = obs_canvas_get_signal_handler(previewCanvas_))
    signal_handler_disconnect(sh, "remove", onPreviewCanvasRemovedSignal, this);
  obs_canvas_t* mourant = previewCanvas_;
  previewCanvas_ = nullptr;
  previewCanvasName_.clear();

  if (force) {
    // Fermeture d'OBS : retraite synchrone, plus de boucle Qt pour un différé.
    obs_canvas_release(mourant);
    return;
  }
  // Le délai dépasse celui de la retraite des générations : le producteur
  // survit à son consommateur, jamais l'inverse.
  QTimer::singleShot(ProgramPreviewOutput::kRetireGraceMs + 1000, this,
                     [mourant]() { obs_canvas_release(mourant); });
}

void SemirprodDock::stopStudioPreview(bool force) {
  if (studioPreviewLeaseTimer_) studioPreviewLeaseTimer_->stop();

  // Ordre impose : la sortie (consommatrice) d'abord, le mix (producteur)
  // ensuite. Mais « ensuite » ne veut PAS dire « a la ligne suivante ».
  //
  // `stop()` rend la main immediatement : `obs_output_stop()` est asynchrone,
  // le fil de capture de la sortie tourne encore et reste branche sur le mix.
  // Detruire le mix dans la foulee tuait donc OBS depuis son propre fil
  // d'arret :
  //
  //   obs.dll!end_data_capture_thread
  //   obs.dll!remove_connection
  //   obs.dll!video_output_disconnect2
  //   w32-pthreads.dll!pthread_mutex_unlock   <- mutex d'un mix deja detruit
  //
  // (violation d'acces du 15 aout, 21:21:50, 1 s apres « Output
  // 'semirprod_studio_whip_g8': stopping ».) L'en-tete de PreviewSceneMix
  // l'annoncait pourtant : tout encodeur branche sur video() doit etre detruit
  // AVANT release(). Le commentaire d'origine supposait un stop() synchrone,
  // ce qu'il n'a jamais ete.
  if (studioPreview_) {
    studioPreview_->stop(force);
    studioPreview_->setVideoMix(nullptr);
  }
  refreshRemotePreviewIndicator();
  if (!studioMix_) return;

  if (force) {
    // Fermeture d'OBS : pas de boucle Qt pour porter un differe, et plus
    // personne derriere nous. Synchrone, comme la retraite des generations.
    studioMix_->release();
    return;
  }

  // On CEDE le mix mourant et on repart sur un neuf, au lieu de differer un
  // release() sur l'objet courant : sans ca, un aperçu redemarre pendant le
  // delai recreerait sa vue dans le meme objet, et le differe la detruirait
  // sous ses pieds.
  //
  // Le delai depasse volontairement celui de la retraite des generations : le
  // producteur doit survivre a son consommateur, jamais l'inverse.
  PreviewSceneMix* mourant = studioMix_;
  studioMix_ = new PreviewSceneMix();
  QTimer::singleShot(ProgramPreviewOutput::kRetireGraceMs + 1000, this,
                     [mourant]() { delete mourant; });
}

void SemirprodDock::onAudioLevelsStartRequested(const QString& requestId,
                                                int fps, int ttlMs) {
  // Bornes : au delà de 25 images par seconde l'œil ne distingue plus rien sur
  // une barre, en dessous de 5 elle saute. Le bail suit les mêmes garde-fous
  // que l'aperçu — un ttl fantaisiste ne doit ni épingler la mesure ni
  // l'éteindre aussitôt.
  fps = std::clamp(fps, 5, 25);
  ttlMs = std::clamp(ttlMs, 1000, 60000);

  if (!audioLevelsTimer_) {
    audioLevelsTimer_ = new QTimer(this);
    connect(audioLevelsTimer_, &QTimer::timeout, this,
            &SemirprodDock::pushAudioLevels);
  }
  if (!audioLevelsLeaseTimer_) {
    audioLevelsLeaseTimer_ = new QTimer(this);
    audioLevelsLeaseTimer_->setSingleShot(true);
    connect(audioLevelsLeaseTimer_, &QTimer::timeout, this, [this]() {
      blog(LOG_INFO, "[semirprod-dock] bail vu-metres expire - arret");
      stopAudioLevels();
    });
  }
  audioLevelsTimer_->start(1000 / fps);
  audioLevelsLeaseTimer_->start(ttlMs);

  liveClient_->sendControlResult(requestId,
                                 QStringLiteral("start_audio_levels"),
                                 QStringLiteral("success"), QString());
}

void SemirprodDock::onAudioLevelsStopRequested(const QString& requestId) {
  stopAudioLevels();
  liveClient_->sendControlResult(requestId, QStringLiteral("stop_audio_levels"),
                                 QStringLiteral("success"), QString());
}

void SemirprodDock::stopAudioLevels() {
  if (audioLevelsTimer_) audioLevelsTimer_->stop();
  if (audioLevelsLeaseTimer_) audioLevelsLeaseTimer_->stop();
  if (audioLevels_) audioLevels_->clear();
}

void SemirprodDock::pushAudioLevels() {
  if (!liveClient_->isConnected() || !audioLevels_) return;

  // La liste des pistes est celle du mixer : mêmes règles d'appartenance, donc
  // une piste qui disparaît du mélangeur perd sa barre au même instant.
  const QJsonArray inputs = sceneController_->collectAudioInputs();
  QStringList names;
  names.reserve(inputs.size());
  for (const QJsonValue& value : inputs) {
    const QString name = value.toObject().value(QStringLiteral("name")).toString();
    if (!name.isEmpty()) names.push_back(name);
  }
  audioLevels_->sync(names);

  liveClient_->sendAudioLevels(settings_->selectedStreamId(),
                               audioLevels_->snapshot());
}

void SemirprodDock::onSetStreamRequested(const QString& requestId,
                                        const QString& streamId) {
  const QString action = QStringLiteral("set_stream");

  // Seuls les flux servis par l'API pour CE compte sont acceptables. Le poste
  // ne prend donc pas un identifiant au mot : sans cette liste, un message mal
  // routé le ferait publier vers un flux qui n'est pas le sien.
  if (selectedStreamIndexById(streamId) < 0) {
    liveClient_->sendControlResult(requestId, action, QStringLiteral("failed"),
                                   QStringLiteral("stream_not_found"));
    return;
  }
  if (streamId == settings_->selectedStreamId()) {
    // Déjà dessus : succès, pas de reconnexion pour rien.
    liveClient_->sendControlResult(requestId, action, QStringLiteral("success"),
                                   QString());
    return;
  }
  if (studioAccessKnown_ && !studioAccess_) {
    // selectStream() ouvre une modale dans ce cas — une boîte de dialogue
    // native ouverte par une commande distante bloquerait OBS sur le poste,
    // sans personne devant l'écran pour la fermer. On répond, on n'ouvre rien.
    liveClient_->sendControlResult(requestId, action, QStringLiteral("failed"),
                                   QStringLiteral("subscription_required"));
    return;
  }

  // L'accusé part AVANT la bascule : selectStream() referme la socket pour
  // réenregistrer le poste sur le nouveau flux, et un accusé émis après serait
  // écrit dans une connexion déjà morte. Le changement lui-même passe par la
  // boucle d'événements pour que l'envoi soit réellement parti du tampon.
  liveClient_->sendControlResult(requestId, action, QStringLiteral("success"),
                                 QString());
  logEvent(QStringLiteral("info"),
           loc("Journal.SiteStream").arg(streamId));
  QPointer<SemirprodDock> self(this);
  QMetaObject::invokeMethod(
      this,
      [self, streamId]() {
        if (!self) return;
        self->selectStream(streamId);
      },
      Qt::QueuedConnection);
}

void SemirprodDock::onSetSceneMappingRequested(const QString& requestId,
                                               const QString& sceneLive,
                                               const QString& sceneBug,
                                               int autoSwitch) {
  // Une scène inconnue est REFUSÉE, jamais créée : le mapping ne sert qu'à
  // désigner des scènes que l'opérateur a déjà montées. Vide reste licite,
  // c'est « non configuré ».
  const QStringList scenes = sceneController_->availableScenes();
  const bool liveOk = sceneLive.isEmpty() || scenes.contains(sceneLive);
  const bool bugOk = sceneBug.isEmpty() || scenes.contains(sceneBug);
  if (!liveOk || !bugOk) {
    liveClient_->sendControlResult(requestId,
                                   QStringLiteral("set_scene_mapping"),
                                   QStringLiteral("failed"),
                                   QStringLiteral("scene_not_found"));
    // L'état repart quand même : la régie a proposé un nom périmé (collection
    // changée entre son affichage et son clic), elle doit voir le vrai.
    pushObsState();
    return;
  }

  // -1 = le dashboard n'a pas parlé de la bascule automatique : on garde le
  // réglage du poste plutôt que de le désarmer sans que personne l'ait demandé.
  const bool nextAutoSwitch =
      autoSwitch < 0 ? settings_->autoSwitchEnabled() : autoSwitch == 1;
  setSceneConfig(sceneLive, sceneBug, nextAutoSwitch);
  logEvent(QStringLiteral("info"),
           loc("Journal.RemoteScenes")
               .arg(sceneLive.isEmpty() ? loc("Journal.None") : sceneLive,
                    sceneBug.isEmpty() ? loc("Journal.None") : sceneBug,
                    nextAutoSwitch ? loc("Journal.Armed")
                                   : loc("Journal.Disarmed")));
  liveClient_->sendControlResult(requestId,
                                 QStringLiteral("set_scene_mapping"),
                                 QStringLiteral("success"), QString());
  pushObsState();
}

void SemirprodDock::onSetProfileRequested(const QString& requestId,
                                          const QString& name) {
  const bool outputActive = sceneController_->isStreamingActive() ||
                            sceneController_->isRecordingActive();
  const bool ok = sceneController_->switchToProfile(name);
  logEvent(ok ? QStringLiteral("info") : QStringLiteral("warn"),
           ok ? loc("Journal.RemoteProfile").arg(name)
              : loc("Journal.RemoteProfileRefused").arg(name));
  liveClient_->sendControlResult(
      requestId, QStringLiteral("set_profile"),
      ok ? QStringLiteral("success") : QStringLiteral("failed"),
      ok ? QString()
         : outputActive ? QStringLiteral("output_active")
                        : QStringLiteral("profile_not_found"));
  // La bascule elle-meme est asynchrone cote OBS (PROFILE_CHANGED rejouera un
  // push complet) ; celui-ci porte au moins le refus immediat.
  pushObsState();
}

void SemirprodDock::onSetSceneCollectionRequested(const QString& requestId,
                                                  const QString& name) {
  const bool outputActive = sceneController_->isStreamingActive() ||
                            sceneController_->isRecordingActive();
  const bool ok = sceneController_->switchToSceneCollection(name);
  logEvent(ok ? QStringLiteral("info") : QStringLiteral("warn"),
           ok ? loc("Journal.RemoteCollection").arg(name)
              : loc("Journal.RemoteCollectionRefused")
                    .arg(name));
  liveClient_->sendControlResult(
      requestId, QStringLiteral("set_scene_collection"),
      ok ? QStringLiteral("success") : QStringLiteral("failed"),
      ok ? QString()
         : outputActive ? QStringLiteral("output_active")
                        : QStringLiteral("collection_not_found"));
  pushObsState();
}

void SemirprodDock::onCreateSceneCollectionRequested(const QString& requestId,
                                                     const QString& name) {
  // Creer une collection BASCULE dessus : les canvas de la collection courante
  // (vertical, chaines du multistream) seraient demontes en plein direct.
  bool busy = verticalBridge_->state().streaming;
  const QJsonValue ms = multistreamBridge_->state();
  for (const QJsonValue& d : ms.toObject().value(QStringLiteral("destinations")).toArray()) {
    const QString st = d.toObject().value(QStringLiteral("state")).toString();
    if (st == QStringLiteral("live") || st == QStringLiteral("connecting") ||
        st == QStringLiteral("reconnecting"))
      busy = true;
  }
  QString reason;
  const bool ok = !busy && sceneController_->createSceneCollection(name, &reason);
  if (busy) reason = QStringLiteral("output_active");
  logEvent(ok ? QStringLiteral("info") : QStringLiteral("warn"),
           ok ? loc("Journal.RemoteCollectionCreated").arg(name)
              : loc("Journal.RemoteCollectionCreateRefused")
                    .arg(name, reason));
  liveClient_->sendControlResult(requestId, QStringLiteral("create_scene_collection"),
                                 ok ? QStringLiteral("success") : QStringLiteral("failed"),
                                 ok ? QString() : reason);
  pushObsState();
}

void SemirprodDock::onSetStudioModeRequested(const QString& requestId,
                                             bool enabled) {
  const bool ok = sceneController_->setStudioMode(enabled);
  // Couper le mode studio rend le deuxieme mix sans objet : la sortie
  // continuerait a composer une preparation qui n'existe plus.
  if (ok && !enabled) stopStudioPreview();
  logEvent(QStringLiteral("info"),
           enabled ? loc("Journal.RemoteStudioOn")
                   : loc("Journal.RemoteStudioOff"));
  liveClient_->sendControlResult(
      requestId, QStringLiteral("set_studio_mode"),
      ok ? QStringLiteral("success") : QStringLiteral("failed"),
      ok ? QString() : QStringLiteral("studio_mode_refused"));
  pushObsState();
}

void SemirprodDock::onSetPreviewSceneRequested(const QString& requestId,
                                               const QString& scene) {
  const bool ok = sceneController_->setPreviewScene(scene);
  liveClient_->sendControlResult(
      requestId, QStringLiteral("set_preview_scene"),
      ok ? QStringLiteral("success") : QStringLiteral("failed"),
      ok ? QString()
         : sceneController_->isStudioModeEnabled()
               ? QStringLiteral("scene_not_found")
               : QStringLiteral("studio_mode_off"));
  // PREVIEW_SCENE_CHANGED repointe le mix et repousse l'etat ; ce push-ci sert
  // au cas refuse, qui n'emet aucun evenement OBS.
  pushObsState();
}

void SemirprodDock::onTriggerTransitionRequested(const QString& requestId) {
  const bool ok = sceneController_->triggerStudioTransition();
  logEvent(ok ? QStringLiteral("info") : QStringLiteral("warn"),
           ok ? loc("Journal.RemoteTransition")
              : loc("Journal.RemoteTransitionRefused"));
  liveClient_->sendControlResult(
      requestId, QStringLiteral("trigger_transition"),
      ok ? QStringLiteral("success") : QStringLiteral("failed"),
      ok ? QString() : QStringLiteral("studio_mode_off"));
  pushObsState();
}

void SemirprodDock::stopProgramPreview(bool force) {
  if (programPreviewLeaseTimer_) programPreviewLeaseTimer_->stop();
  if (programPreviewWarmTimer_) programPreviewWarmTimer_->stop();
  if (programPreview_) programPreview_->stop(force);
  refreshRemotePreviewIndicator();
}

void SemirprodDock::warmStopProgramPreview(const char* reason) {
  // Fin de lease ou stop viewer : on garde l'output WHIP chaud pendant une
  // periode de grace. Une reouverture de l'apercu dans la fenetre est alors
  // quasi instantanee (le start() idempotent garde la session en vie) ; sans
  // reouverture, le vrai teardown libere encodeur et uplink. La deconnexion
  // WS, elle, appelle stopProgramPreview() directement : sans canal de
  // controle, personne ne peut reutiliser l'output.
  if (!programPreview_) return;
  constexpr int kWarmGraceMs = 3 * 60 * 1000;
  if (!programPreviewWarmTimer_) {
    programPreviewWarmTimer_ = new QTimer(this);
    programPreviewWarmTimer_->setSingleShot(true);
    connect(programPreviewWarmTimer_, &QTimer::timeout, this, [this]() {
      blog(LOG_INFO,
           "[semirprod-dock] program preview warm grace elapsed - stopping "
           "WHIP output");
      stopProgramPreview();
    });
  }
  blog(LOG_INFO,
       "[semirprod-dock] program preview %s - keeping WHIP output warm for "
       "%d s",
       reason, kWarmGraceMs / 1000);
  if (programPreviewLeaseTimer_) programPreviewLeaseTimer_->stop();
  programPreviewWarmTimer_->start(kWarmGraceMs);
}

void SemirprodDock::pushObsState() {
  if (!liveClient_->isConnected()) return;
  ObsStateReport report;
  report.streamId = settings_->selectedStreamId();
  report.activeScene = sceneController_->currentScene();
  report.scenes = sceneController_->availableScenes();
  report.sceneLive = settings_->sceneLive();
  report.sceneBug = settings_->sceneBug();
  report.streaming = sceneController_->isStreamingActive();
  report.recording = sceneController_->isRecordingActive();
  report.sourceReady =
      !report.sceneLive.isEmpty() &&
      sceneController_->isSemirprodSourceReadyInScene(report.sceneLive);
  report.width = lastSourceStateWidth_;
  report.height = lastSourceStateHeight_;
  // Mode studio + configuration du poste : relus a chaque envoi plutot que mis
  // en cache. Ce sont des getters libobs (listes courtes, aucune E/S) et
  // l'operateur peut changer de profil, de collection ou de mode studio DANS
  // OBS : l'etat de la regie doit suivre le poste, jamais l'inverse.
  report.studioMode = sceneController_->isStudioModeEnabled();
  report.previewScene = sceneController_->previewScene();
  report.profiles = sceneController_->availableProfiles();
  report.currentProfile = sceneController_->currentProfile();
  report.sceneCollections = sceneController_->availableSceneCollections();
  report.currentSceneCollection = sceneController_->currentSceneCollection();
  report.autoSwitch = settings_->autoSwitchEnabled();
  const VerticalBridge::State vertical = verticalBridge_->state();
  report.verticalAvailable = vertical.available;
  report.verticalStreaming = vertical.streaming;
  report.verticalConfigured = vertical.configured;
  report.verticalFollowStream = vertical.followStream;
  report.verticalServer = vertical.server;
  report.verticalError = vertical.lastError;
  report.multistream = multistreamBridge_->state();
  // Heal the timestamp on the slim chance OBS reports active without ever
  // emitting STREAMING_STARTED (e.g. plugin loaded mid-stream after an OBS
  // crash recovery). We don't know the real start, but stamping "now" gives
  // the dashboard a non-jumping baseline rather than a missing field.
  if (report.streaming && streamingStartedAt_.isEmpty()) {
    streamingStartedAt_ = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
  } else if (!report.streaming && !streamingStartedAt_.isEmpty()) {
    streamingStartedAt_.clear();
  }
  if (report.recording && recordingStartedAt_.isEmpty()) {
    recordingStartedAt_ = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
  } else if (!report.recording && !recordingStartedAt_.isEmpty()) {
    recordingStartedAt_.clear();
  }
  report.streamingStartedAt = streamingStartedAt_;
  report.recordingStartedAt = recordingStartedAt_;
  liveClient_->sendObsState(report);
}

void SemirprodDock::noticeRemoteDestination(const char* journalKey,
                                            const QStringList& hosts,
                                            bool hostChanged) {
  const QString shown =
      hosts.isEmpty() ? loc("Journal.UnknownHost") : hosts.join(QStringLiteral(", "));
  const QString message = loc(journalKey).arg(shown);
  // Hote change : avertissement. Meme hote : information, la cle a pu changer
  // (une autre chaine sur la meme plateforme), donc on le dit quand meme.
  logEvent(hostChanged ? QStringLiteral("warn") : QStringLiteral("info"), message);
  // Instance cloud : la regie EST l'operateur, pas un tiers a signaler. La
  // ligne reste dans le journal, aucun bandeau.
  if (edition::isCloud()) return;
  remoteDestinationNotice_ =
      QStringLiteral("%1 (%2)").arg(
          message, QTime::currentTime().toString(QStringLiteral("HH:mm")));
  broadcastToWeb();
}

void SemirprodDock::dismissRemoteDestinationNotice() {
  if (remoteDestinationNotice_.isEmpty()) return;
  remoteDestinationNotice_.clear();
  broadcastToWeb();
}

bool SemirprodDock::remotePreviewActive() const {
  if (programPreview_ && programPreview_->isRunning()) return true;
  if (studioPreview_ && studioPreview_->isRunning()) return true;
  if (canvasPreview_ && canvasPreview_->isRunning()) return true;
  if (verticalPreviewActive_) return true;
  if (previewTimer_ && previewTimer_->isActive()) return true;
  constexpr qint64 kCaptureWindowMs = 30000;
  return lastRemoteCaptureMs_ > 0 &&
         QDateTime::currentMSecsSinceEpoch() - lastRemoteCaptureMs_ < kCaptureWindowMs;
}

void SemirprodDock::markRemoteCapture() {
  lastRemoteCaptureMs_ = QDateTime::currentMSecsSinceEpoch();
  // Une capture isolee n'a pas de « fin » : l'indicateur retombe 30 s apres
  // la derniere. Minuteur relance a chaque capture, un seul pour toutes.
  if (!remoteCaptureTimer_) {
    remoteCaptureTimer_ = new QTimer(this);
    remoteCaptureTimer_->setSingleShot(true);
    connect(remoteCaptureTimer_, &QTimer::timeout, this,
            [this]() { refreshRemotePreviewIndicator(); });
  }
  remoteCaptureTimer_->start(30000 + 500);
  refreshRemotePreviewIndicator();
}

void SemirprodDock::refreshRemotePreviewIndicator() {
  // Fermeture d'OBS : plus d'interface a prevenir.
  if (shuttingDown_) return;
  // Instance cloud : la regie regarde en permanence, c'est son role. Un
  // indicateur toujours allume ne dirait rien, et son bouton « couper
  // l'apercu » couperait les apercus de la regie (reglage persiste).
  if (edition::isCloud()) return;
  const bool active = remotePreviewActive();
  if (active == remotePreviewIndicated_) return;
  remotePreviewIndicated_ = active;
  logEvent(active ? QStringLiteral("warn") : QStringLiteral("info"),
           active ? loc("Journal.RemotePreviewOn") : loc("Journal.RemotePreviewOff"));
  broadcastToWeb();
}

void SemirprodDock::logEvent(const QString& level, const QString& message) {
  // Double sortie : le dock Journal pour l'operateur, le log OBS pour le
  // support (le fichier de log part avec les rapports de bug).
  emit eventLogged(QDateTime::currentDateTime(), level, message);
  const int obsLevel = (level == QStringLiteral("error"))  ? LOG_ERROR
                       : (level == QStringLiteral("warn")) ? LOG_WARNING
                                                           : LOG_INFO;
  blog(obsLevel, "[semirprod] %s", message.toUtf8().constData());
}

void SemirprodDock::evaluateStreamHealth(int kbps, float congestion,
                                         double droppedPct) {
  // Le debit brut ne sert pas au diagnostic : c'est la congestion et les
  // images perdues qui disent si le lien tient, pas les kbps eux-memes.
  (void)kbps;

  // Seuils volontairement SOUTENUS : une mesure isolee au-dessus du seuil est
  // du bruit (un pic de congestion de 2 s se resorbe seul). On n'alerte
  // l'operateur que si la degradation persiste, et on ne l'annule qu'apres
  // une accalmie plus longue - sinon le bandeau clignote.
  constexpr double kDroppedPctThreshold = 5.0;
  constexpr float kCongestionThreshold = 0.5f;
  constexpr qint64 kSustainedMs = 10000;
  constexpr qint64 kRecoveryMs = 20000;

  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  QString reason;
  if (droppedPct >= kDroppedPctThreshold) {
    reason = loc("Health.Dropped").arg(droppedPct, 0, 'f', 1);
  } else if (congestion >= kCongestionThreshold) {
    reason = loc("Health.Congestion")
                 .arg(static_cast<int>(congestion * 100));
  }

  if (!reason.isEmpty()) {
    healthGoodSinceMs_ = 0;
    if (healthBadSinceMs_ == 0) healthBadSinceMs_ = now;
    if (!healthAlertActive_ && now - healthBadSinceMs_ >= kSustainedMs) {
      healthAlertActive_ = true;
      healthAlertReason_ = reason;
      ++sessionIncidents_;
      logEvent(QStringLiteral("warn"),
               loc("Journal.HealthDegraded").arg(reason));
    } else if (healthAlertActive_) {
      healthAlertReason_ = reason;
    }
  } else {
    healthBadSinceMs_ = 0;
    if (healthAlertActive_) {
      if (healthGoodSinceMs_ == 0) healthGoodSinceMs_ = now;
      if (now - healthGoodSinceMs_ >= kRecoveryMs) {
        healthAlertActive_ = false;
        healthAlertReason_.clear();
        healthGoodSinceMs_ = 0;
        logEvent(QStringLiteral("success"),
                 loc("Journal.HealthRecovered"));
      }
    }
  }

  // Pas de debit adaptatif ici : c'est le travail d'OBS.
  //
  // Cette fonction ne fait plus que CONSTATER la degradation (alerte sante,
  // journal, remontee a la regie). Regler le debit du direct appartient a OBS,
  // qui a son propre reglage « Changer dynamiquement le debit pour gerer la
  // congestion » (Parametres > Sortie > Avance). Le notre agissait sur le MEME
  // encodeur, via obs_frontend_get_streaming_output : deux regulateurs sur le
  // meme bouton, dont les remontees visaient un debit « d'origine » capture une
  // seule fois et que l'autre avait deja pu changer.
}

QString SemirprodDock::sessionHistoryFilePath() {
  // A cote du fichier de reglages du plugin : meme cycle de vie, meme
  // repertoire par profil OBS.
  const QFileInfo info(Settings::configFilePath());
  return info.absoluteDir().filePath(QStringLiteral("semirprod-sessions.json"));
}

void SemirprodDock::loadSessionHistory() {
  QFile file(sessionHistoryFilePath());
  if (!file.exists() || !file.open(QIODevice::ReadOnly)) return;
  const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
  if (doc.isArray()) sessionHistory_ = doc.array();
}

void SemirprodDock::saveSessionHistory() {
  QFile file(sessionHistoryFilePath());
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
  file.write(QJsonDocument(sessionHistory_).toJson(QJsonDocument::Indented));
}

void SemirprodDock::beginSessionRecord() {
  sessionStartedMs_ = QDateTime::currentMSecsSinceEpoch();
  sessionKbpsSum_ = 0;
  sessionKbpsSamples_ = 0;
  sessionKbpsPeak_ = 0;
  sessionDroppedFrames_ = 0;
  sessionTotalFrames_ = 0;
  sessionIncidents_ = 0;
}

void SemirprodDock::finalizeSessionRecord() {
  if (sessionStartedMs_ == 0) return;
  const qint64 endMs = QDateTime::currentMSecsSinceEpoch();
  const qint64 durationSec = (endMs - sessionStartedMs_) / 1000;
  sessionStartedMs_ = 0;
  // Sous 30 s, c'est un test de configuration, pas une diffusion : l'y
  // consigner noierait l'historique utile.
  if (durationSec < 30) return;

  QJsonObject entry;
  entry[QStringLiteral("endedAt")] =
      QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
  entry[QStringLiteral("durationSec")] = static_cast<double>(durationSec);
  entry[QStringLiteral("avgKbps")] =
      sessionKbpsSamples_ > 0
          ? static_cast<int>(sessionKbpsSum_ / sessionKbpsSamples_)
          : 0;
  entry[QStringLiteral("peakKbps")] = sessionKbpsPeak_;
  entry[QStringLiteral("droppedFrames")] = sessionDroppedFrames_;
  entry[QStringLiteral("totalFrames")] = sessionTotalFrames_;
  entry[QStringLiteral("droppedPct")] =
      sessionTotalFrames_ > 0
          ? (static_cast<double>(sessionDroppedFrames_) / sessionTotalFrames_) *
                100.0
          : 0.0;
  entry[QStringLiteral("incidents")] = sessionIncidents_;

  sessionHistory_.prepend(entry);
  while (sessionHistory_.size() > 20) sessionHistory_.removeLast();
  saveSessionHistory();
  logEvent(QStringLiteral("info"),
           loc("Journal.StreamSummary")
               .arg(durationSec / 60)
               .arg(entry[QStringLiteral("avgKbps")].toInt())
               .arg(sessionIncidents_));
}

void SemirprodDock::pushOutputStats() {
  const OutputStats s = sceneController_->collectOutputStats();

  // La WebUI locale recoit les stats meme si le WS backend est coupe.
  if (webStatsActive_ != s.active) {
    webStatsActive_ = s.active;
    if (s.active) {
      beginSessionRecord();
    } else {
      finalizeSessionRecord();
      webStatsKbps_ = 0;
      webStatsDroppedFrames_ = 0;
      webStatsTotalFrames_ = 0;
      webStatsCongestion_ = 0.0f;
      healthAlertActive_ = false;
      healthAlertReason_.clear();
      healthBadSinceMs_ = 0;
      healthGoodSinceMs_ = 0;
      broadcastToWeb();
    }
  }

  if (!liveClient_->isConnected() && !s.active) return;

  if (!s.active) {
    // Rien en diffusion : on invalide la baseline pour repartir propre au
    // prochain départ. Le dashboard sait déjà via obs_state.streaming qu'il
    // n'y a pas de sortie, donc on n'émet rien ici.
    outputStatsBaselineValid_ = false;
    return;
  }

  const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
  double kbps = 0.0;
  int droppedDelta = 0;
  if (outputStatsBaselineValid_ && nowMs > lastOutputStatsMs_) {
    // totalBytes est cumulatif et monotone tant que la sortie tourne ; le
    // garde >= protège d'un reset improbable (sortie relancée entre 2 ticks).
    const quint64 deltaBytes =
        s.totalBytes >= lastOutputBytes_ ? s.totalBytes - lastOutputBytes_ : 0;
    const qint64 deltaMs = nowMs - lastOutputStatsMs_;
    // bits / ms == kbits / s, donc kbps sort directement de cette division.
    kbps = static_cast<double>(deltaBytes * 8) / static_cast<double>(deltaMs);
    droppedDelta = std::max(0, s.droppedFrames - lastOutputDroppedFrames_);
  }
  lastOutputBytes_ = s.totalBytes;
  lastOutputDroppedFrames_ = s.droppedFrames;
  lastOutputStatsMs_ = nowMs;
  outputStatsBaselineValid_ = true;

  webStatsKbps_ = static_cast<int>(qRound(kbps));
  webStatsDroppedFrames_ = s.droppedFrames;
  webStatsTotalFrames_ = s.totalFrames;
  webStatsCongestion_ = s.congestion;

  // Accumulateurs de session. Le debit n'est echantillonne qu'une fois la
  // baseline etablie (le premier tick vaut 0 par construction).
  if (webStatsKbps_ > 0) {
    sessionKbpsSum_ += webStatsKbps_;
    ++sessionKbpsSamples_;
    sessionKbpsPeak_ = std::max(sessionKbpsPeak_, webStatsKbps_);
  }
  sessionDroppedFrames_ = s.droppedFrames;
  sessionTotalFrames_ = s.totalFrames;

  const double droppedPct =
      s.totalFrames > 0
          ? (static_cast<double>(s.droppedFrames) / s.totalFrames) * 100.0
          : 0.0;
  evaluateStreamHealth(webStatsKbps_, s.congestion, droppedPct);
  broadcastToWeb();

  if (!liveClient_->isConnected()) return;
  liveClient_->sendOutputStats(
      settings_->selectedStreamId(),
      s.congestion,
      static_cast<int>(qRound(kbps)),
      s.droppedFrames,
      s.totalFrames,
      droppedDelta,
      static_cast<int>(s.renderSkipped),
      static_cast<int>(s.renderTotal));
}

void SemirprodDock::pushSourcesState() {
  if (!liveClient_->isConnected()) return;
  liveClient_->sendSourcesState(
      settings_->selectedStreamId(),
      sceneController_->collectSceneSources());
}

void SemirprodDock::pushAudioState() {
  if (!liveClient_->isConnected()) return;
  liveClient_->sendAudioState(
      settings_->selectedStreamId(),
      sceneController_->collectAudioInputs());
}

namespace {

// Délai de regroupement des envois d'audio_state. Assez court pour que la
// liste des pistes suive une bascule de scène à l'oeil, assez long pour
// absorber la rafale de signaux d'une transition (une source qui s'active,
// une qui s'éteint, pour chaque source des deux scènes).
constexpr int kAudioSyncDelayMs = 150;

// Signaux globaux de libobs qui changent l'APPARTENANCE au mélangeur : une
// source n'y figure que tant qu'elle est active dans le programme.
constexpr const char* kCoreActivitySignals[] = {
    "source_activate",
    "source_deactivate",
    "source_audio_activate",
    "source_audio_deactivate",
};

} // namespace

void SemirprodDock::connectCoreSignals() {
  if (coreSignalsConnected_) return;
  signal_handler_t* handler = obs_get_signal_handler();
  if (!handler) return;
  for (const char* name : kCoreActivitySignals) {
    signal_handler_connect(handler, name, &SemirprodDock::onCoreSourceActivity,
                           this);
  }
  coreSignalsConnected_ = true;
}

void SemirprodDock::disconnectCoreSignals() {
  if (!coreSignalsConnected_) return;
  signal_handler_t* handler = obs_get_signal_handler();
  if (handler) {
    for (const char* name : kCoreActivitySignals) {
      signal_handler_disconnect(handler, name,
                                &SemirprodDock::onCoreSourceActivity, this);
    }
  }
  coreSignalsConnected_ = false;
}

void SemirprodDock::onCoreSourceActivity(void* data, calldata_t* /*cd*/) {
  auto* self = static_cast<SemirprodDock*>(data);
  if (!self) return;
  // Ce callback tourne sur le thread qui a émis le signal — celui du rendu ou
  // du coeur d'OBS, jamais Qt. Rien d'autre ne doit se faire ici : on repasse
  // par la boucle d'événements principale, seule autorisée à toucher aux
  // objets Qt et à la socket.
  QMetaObject::invokeMethod(
      self, [self]() { self->scheduleAudioSync(); }, Qt::QueuedConnection);
}

void SemirprodDock::scheduleAudioSync() {
  if (shuttingDown_) return;
  if (!audioSyncTimer_) {
    audioSyncTimer_ = new QTimer(this);
    audioSyncTimer_->setSingleShot(true);
    connect(audioSyncTimer_, &QTimer::timeout, this,
            &SemirprodDock::pushAudioState);
  }
  // start() sur un minuteur déjà armé le RELANCE : la rafale d'une transition
  // se résout donc en un seul envoi, une fois le calme revenu.
  audioSyncTimer_->start(kAudioSyncDelayMs);
}

void SemirprodDock::onSetSceneItemVisible(const QString& requestId,
                                          const QString& sceneName,
                                          const QString& sourceName,
                                          bool visible) {
  const bool ok = sceneController_->setSceneItemVisible(sceneName, sourceName, visible);
  liveClient_->sendSourceControlResult(
      requestId,
      QStringLiteral("set_source_visible"),
      ok ? QStringLiteral("success") : QStringLiteral("failed"),
      ok ? QString() : QStringLiteral("scene_item_not_found"));
  // Echo the new snapshot immediately so the dashboard sees the toggle commit
  // without waiting for the 5 s heartbeat.
  pushSourcesState();
}

void SemirprodDock::onSetSceneItemLocked(const QString& requestId,
                                         const QString& sceneName,
                                         const QString& sourceName,
                                         bool locked) {
  const bool ok = sceneController_->setSceneItemLocked(sceneName, sourceName, locked);
  liveClient_->sendSourceControlResult(
      requestId,
      QStringLiteral("set_source_locked"),
      ok ? QStringLiteral("success") : QStringLiteral("failed"),
      ok ? QString() : QStringLiteral("scene_item_not_found"));
  pushSourcesState();
}

void SemirprodDock::onSetSourceMuted(const QString& requestId,
                                     const QString& sourceName,
                                     bool muted) {
  const bool ok = sceneController_->setSourceMuted(sourceName, muted);
  liveClient_->sendSourceControlResult(
      requestId,
      QStringLiteral("set_audio_mute"),
      ok ? QStringLiteral("success") : QStringLiteral("failed"),
      ok ? QString() : QStringLiteral("source_not_found"));
  pushAudioState();
}

void SemirprodDock::onSetSourceVolume(const QString& requestId,
                                      const QString& sourceName,
                                      float volumeDb) {
  const bool ok = sceneController_->setSourceVolumeDb(sourceName, volumeDb);
  liveClient_->sendSourceControlResult(
      requestId,
      QStringLiteral("set_audio_volume"),
      ok ? QStringLiteral("success") : QStringLiteral("failed"),
      ok ? QString() : QStringLiteral("source_not_found"));
  pushAudioState();
}

void SemirprodDock::onSetSourceMonitoring(const QString& requestId,
                                          const QString& sourceName,
                                          int monitoringType) {
  const bool ok = sceneController_->setSourceMonitoringType(sourceName, monitoringType);
  liveClient_->sendSourceControlResult(
      requestId,
      QStringLiteral("set_audio_monitoring"),
      ok ? QStringLiteral("success") : QStringLiteral("failed"),
      ok ? QString() : QStringLiteral("source_or_value_invalid"));
  pushAudioState();
}

void SemirprodDock::startAuth() {
  // En pause (version bloquee ou mise a jour obligatoire) : pas d'appairage,
  // la liaison ne s'ouvrirait de toute facon pas.
  if (pluginBlocked_) {
    blockedPromptShown_ = false;
    showBlockedPrompt();
    return;
  }
  // Un nouvel appairage commence : une relecture du trousseau encore en
  // attente ne doit pas reconnecter l'ancienne liaison pendant ce temps.
  tokenStore_->cancelPendingLoad();
  // Immediate feedback so the user can tell the click registered. The
  // /device/code POST normally returns in <500 ms but on the first hit
  // after a deploy or on a slow link the user might otherwise think the
  // button is dead. Replaced by either the pairing code (success) or a
  // red error message (deviceFlowFailed) within a couple seconds.
  accountLabel_ = loc("Account.PairingStarting");
  accountUrl_.clear();
  pairingFailed_ = false;
  setStatus(QStringLiteral("Demande de code de pairing..."));
  broadcastToWeb();
  authFlow_->start();
}

void SemirprodDock::disconnectAccount() {
  // Le poste se retire lui-même côté serveur AVANT l'effacement local : c'est
  // ce qui permet de reconnecter un AUTRE compte sur ce PC sans que le poste
  // reste attaché à l'ancien espace. Au mieux, 3 s au plus.
  const TokenBundle binding = tokenStore_->cached();
  // Un renouvellement en vol enregistrerait sa liaison APRES l'effacement :
  // le poste se reconnecterait seul. On l'annule d'abord.
  authFlow_->cancel();
  tokenStore_->cancelPendingLoad();
  // Plus aucune reprise ni renouvellement programme : l'un comme l'autre
  // relancerait authFlow_->refresh() sur la liaison qu'on efface.
  if (sessionRetryTimer_) sessionRetryTimer_->stop();
  if (tokenRefreshTimer_) tokenRefreshTimer_->stop();
  liveClient_->disconnectClient();
  QPointer<SemirprodDock> self(this);
  authFlow_->release(binding, [self]() {
    if (!self) return;
    // Pendant les 3 s du retrait, un renouvellement a pu repartir : annule
    // encore, juste avant l'effacement local.
    self->authFlow_->cancel();
    self->clearLocalBinding();
  });
}

void SemirprodDock::clearLocalBinding() {
  liveClient_->setDeviceId(QString());
  // Liste des sauvegardes et leur provenance (« creee par toi ») : oubliees
  // TOUT DE SUITE, sans attendre le trousseau. Le compte suivant ne doit
  // jamais importer depuis la liste de celui-ci.
  backupService_->setBearerToken(QString());
  tokenStore_->clear([this](bool) {
    liveClient_->disconnectClient();
    streams_.clear();
    // Forget the entitlement so a fresh login re-queries from scratch.
    studioAccessKnown_ = false;
    // Les fonctions ouvertes appartiennent au compte deconnecte.
    pluginFeaturesJson_.clear();
    serverOrgName_.clear();
    // L'identite appartient au compte deconnecte. Sans ca, l'instantane du
    // compte suivant trouverait un identifiant deja la et ne relirait pas
    // /api/me : l'avatar de l'ancien compte resterait affiche.
    accountName_.clear();
    accountEmail_.clear();
    accountId_.clear();
    accountAvatarSourceUrl_.clear();
    accountAvatarBytes_.clear();
    accountAvatarType_.clear();
    accountAvatarTag_.clear();
    if (initialFetchTimer_) initialFetchTimer_->stop();
    studioAccess_ = true;
    studioReason_.clear();
    studioCheckInFlight_ = false;
    setStatus(QStringLiteral("Plugin déconnecté."));
    updateUiState();
  });
}

void SemirprodDock::refreshAll() {
  refreshSceneChoices();
  checkStudioAccess();
}

void SemirprodDock::selectStream(const QString& streamId) {
  const int index = selectedStreamIndexById(streamId);
  if (index < 0) return;

  const QString newId = streams_[index].id;
  // Le corps de cette fonction vivait dans le slot currentIndexChanged du
  // combo : selectStream() ne faisait que deplacer l'index, et changer d'index
  // declenchait tout ce qui suit. Sans combo, on l'execute directement — y
  // compris cette sortie anticipee, qui rend l'equivalent du « pas de signal
  // quand l'index ne bouge pas » (sinon re-selectionner le flux courant
  // relancerait une modale et un enregistrement pour rien).
  if (newId == settings_->selectedStreamId()) return;

  settings_->setSelectedStreamId(newId);
  applySelectedStream();
  // Poste v2 : le flux change SUR la liaison ouverte (`set_target`). Fermer
  // puis rouvrir le socket, comme avant, sortait le poste de la liste et
  // coupait la régie à chaque changement de flux.
  liveClient_->registerStream(newId, settings_->sceneLive(), settings_->sceneBug());
  broadcastToWeb();
}

void SemirprodDock::setInstanceLabel(const QString& label) {
  const QString trimmed = label.trimmed().left(64);
  if (trimmed.isEmpty() || trimmed == settings_->instanceLabel()) return;
  settings_->setInstanceLabel(trimmed);
  // Le dashboard identifie les instances par ce libelle dans son selecteur :
  // un heartbeat immediat evite d'attendre jusqu'a 5 s l'affichage du
  // nouveau nom.
  if (liveClient_->isConnected()) liveClient_->sendInstanceHeartbeat();
  logEvent(QStringLiteral("info"),
           loc("Journal.MachineName").arg(trimmed));
  broadcastToWeb();
}

void SemirprodDock::openUpdateDownload() {
  if (pendingUpdateUrl_.isEmpty()) {
    // Manifeste pas encore arrive (version bloquee au demarrage, panne) : on
    // le redemande et l'ouverture suit son arrivee.
    openUpdateWhenKnown_ = true;
    updateChecker_->check();
    return;
  }
  logEvent(QStringLiteral("info"),
           loc("Journal.UpdateDownload")
               .arg(pendingUpdateVersion_));
  QDesktopServices::openUrl(QUrl(pendingUpdateUrl_));
}

void SemirprodDock::maybePromptUpdate() {
  // Instance infogeree : personne devant l'ecran.
  if (!edition::showsBlockingPrompts()) return;
  if (shuttingDown_ || !obsLoaded_ || pluginBlocked_ || updatePrompt_) return;
  const QString version = pendingUpdateVersion_;
  if (version.isEmpty() || updatePromptedVersion_ == version) return;
  // Jamais par dessus un direct ou un enregistrement : reprise a l'arret.
  if (obs_frontend_streaming_active() || obs_frontend_recording_active()) return;

  // Critique : le plugin s'arrete jusqu'a la mise a jour. Meme chemin qu'une
  // version bloquee par le serveur, avec son propre texte.
  if (pendingUpdateSeverity_ == QStringLiteral("critical")) {
    updatePromptedVersion_ = version;
    enterPluginBlocked(BlockReason::UpdateRequired);
    return;
  }

  // Facultative : UNE alerte. « Me rappeler plus tard » la reporte de 24 h,
  // relances d'OBS comprises ; « Ne plus me rappeler » enterre cette version.
  if (settings_->skippedUpdateVersion() == version) return;
  const qint64 now = QDateTime::currentSecsSinceEpoch();
  if (settings_->isUpdateSnoozed(version, now)) return;

  updatePromptedVersion_ = version;
  auto* box = new QMessageBox(static_cast<QWidget*>(obs_frontend_get_main_window()));
  box->setAttribute(Qt::WA_DeleteOnClose);
  // Non modale : OBS reste utilisable, la fenetre attend qu'on y reponde.
  box->setWindowModality(Qt::NonModal);
  box->setIcon(QMessageBox::Information);
  box->setWindowTitle(QStringLiteral("Semirprod"));
  box->setTextFormat(Qt::PlainText);
  box->setText(loc("Box.UpdateAvailable").arg(version));
  if (!pendingUpdateNotes_.isEmpty()) box->setInformativeText(pendingUpdateNotes_);
  QPushButton* install = box->addButton(loc("Box.Update"), QMessageBox::AcceptRole);
  QPushButton* later =
      box->addButton(loc("Box.RemindLater"), QMessageBox::RejectRole);
  QPushButton* never =
      box->addButton(loc("Box.NeverRemind"), QMessageBox::DestructiveRole);
  box->setDefaultButton(install);
  box->setEscapeButton(later);
  connect(box, &QMessageBox::buttonClicked, this, [this, install, never, version](QAbstractButton* b) {
    if (b == install) {
      openUpdateDownload();
    } else if (b == never) {
      settings_->setSkippedUpdateVersion(version);
      logEvent(QStringLiteral("info"),
               loc("Journal.UpdateSkipped").arg(version));
    } else {
      // « Me rappeler plus tard », croix ou Echap.
      settings_->snoozeUpdate(version, QDateTime::currentSecsSinceEpoch() + 24 * 60 * 60);
    }
  });
  updatePrompt_ = box;
  box->show();
}

void SemirprodDock::enterPluginBlocked(BlockReason reason) {
  if (pluginBlocked_) return;
  pluginBlocked_ = true;
  blockReason_ = reason;
  const bool update = reason == BlockReason::UpdateRequired;
  blog(LOG_WARNING, update ? "[semirprod-dock] mise a jour obligatoire : plugin en pause"
                           : "[semirprod-dock] version bloquee par le serveur : pilotage coupe");
  logEvent(QStringLiteral("error"),
           update ? loc("Journal.UpdateRequired")
                  : loc("Journal.VersionBlocked"));
  // Arret ferme : aucun renouvellement de jeton ne rouvre la liaison. La
  // nouvelle version, elle, repart d'un processus neuf.
  liveClient_->halt();
  if (updatePrompt_) updatePrompt_->close();
  // Le manifeste n'est peut-etre pas encore arrive (demarrage, panne) : on le
  // redemande, l'ouverture suivra s'il est demande avant.
  if (pendingUpdateUrl_.isEmpty()) updateChecker_->check();
  broadcastToWeb();
  if (obsLoaded_) showBlockedPrompt();
}

void SemirprodDock::showBlockedPrompt() {
  // Instance infogeree : personne devant l'ecran.
  if (!edition::showsBlockingPrompts()) return;
  if (shuttingDown_ || blockedPromptShown_) return;
  blockedPromptShown_ = true;
  auto* box = new QMessageBox(static_cast<QWidget*>(obs_frontend_get_main_window()));
  box->setAttribute(Qt::WA_DeleteOnClose);
  box->setWindowModality(Qt::NonModal);
  box->setIcon(QMessageBox::Critical);
  box->setWindowTitle(QStringLiteral("Semirprod"));
  box->setTextFormat(Qt::PlainText);
  if (blockReason_ == BlockReason::UpdateRequired) {
    box->setText(loc("Box.UpdateRequired")
                     .arg(pendingUpdateVersion_));
    if (!pendingUpdateNotes_.isEmpty()) box->setInformativeText(pendingUpdateNotes_);
  } else {
    box->setText(loc("Box.Blocked"));
    box->setInformativeText(loc("Box.BlockedInfo"));
  }
  QPushButton* install = box->addButton(loc("Box.Update"), QMessageBox::AcceptRole);
  // Seul choix a cote de la mise a jour : la popup revient au prochain lancement.
  box->addButton(loc("Box.Later"), QMessageBox::RejectRole);
  box->setDefaultButton(install);
  connect(box, &QMessageBox::buttonClicked, this, [this, install](QAbstractButton* b) {
    if (b == install) openUpdateDownload();
  });
  box->show();
}

namespace {
/**
 * Chemin de page accepte depuis la matrice serveur : `/org/<slug>/<page>`, et
 * rien d'autre. Relatif et borne a dessein — une reponse ne choisit jamais
 * l'hote, ni un chemin qui sortirait du dashboard.
 */
bool isSafeFeaturePath(const QString& path) {
  if (!path.startsWith(QStringLiteral("/org/")) || path.size() > 200) return false;
  if (path.contains(QStringLiteral("..")) || path.contains(QStringLiteral("//"))) return false;
  for (const QChar& c : path) {
    const bool ok = (c >= QLatin1Char('a') && c <= QLatin1Char('z')) ||
                    (c >= QLatin1Char('A') && c <= QLatin1Char('Z')) ||
                    (c >= QLatin1Char('0') && c <= QLatin1Char('9')) ||
                    c == QLatin1Char('/') || c == QLatin1Char('-') || c == QLatin1Char('_') ||
                    c == QLatin1Char('%');
    if (!ok) return false;
  }
  return true;
}
}  // namespace

void SemirprodDock::openFeatureAction(const QString& feature) {
  const QJsonObject matrix = QJsonDocument::fromJson(pluginFeaturesJson_).object();

  // Impaye en cours : l'avertissement ne vit pas dans la liste des fonctions
  // (il n'en ferme aucune), mais il porte le meme geste.
  if (feature == QStringLiteral("payment-grace")) {
    const QString path =
        matrix.value(QStringLiteral("paymentGrace")).toObject().value(QStringLiteral("actionPath")).toString();
    if (!isSafeFeaturePath(path)) return;
    logEvent(QStringLiteral("info"), loc("Journal.OpenPaymentFix"));
    QDesktopServices::openUrl(QUrl(Config::apiBaseUrl() + path));
    return;
  }

  const QJsonArray features = matrix.value(QStringLiteral("features")).toArray();
  for (const QJsonValue& item : features) {
    const QJsonObject verdict = item.toObject();
    if (verdict.value(QStringLiteral("feature")).toString() != feature) continue;

    // Mise a jour : le lien vient du manifeste SIGNE, jamais de la matrice.
    if (verdict.value(QStringLiteral("action")).toString() == QStringLiteral("update-plugin")) {
      openUpdateDownload();
      return;
    }

    const QString path = verdict.value(QStringLiteral("actionPath")).toString();
    if (!isSafeFeaturePath(path)) {
      blog(LOG_WARNING, "[semirprod-dock] chemin de geste refuse pour %s",
           feature.toUtf8().constData());
      return;
    }
    logEvent(QStringLiteral("info"),
             loc("Journal.OpenAction")
                 .arg(verdict.value(QStringLiteral("actionLabel")).toString()));
    QDesktopServices::openUrl(QUrl(Config::apiBaseUrl() + path));
    return;
  }
}

void SemirprodDock::openSubscribePage() {
  const QString url = Config::siteBaseUrl();
  logEvent(QStringLiteral("info"),
           loc("Journal.OpenSubscribe"));
  QDesktopServices::openUrl(QUrl(url));
}

// ============================================================================
// Sauvegardes de collections de scenes
//
// Le dock ne fait que RELIER : il donne au service le jeton du compte et
// l'identite du poste, puis le laisse travailler. Les deux sont relus a chaque
// appel plutot que poses une fois — le jeton tourne au fil des sessions, et le
// nom du poste change des que le client le renomme depuis le dashboard.
// ============================================================================

void SemirprodDock::backupCurrentCollection() {
  backupCollections({});
}

void SemirprodDock::backupCollections(const QStringList& collectionNames) {
  backupService_->setBearerToken(accessToken());
  // Poste v2 : les sauvegardes sont rattachées à l'appareil, identité tirée
  // par le serveur, comme tout le reste.
  backupService_->setInstance(tokenStore_->cached().deviceId, settings_->displayInstanceLabel());
  backupService_->backupCollections(collectionNames);
}

void SemirprodDock::refreshBackups() {
  backupService_->setBearerToken(accessToken());
  backupService_->refresh();
}

void SemirprodDock::importBackup(const QString& backupId) {
  backupService_->setBearerToken(accessToken());
  backupService_->importBackup(backupId);
}

void SemirprodDock::openImportedCollection() {
  // L'import n'ouvre plus la collection de lui-meme : c'est ce geste-ci, fait
  // par l'operateur, qui la charge. Le nom est celui que l'import a reellement
  // installe, et switchToSceneCollection refuse pendant une sortie active.
  const QString name = backupService_->importedCollectionName();
  if (name.isEmpty()) return;
  sceneController_->switchToSceneCollection(name);
  broadcastToWeb();
}

void SemirprodDock::runConnectionTest() {
  if (connTestRunning_) return;

  // Le test envoie deux requetes reelles. Le garde `connTestRunning_` empeche
  // deja les tirs concurrents, mais pas un enchainement au rythme des
  // round-trips : on impose une fenetre minimale. Une demande trop rapprochee
  // est ignoree (le resultat precedent est encore affiche, donc rien a
  // reporter a l'operateur).
  static constexpr qint64 kConnTestMinIntervalMs = 10'000;
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  if (connTestLastMs_ > 0 && now - connTestLastMs_ < kConnTestMinIntervalMs) return;
  connTestLastMs_ = now;

  connTestRunning_ = true;
  connTestApiMs_ = -1;
  connTestLiveMs_ = -1;
  connTestError_.clear();
  connTestPending_ = 2;
  broadcastToWeb();

  // Deux sondes : l'API applicative (routage backend + base) et l'hote du
  // WebSocket temps reel (chemin des commandes de regie). On mesure le
  // round-trip complet, pas seulement le TCP — c'est ce que ressent
  // l'operateur. Un code HTTP non-2xx compte comme joignable : l'endpoint WS
  // repond 400/426 a un GET simple, ce qui prouve quand meme le chemin.
  auto probe = [this](const QString& url, int* slot, const QString& what) {
    const qint64 startedMs = QDateTime::currentMSecsSinceEpoch();
    http_->getJson(url, QString(),
                   [this, startedMs, slot, what](const HttpResponse& res,
                                                 const QJsonDocument&) {
                     const int ms = static_cast<int>(
                         QDateTime::currentMSecsSinceEpoch() - startedMs);
                     if (res.statusCode > 0) {
                       *slot = ms;
                     } else {
                       *slot = -1;
                       if (connTestError_.isEmpty()) {
                         connTestError_ = loc("ConnTest.Unreachable").arg(what);
                       }
                     }
                     if (--connTestPending_ <= 0) connTestRunning_ = false;
                     broadcastToWeb();
                   });
  };

  probe(Config::apiBaseUrl() + QStringLiteral("/api/plugin/latest"), &connTestApiMs_,
        loc("ConnTest.Backend"));
  // wss:// -> https:// : meme hote, meme chemin reseau, mesurable en HTTP.
  QString liveProbe = Config::liveWsUrl();
  liveProbe.replace(QStringLiteral("wss://"), QStringLiteral("https://"));
  liveProbe.replace(QStringLiteral("ws://"), QStringLiteral("http://"));
  probe(liveProbe, &connTestLiveMs_, loc("ConnTest.Realtime"));
}

void SemirprodDock::setPreviewEnabled(bool enabled) {
  if (settings_->previewEnabled() == enabled) return;
  settings_->setPreviewEnabled(enabled);
  if (!enabled) {
    // Coupe tout ce qui encode : screenshots + output WHIP (arret dur, pas
    // de keep-warm — le but est justement de liberer la machine).
    stopPreviewStream();
    stopProgramPreview();
    // Les trois autres apercus aussi : c'est le geste « couper l'apercu » de
    // l'indicateur d'apercu distant, il doit couper TOUT ce qui part.
    stopStudioPreview();
    stopVerticalPreview();
    stopCanvasPreview();
    lastRemoteCaptureMs_ = 0;
    refreshRemotePreviewIndicator();
  }
  logEvent(QStringLiteral("info"), enabled
                                       ? loc("Journal.PreviewOn")
                                       : loc("Journal.PreviewOff"));
  broadcastToWeb();
}

void SemirprodDock::setSceneConfig(const QString& live, const QString& bug, bool autoSwitch) {
  // La page peut avoir été rendue avant un changement de collection : elle
  // renvoie alors des noms de l'ancienne collection, et SceneCard réémet
  // TOUJOURS les deux slots même quand l'opérateur n'en a touché qu'un. On
  // refuse un nom qui ne désigne aucune scène réelle au lieu de le persister :
  // le slot garde sa valeur et le broadcast en fin de fonction resynchronise la
  // page. Une chaîne vide reste acceptée — c'est "non configuré".
  const QStringList scenes = sceneController_->availableScenes();
  const auto accept = [&](const QString& name, const QString& slot) {
    if (name.isEmpty() || scenes.contains(name)) return true;
    logEvent(QStringLiteral("warn"),
             loc("Journal.SceneIgnored")
                 .arg(slot, name, Settings::activeSceneCollection()));
    return false;
  };
  if (accept(live, QStringLiteral("Live"))) settings_->setSceneLive(live);
  if (accept(bug, QStringLiteral("Bug"))) settings_->setSceneBug(bug);
  settings_->setAutoSwitchEnabled(autoSwitch);
  // Repopule les combos du dock natif : même mapping, deux UIs, elles doivent
  // afficher la même chose. Rejoue aussi register_stream et updateSourcePanel,
  // donc le bridge apprend le mapping tout de suite plutôt qu'au prochain
  // heartbeat (parité avec les combos du dock).
  refreshSceneChoices();
  broadcastToWeb();
}

// Bouton « Corriger » du diagnostic : pose une source Semirprod dans la scene
// de direct.
//
// Le plugin n'a jamais rien pose de lui-meme, et ne le fera pas ici non plus :
// ce chemin ne s'ouvre que sur un clic. Ce qui disparait, c'est l'obligation de
// DEVINER qu'une source etait necessaire et dans quelle scene la mettre — pas
// le controle de l'operateur sur sa collection.
void SemirprodDock::placeStreamSourceInCurrentScene(const QString& streamId) {
  if (streamId.isEmpty()) return;

  // Le flux doit venir de la liste du compte : une commande du pont local ne
  // doit pas pouvoir faire poser une source sur un identifiant invente.
  QString label;
  bool known = false;
  for (const StreamItem& it : streams_) {
    if (it.id == streamId) {
      label = it.label;
      known = true;
      break;
    }
  }
  if (!known) {
    logEvent(QStringLiteral("warn"),
             loc("Journal.StreamNotInWorkspace"));
    return;
  }

  const QString scene = sceneController_->currentScene();
  if (scene.isEmpty()) {
    logEvent(QStringLiteral("warn"), loc("Journal.NoSceneSelected"));
    return;
  }

  const bool ok = sceneController_->placeStreamSourceInCurrentScene(streamId, label);
  logEvent(ok ? QStringLiteral("success") : QStringLiteral("error"),
           ok ? loc("Journal.SourceAdded")
                    .arg(label.isEmpty() ? streamId : label, scene)
              : loc("Journal.SourceAddFailed")
                    .arg(label.isEmpty() ? streamId : label, scene));

  updateSourcePanel();
  broadcastToWeb();
}

void SemirprodDock::placeSourceInLiveScene() {
  const QString live = settings_->sceneLive();
  if (live.isEmpty()) {
    logEvent(QStringLiteral("warn"),
             loc("Journal.ChooseLiveSceneFirst"));
    return;
  }
  if (!sceneController_->sceneExists(live)) {
    logEvent(QStringLiteral("warn"),
             loc("Journal.LiveSceneMissing")
                 .arg(live));
    return;
  }
  if (sceneController_->hasSemirprodSourceInScene(live)) {
    // Rien a faire, mais on le dit : un bouton qui ne repond pas se lit comme
    // un bouton casse.
    logEvent(QStringLiteral("info"),
             loc("Journal.SourceAlreadyIn").arg(live));
    broadcastToWeb();
    return;
  }

  const bool ok = sceneController_->placeSemirprodSourceInScene(live);
  logEvent(ok ? QStringLiteral("success") : QStringLiteral("error"),
           ok ? loc("Journal.SourcePlaced")
                    .arg(live)
              : loc("Journal.SourcePlaceFailed").arg(live));

  // Le panneau recalcule sourceInLiveScene_ au prochain tick, mais l'operateur
  // vient de cliquer : il attend une reponse maintenant, pas dans 200 ms.
  updateSourcePanel();
  broadcastToWeb();
}

void SemirprodDock::loadToken() {
  tokenStore_->load([this](const TokenBundle& bundle) { resumeWithBundle(bundle); });
}

// Ouvre (ou rouvre) la session a partir d'une liaison deja en main : lue dans
// le magasin par loadToken(), ou celle EN MEMOIRE pour une reprise apres echec
// (le trousseau n'est alors pas relu : verrouille, il aurait tue la session).
void SemirprodDock::resumeWithBundle(const TokenBundle& bundle) {
  // Pas de liaison v2 (jamais appairé, ou liaison d'un plugin 0.1.x) :
  // l'écran d'appairage s'affiche.
  //
  // Sauf trousseau illisible (verrouillé au démarrage) : la liaison n'est pas
  // absente, elle est hors d'atteinte. On le dit, pour qu'un nouvel
  // appairage (qui créerait un second appareil) ne soit pas le réflexe ; le
  // magasin réessaie seul et rappelle ce callback dès qu'il lit la liaison.
  if (!bundle.isBound()) {
    if (tokenStore_->isRefused()) {
      // Refus : aucun nouvel essai, ne pas laisser croire le contraire.
      logEvent(QStringLiteral("warn"), loc("Journal.KeychainRefused"));
    } else if (tokenStore_->isUnavailable()) {
      logEvent(QStringLiteral("warn"), loc("Journal.KeychainUnavailable"));
    }
    updateUiState();
    return;
  }
  liveClient_->setDeviceId(bundle.deviceId);
  // The cached access_token can easily be past TTL after a plugin rebuild
  // or a long OBS pause. Opening the WS with a dead token just makes the
  // server slam the door, and the auto-reconnect loop would then spin on
  // the same expired token forever. Refresh proactively when there's a
  // refresh_token available; deviceFlowSucceeded saves the new bundle and
  // calls connectWithToken() for us.
  // Session d'appareil de 15 min : au démarrage d'OBS elle est presque
  // toujours périmée. Une session neuve vérifie aussi que le poste est
  // toujours autorisé avant de se montrer « connecté ».
  if (!bundle.isValid() || bundle.isExpired(Config::sessionRenewBeforeSeconds)) {
    setStatus(QStringLiteral("Ouverture de la session Semirprod..."));
    authFlow_->refresh(bundle);
    updateUiState();
    return;
  }
  liveClient_->connectWithToken(bundle.accessToken);
  // load() n'emet pas tokenChanged : l'echeance est armee ici.
  armTokenRefresh();
  // Acces et identite : apportes par l'instantane de l'enregistrement du
  // poste. Le filet relit les routes HTTP s'il n'arrive pas a temps (sans
  // lui, le compte ne s'afficherait jamais pour qui relance OBS sans pont
  // joignable).
  armInitialFetchFallback();
  updateUiState();
}

void SemirprodDock::onExternalTokenStored() {
  // POURQUOI une methode publique appelee par plugin.cpp plutot qu'un
  // connect() sur TokenStore::tokenChanged :
  //
  //   1. Ce n'est PAS le meme TokenStore. PairingFlow recoit celui que
  //      startCloudPairing construit, le dock garde le sien ; seul le support
  //      (trousseau ou fichier) est commun. Le signal du magasin du dock ne
  //      part donc jamais a l'appairage, et s'abonner a celui-la n'aurait rien
  //      reveille du tout.
  //   2. Meme en partageant l'instance, tokenChanged est emis a CHAQUE save(),
  //      donc a chaque rotation de jeton en cours de session (voir
  //      TokenStore::save, qui l'emet avant meme d'ecrire) et a chaque clear().
  //      S'y brancher ferait rejouer cette chaine plusieurs fois par session
  //      sur un poste de bureau, ou rien n'a change : le contraire de
  //      l'exigence « rien ne bouge hors kiosque ».
  //
  // Le point d'entree est donc explicite, et sa seule source est le rappel de
  // PairingFlow, qui n'existe que dans la variante kiosque.

  // OBS se ferme : ne rien rouvrir pendant que libobs se demonte.
  if (shuttingDown_) return;

  // Deja relie : surtout ne pas rappeler loadToken(). LiveClient::connectWithToken
  // appelle socket_->open() sans regarder s'il en tient deja un ouvert, on se
  // retrouverait avec deux sessions WS pour la meme instance — dont une que
  // plus personne ne referme, et le serveur voit alors deux postes la ou il
  // n'y en a qu'un.
  if (liveClient_ && liveClient_->isConnected()) return;

  // Un appairage automatique n'a aucune raison de tomber pendant un appairage
  // interactif, mais si les deux se croisent c'est l'humain qui gagne : relire
  // le magasin sous les pieds de DeviceAuthFlow remplacerait le code affiche a
  // l'ecran par un etat « connecte » tire d'un autre jeton, et l'operateur
  // finirait de saisir un code devenu sans effet.
  if (authFlow_ && authFlow_->isRunning()) return;

  // Une ligne, sinon ce defaut redevient invisible : de l'exterieur, une
  // instance non authentifiee et une instance authentifiee tardivement se
  // ressemblent, elles se distinguent seulement par ce que dit le journal.
  // Aucun jeton ici, ni en clair ni tronque : le journal d'OBS part dans les
  // tickets de support.
  blog(LOG_INFO,
       "[semirprod-dock] authentification tardive : relecture du magasin de jetons");
  loadToken();
}

void SemirprodDock::refreshSceneChoices() {
  const QStringList scenes = sceneController_->availableScenes();

  // Le mapping est persisté par collection de scènes, mais on ne le réécrit
  // que face à une liste de scènes complète. OBS pompe la boucle d'événements
  // pendant le chargement d'une collection : ce slot peut donc tourner alors
  // que la nouvelle collection est déjà annoncée et que ses scènes n'ont pas
  // fini d'arriver. Écrire à cet instant enregistrerait un mapping tiré d'une
  // liste partielle. SCENE_COLLECTION_CHANGED rejoue cette fonction sur la
  // liste complète, donc on ne perd rien à attendre.
  const bool mappingWritable = !sceneCollectionChanging_ && !scenes.isEmpty();

  bool changed = false;
  if (mappingWritable) {
    // Un slot qui ne désigne plus une scène existante est périmé : scène
    // renommée/supprimée, ou mapping héritée de l'ancien format global. On le
    // vide pour que l'auto-pick ci-dessous reprenne la main, plutôt que de
    // garder un nom fantôme qui tue l'auto-switch sans rien dire.
    const auto dropIfMissing = [&](const QString& current, const QString& slot,
                                   auto&& setter) {
      if (current.isEmpty() || scenes.contains(current)) return;
      logEvent(QStringLiteral("warn"),
               loc("Journal.SceneNotFoundReset")
                   .arg(slot, current, Settings::activeSceneCollection()));
      setter(QString());
      changed = true;
    };
    dropIfMissing(settings_->sceneLive(), QStringLiteral("Live"),
                  [this](const QString& v) { settings_->setSceneLive(v); });
    dropIfMissing(settings_->sceneBug(), QStringLiteral("Bug"),
                  [this](const QString& v) { settings_->setSceneBug(v); });

    // First-run auto-pick : on remplit uniquement les emplacements vides pour
    // qu'un nouvel utilisateur (scène par défaut "Scène"/"Scène 2", ou tout
    // autre nommage) ait directement un mapping qui marche, sans avoir à
    // toucher l'onglet "Préférences scène". Idem pour une collection encore
    // jamais configurée. On ne touche jamais à un choix explicite — vide veut
    // dire "non configuré", on respecte.
    if (settings_->sceneLive().isEmpty()) {
      settings_->setSceneLive(scenes.first());
      changed = true;
    }
    if (settings_->sceneBug().isEmpty() && scenes.size() >= 2 &&
        scenes.at(1) != settings_->sceneLive()) {
      settings_->setSceneBug(scenes.at(1));
      changed = true;
    }
  }

  // Plus de combos à repeupler : la liste des scènes part à l'interface web
  // dans `_scenes`, et les deux slots dans `sceneLive` / `sceneBug`. C'est
  // Settings qui fait foi des deux côtés, donc il n'y a plus rien à
  // resynchroniser ici.

  // Le dashboard doit voir le mapping de la collection courante tout de suite,
  // sans attendre le prochain heartbeat : register_stream rejoue les valeurs
  // courantes côté bridge. Inconditionnel car un simple changement de
  // collection remplace le mapping sans passer par un setter (le bucket de la
  // nouvelle collection existait déjà) — `changed` serait faux alors que le
  // bridge tient encore celui de l'ancienne collection. L'envoi est idempotent.
  registerSelectedStream();
  if (changed) pushObsState();
  updateSourcePanel();
}

void SemirprodDock::refreshStreams() {
  const QString token = accessToken();
  if (token.isEmpty()) return;

  // Single-retry-on-401: the bound JWT lives ~1 h matching the server's
  // accessTokenExpiresIn, so /api/streams *can* hit expiry mid-session
  // (OBS resumed from sleep, clock skew). Refresh once and retry instead
  // of dumping the user back to the auth screen. If the refresh itself
  // fails or the retry also 401s, fall through silently — the
  // next pushed event or reconnect will retake the recovery path.
  http_->getJson(Config::apiBaseUrl() + QStringLiteral("/api/streams"), token,
      [this](const HttpResponse& response, const QJsonDocument& json) {
    if (response.statusCode == 401 &&
        !tokenStore_->cached().refreshToken.isEmpty() && !authFlow_->isRunning()) {
      blog(LOG_INFO, "[semirprod-dock] /api/streams returned 401 — refreshing token and retrying");
      // Hook the next tokenChanged emission once, then disconnect and retry.
      // tokenChanged fires from TokenStore::save (called by deviceFlowSucceeded)
      // — that's how we know the new bundle is in cached_. We must ALSO listen
      // for deviceFlowFailed: if the refresh fails, tokenChanged never fires,
      // so without a failure hook both connections + the heap guard would leak
      // and the stale success hook would replay a stray fetch on the next
      // unrelated tokenChanged. Both branches share one teardown.
      auto* success = new QMetaObject::Connection;
      auto* failure = new QMetaObject::Connection;
      auto teardown = [success, failure]() {
        QObject::disconnect(*success);
        QObject::disconnect(*failure);
        delete success;
        delete failure;
      };
      *success = connect(tokenStore_, &TokenStore::tokenChanged, this,
          [this, teardown]() {
        teardown();
        const QString fresh = tokenStore_->cached().accessToken;
        if (fresh.isEmpty()) return;
        // Replay without the retry hook — if this also 401s, give up and
        // let the watchdog handle it.
        http_->getJson(Config::apiBaseUrl() + QStringLiteral("/api/streams"), fresh,
            [this](const HttpResponse&, const QJsonDocument& retryJson) {
          applyStreamsResponse(retryJson);
        });
      });
      *failure = connect(authFlow_, &DeviceAuthFlow::deviceFlowFailed, this,
          [teardown](const QString&) {
        // Refresh failed — drop the hooks and let the watchdog recover.
        teardown();
      });
      authFlow_->refresh(tokenStore_->cached());
      return;
    }
    applyStreamsResponse(json);
  });
}

void SemirprodDock::applyStreamsResponse(const QJsonDocument& json) {
    if (!json.isObject()) return;
    auto array = json.object().value(QStringLiteral("streams")).toArray();
    streams_.clear();
    QList<StreamEntry> registryEntries;

    for (const auto& v : array) {
      auto o = v.toObject();
      StreamItem it;
      it.id = o.value(QStringLiteral("id")).toString();
      it.label = o.value(QStringLiteral("name")).toString();
      it.rtmpUrl = o.value(QStringLiteral("rtmpUrl")).toString();
      it.streamKey = o.value(QStringLiteral("streamKey")).toString();
      it.srtReadUrl = o.value(QStringLiteral("srtReadUrl")).toString();
      streams_.push_back(it);

      StreamEntry re;
      re.id = it.id;
      re.label = it.label;
      re.rtmpUrl = it.rtmpUrl;
      re.streamKey = it.streamKey;
      re.srtReadUrl = it.srtReadUrl;
      registryEntries.append(re);
    }

    StreamRegistry::instance().set(registryEntries);

    QString activeId = settings_->selectedStreamId();
    bool stillExists = false;
    for (const auto& e : registryEntries) {
      if (e.id == activeId) { stillExists = true; break; }
    }
    if (!stillExists) {
      activeId = registryEntries.isEmpty() ? QString() : registryEntries.first().id;
      if (!activeId.isEmpty()) settings_->setSelectedStreamId(activeId);
    }
    if (!activeId.isEmpty()) {
      if (studioAccessKnown_ && !studioAccess_) {
        StreamRegistry::instance().setActiveStreamId(QString());
      } else {
        StreamRegistry::instance().setActiveStreamId(activeId);
      }
      reapplyActiveStreamToAllInstances();
    } else if (!settings_->selectedStreamId().isEmpty()) {
      // Le dernier flux vient d'être supprimé : on lâche celui qu'on lisait.
      // Sans ça, le registre gardait l'ancien flux actif et les sources
      // continuaient d'ouvrir une clé qui n'existe plus.
      settings_->setSelectedStreamId(QString());
      StreamRegistry::instance().setActiveStreamId(QString());
      reapplyActiveStreamToAllInstances();
    }

    registerSelectedStream();
    updateUiState();
    broadcastToWeb();
}

void SemirprodDock::onStreamDeleted(const QString& streamId) {
  bool removed = false;
  for (qsizetype i = streams_.size() - 1; i >= 0; --i) {
    if (streams_[i].id == streamId) {
      streams_.remove(i);
      removed = true;
    }
  }
  const bool wasOurs = settings_->selectedStreamId() == streamId;
  if (!removed && !wasOurs) return;
  blog(LOG_INFO, "[semirprod-dock] stream_deleted : flux retire de la liste%s",
       wasOurs ? " (c'etait la cible du poste)" : "");

  QList<StreamEntry> registryEntries;
  for (const StreamItem& it : streams_) {
    StreamEntry re;
    re.id = it.id;
    re.label = it.label;
    re.rtmpUrl = it.rtmpUrl;
    re.streamKey = it.streamKey;
    re.srtReadUrl = it.srtReadUrl;
    registryEntries.append(re);
  }
  StreamRegistry::instance().set(registryEntries);

  if (wasOurs) {
    // Meme effet que la suppression du dernier flux dans applyStreamsResponse :
    // on lache le flux qu'on lisait, sinon les sources continueraient d'ouvrir
    // une cle qui n'existe plus. Pas de bascule automatique sur un autre flux :
    // le pont a deja vide la cible, c'est a la personne d'en choisir une.
    settings_->setSelectedStreamId(QString());
    StreamRegistry::instance().setActiveStreamId(QString());
    registerSelectedStream();
  }
  reapplyActiveStreamToAllInstances();
  updateUiState();
  broadcastToWeb();
}

void SemirprodDock::onInstanceRenamed(const QString& label) {
  const QString trimmed = label.trimmed().left(80);
  if (trimmed == settings_->serverInstanceLabel()) return;
  settings_->setServerInstanceLabel(trimmed);
  logEvent(QStringLiteral("info"),
           loc("Journal.InstanceName").arg(settings_->displayInstanceLabel()));
  broadcastToWeb();
}

void SemirprodDock::applySelectedStream() {
  if (studioAccessKnown_ && !studioAccess_) {
    StreamRegistry::instance().setActiveStreamId(QString());
    reapplyActiveStreamToAllInstances();
    return;
  }
  // Le flux courant se lit maintenant dans Settings, seule source de vérité —
  // c'était l'index du combo, qu'il fallait garder synchronisé avec `streams_`
  // à trois endroits distincts. La borne HAUTE reste : `QVector::operator[]`
  // hors bornes est un comportement indéfini, pas un crash propre.
  const int i = selectedStreamIndexById(settings_->selectedStreamId());
  if (i >= 0 && i < streams_.size()) {
    StreamRegistry::instance().setActiveStreamId(streams_[i].id);
    reapplyActiveStreamToAllInstances();
  }
}

void SemirprodDock::registerSelectedStream() {
  // Fusion des appels rapproches.
  //
  // Quatre chemins appellent cette fonction, et ils se declenchent tous au
  // demarrage : connexion du WS, chargement des flux, chargement de la
  // collection de scenes, ouverture de session. Le journal du 15 aout montre
  // TROIS `register_stream` identiques en 7 ms — inutiles a nous, et surtout
  // ambigus pour le backend, qui peut y voir trois instances et rediffuser
  // ensuite chaque commande autant de fois. C'est ce qui a amene le doublon de
  // `start_program_preview` qui faisait tomber OBS.
  //
  // On ne garde donc que le dernier appel d'une rafale. Le delai est court : il
  // n'existe que pour absorber la salve d'initialisation, pas pour retarder une
  // vraie reconfiguration.
  if (!registerCoalesceTimer_) {
    registerCoalesceTimer_ = new QTimer(this);
    registerCoalesceTimer_->setSingleShot(true);
    connect(registerCoalesceTimer_, &QTimer::timeout, this, [this]() {
      // Même hors ligne : la cible est gardée et annoncée à l'enregistrement.
      liveClient_->registerStream(settings_->selectedStreamId(),
                                  settings_->sceneLive(),
                                  settings_->sceneBug());
    });
  }
  registerCoalesceTimer_->start(250);
}

void SemirprodDock::updateUiState() {
  const bool connected = !accessToken().isEmpty();

  // Hors appairage en cours, la zone de code redevient un simple état de
  // compte : connecté ou pas. Les boutons Connecter / Déconnecter vivent dans
  // l'interface web, qui les active depuis `connected`.
  //
  // ÉCRIT AVANT LA DIFFUSION. Ces deux champs partent dans le même instantané :
  // quand le libellé était un QLabel, il était écrit APRÈS broadcastToWeb() et
  // l'interface recevait donc la valeur précédente, jusqu'au prochain
  // événement — « Compte Semirprod connecté. » arrivait avec un tour de retard.
  if (!authFlow_->isRunning()) {
    accountLabel_ = connected
        ? loc("Account.Connected")
        : loc("Account.NoSession");
    accountUrl_.clear();
    pairingFailed_ = false;
  }
  broadcastToWeb();
  updateStudioNotice();
}

/// Récupère l'identité du compte connecté et la rediffuse à l'interface.
///
/// Le dock annonçait « connecté » sans dire à QUI, et la carte Compte de
/// l'interface web affichait le CODE D'APPAIRAGE dans `accountLabel` : une fois
/// appairé, ce champ se vidait et l'écran retombait sur « Aucun compte relié »
/// alors qu'un compte était bien lié. Or le plugin publie sur les streams de
/// l'organisation de ce compte : sur un poste partagé, ou après un ré-appairage,
/// rien ne permettait de s'en apercevoir avant qu'un direct ne parte au mauvais
/// endroit.
///
/// Le plugin demandait déjà les portées `openid profile email` sans jamais s'en
/// servir : il suffisait d'appeler /api/me.
///
/// Échec silencieux volontaire : c'est un confort d'affichage, pas une étape de
/// l'appairage. Une erreur réseau ne doit ni bloquer, ni alarmer.
void SemirprodDock::refreshAccountIdentity() {
  const QString token = accessToken();
  if (token.isEmpty()) {
    accountName_.clear();
    accountEmail_.clear();
    accountId_.clear();
    accountAvatarSourceUrl_.clear();
    accountAvatarBytes_.clear();
    accountAvatarType_.clear();
    accountAvatarTag_.clear();
    broadcastToWeb();
    return;
  }

  http_->getJson(Config::apiBaseUrl() + QStringLiteral("/api/me"), token,
                 [this](const HttpResponse& res, const QJsonDocument& doc) {
    if (!res.ok() || !doc.isObject()) {
      return;
    }
    // Cette API enveloppe ses reponses dans {success, data} — /api/live/access
    // et /api/plugin/latest lisent bien `data`. Ici on lisait la RACINE, donc
    // name et email restaient vides et la carte Compte retombait sur son
    // libelle par defaut. On accepte les deux formes plutot que de parier.
    const QJsonObject root = doc.object();
    const QJsonObject data = root.value(QStringLiteral("data")).toObject();
    const QJsonObject o = data.isEmpty() ? root : data;

    // Les CLES seulement, jamais les valeurs : le journal OBS part avec les
    // rapports de bug. De quoi verifier la forme reelle sans exposer le compte.
    blog(LOG_INFO, "[semirprod-dock] /api/me champs: %s",
         o.keys().join(QLatin1Char(',')).toUtf8().constData());

    accountName_ = o.value(QStringLiteral("name")).toString().trimmed();
    accountEmail_ = o.value(QStringLiteral("email")).toString().trimmed();
    // Graine du dégradé par défaut. L'identifiant et non le nom : un compte
    // renommé garderait sa couleur, et c'est ce que le site fait déjà.
    accountId_ = o.value(QStringLiteral("id")).toString().trimmed();

    // Le nom du champ portant l'avatar n'est pas fixé par un contrat écrit
    // côté plugin — selon la brique d'auth c'est `image`, `avatarUrl`,
    // `avatar` ou `picture`. On prend le premier renseigné et on consigne
    // LEQUEL, pour pouvoir resserrer ce code une fois la réponse observée.
    // On ne consigne pas l'URL : sur un stockage privé elle porte parfois un
    // jeton signé, et le journal OBS part avec les rapports de bug.
    static const char* kAvatarKeys[] = {"image", "avatarUrl", "avatar", "picture"};
    QString avatarUrl;
    for (const char* key : kAvatarKeys) {
      const QString v = o.value(QLatin1String(key)).toString().trimmed();
      if (!v.isEmpty()) {
        avatarUrl = v;
        blog(LOG_INFO, "[semirprod-dock] avatar du compte : champ « %s »", key);
        break;
      }
    }
    fetchAccountAvatar(avatarUrl);

    // L'instantané est complet à chaque envoi : une rediffusion suffit à faire
    // apparaître l'identité, sans mécanisme incrémental.
    broadcastToWeb();
  });
}

/// Rapatrie l'avatar pour pouvoir le servir depuis le pont local.
///
/// Refus assumé si l'image est hébergée hors du domaine Semirprod : HttpClient
/// applique l'allowlist à TOUTE requête, et on ne l'assouplit pas pour une
/// image. Un avatar chez un tiers (Google, Gravatar) n'est pas récupéré — on
/// retombe alors sur fetchDefaultAvatar(). Pour les activer, il faut soit les
/// servir depuis app.semirprod.fr, soit ajouter l'hôte à l'allowlist en
/// connaissance de cause.
///
/// TOUS les chemins d'échec appellent fetchDefaultAvatar() : c'est ce qui
/// permet à l'interface de n'avoir aucun repli à dessiner. Un `return` nu
/// ajouté ici lui redonnerait la charge de décider quoi afficher, et c'est
/// précisément ce qui avait produit deux couleurs pour un même compte.
///
/// `/api/me` renvoie `image` tel qu'il est stocké en base, c'est-à-dire un
/// CHEMIN RELATIF (`/api/avatar/<userId>?v=<horodatage>`) : le bucket est privé
/// et la route sert de proxy, il n'y a donc jamais d'URL absolue à stocker.
/// Un chemin n'a pas d'hôte, `QUrl::host()` rend une chaîne vide, et
/// l'allowlist refusait TOUS les avatars — le dégradé s'affichait même pour un
/// compte qui en avait un. D'où la résolution ci-dessous, avant le contrôle.
void SemirprodDock::fetchAccountAvatar(const QString& rawUrl) {
  if (rawUrl.isEmpty()) {
    fetchDefaultAvatar();
    return;
  }

  // Résolution du chemin relatif contre l'API. Le `//` est écarté à part :
  // `//evil.com/x` est une URL « protocol-relative » que `QUrl::resolved()`
  // interpréterait comme un CHANGEMENT D'HÔTE tout en ressemblant à un chemin.
  // L'allowlist ci-dessous le rattraperait, mais un refus explicite vaut mieux
  // qu'un refus par effet de bord.
  QString url = rawUrl;
  if (url.startsWith(QLatin1Char('/'))) {
    if (url.startsWith(QLatin1String("//"))) {
      blog(LOG_WARNING,
           "[semirprod-dock] avatar ignore : chemin protocol-relative refuse");
      fetchDefaultAvatar();
      return;
    }
    url = QUrl(Config::apiBaseUrl()).resolved(QUrl(url)).toString();
  }

  // Le cache porte sur l'URL RÉSOLUE : deux instantanés successifs de
  // `/api/me` donnent le même chemin relatif, donc la même absolue, et l'image
  // n'est rapatriée qu'une fois.
  if (url == accountAvatarSourceUrl_ && !accountAvatarBytes_.isEmpty()) {
    return;  // déjà en cache, même source
  }
  if (!Config::isAllowedHost(url)) {
    blog(LOG_INFO,
         "[semirprod-dock] avatar ignore : hote hors allowlist (dégradé par défaut)");
    fetchDefaultAvatar();
    return;
  }

  // 512 Ko : très large pour un avatar, assez bas pour qu'une réponse inattendue
  // ne se retrouve pas entièrement en mémoire.
  static constexpr qint64 kMaxAvatarBytes = 512 * 1024;
  // Pas de jeton sur cette requête : c'est une image, et l'envoyer à une URL de
  // média n'apporte rien qu'un risque de fuite si l'hôte change un jour.
  http_->getBytes(url, QString(), kMaxAvatarBytes,
                  [this, url](const HttpResponse& res, const QString& contentType) {
    // Un échec n'est pas une absence d'avatar : sans repli explicite, la carte
    // Compte gardait l'image du compte PRÉCÉDENT jusqu'au redémarrage d'OBS.
    if (!res.ok() || res.body.isEmpty()) {
      fetchDefaultAvatar();
      return;
    }
    // PNG, JPEG, WebP ou GIF seulement : une photo de profil n'a aucune raison
    // d'etre un SVG, format qui porte du script (voir acceptedAvatarType).
    const QString type = acceptedAvatarType(contentType, /*allowSvg=*/false);
    if (type.isEmpty()) {
      blog(LOG_WARNING,
           "[semirprod-dock] avatar ignore : type d'image non accepte");
      fetchDefaultAvatar();
      return;
    }
    accountAvatarSourceUrl_ = url;
    accountAvatarBytes_ = res.body;
    accountAvatarType_ = type;
    accountAvatarTag_ = QString::fromLatin1(
        QCryptographicHash::hash(res.body, QCryptographicHash::Md5).toHex().left(8));
    blog(LOG_INFO, "[semirprod-dock] avatar du compte charge (%lld octets)",
         static_cast<long long>(res.body.size()));
    broadcastToWeb();
  });
}

/// Rapatrie le dégradé par défaut du compte, dessiné par le site.
///
/// ─── Pourquoi le site le dessine plutôt que l'interface ─────────────────────
///
/// Ce dégradé sert à reconnaître un compte d'un coup d'œil. Il ne vaut donc que
/// s'il est le MÊME partout. Tant que chaque plateforme le calculait, il ne
/// l'était pas : l'interface de ce plugin affichait un dégradé neutre, sans
/// rapport avec celui du site, et l'application iOS recopiait l'algorithme à
/// l'octet près en pariant que personne ne toucherait un seul des deux
/// fichiers. Le site rend maintenant l'image sur `/avatars/<id>` ; ici, c'est
/// une image comme une autre.
///
/// Elle passe par le même chemin que la photo du compte — HttpClient, allowlist,
/// service local sur `/avatar` — donc la page ne va toujours chercher aucune
/// ressource distante, et la CSP reste `img-src 'self'`. `semirprod.fr` est
/// déjà dans l'allowlist (les builds de dev y ajoutent leurs hôtes de test) :
/// aucun hôte n'est ouvert pour cette fonction.
///
/// Sans repli local : si la requête échoue, l'interface affiche sa pastille
/// d'attente. Deux couleurs pour un même compte est exactement ce qu'on retire.
void SemirprodDock::fetchDefaultAvatar() {
  if (accountId_.isEmpty()) {
    // Pas de compte lié, ou /api/me n'a pas encore répondu : il n'y a pas de
    // graine, donc pas de dégradé à demander.
    accountAvatarSourceUrl_.clear();
    accountAvatarBytes_.clear();
    accountAvatarType_.clear();
    accountAvatarTag_.clear();
    return;
  }

  // Les initiales dessinées par dessus. Le nom d'affichage s'il existe, sinon
  // la partie locale de l'adresse — sans quoi tous les comptes d'un domaine
  // partageraient la même lettre. Le site coupe sur l'espace, le point, le
  // tiret et le souligné, donc « alex.martin » rend bien AM.
  //
  // Ce paramètre n'entre PAS dans le choix des couleurs : un compte renommé
  // change d'initiales, jamais de dégradé.
  const QString label = accountName_.isEmpty()
      ? accountEmail_.section(QLatin1Char('@'), 0, 0)
      : accountName_;

  QUrl url(Config::siteBaseUrl());
  url.setPath(QStringLiteral("/avatars/%1").arg(accountId_));
  QUrlQuery query;
  if (!label.isEmpty()) query.addQueryItem(QStringLiteral("n"), label);
  // 128 px : la page l'affiche en 36, l'écart couvre les écrans à forte densité.
  query.addQueryItem(QStringLiteral("s"), QStringLiteral("128"));
  url.setQuery(query);
  const QString target = url.toString();

  // Déjà en cache pour cette même graine : rien à refaire. `/api/me` est
  // réinterrogé à chaque rafraîchissement du compte, la comparaison évite un
  // aller-retour par appel.
  if (target == accountAvatarSourceUrl_ && !accountAvatarBytes_.isEmpty()) {
    return;
  }

  // La cible a changé : l'image en mémoire est celle d'un AUTRE état du compte
  // (photo retirée, ou compte précédent après un ré-appairage). On la lâche
  // avant de demander la nouvelle, sans quoi un échec réseau laisserait
  // afficher l'ancienne indéfiniment.
  accountAvatarSourceUrl_.clear();
  accountAvatarBytes_.clear();
  accountAvatarType_.clear();
  accountAvatarTag_.clear();
  broadcastToWeb();

  if (!Config::isAllowedHost(target)) {
    // Ne peut arriver qu'avec une base de site mal configurée en build de dev.
    blog(LOG_WARNING,
         "[semirprod-dock] degrade par defaut ignore : hote hors allowlist");
    return;
  }

  static constexpr qint64 kMaxDefaultAvatarBytes = 64 * 1024;
  // Pas de jeton : la route est publique et ne rend qu'un dessin déduit de
  // l'identifiant. L'y envoyer n'apporterait qu'un risque de fuite.
  http_->getBytes(target, QString(), kMaxDefaultAvatarBytes,
                  [this, target](const HttpResponse& res, const QString& contentType) {
    if (!res.ok() || res.body.isEmpty()) return;
    // Le degrade est un SVG dessine par notre site : seul ce chemin-ci accepte
    // ce format.
    const QString type = acceptedAvatarType(contentType, /*allowSvg=*/true);
    if (type.isEmpty()) {
      blog(LOG_WARNING,
           "[semirprod-dock] degrade par defaut ignore : type d'image non accepte");
      return;
    }
    accountAvatarSourceUrl_ = target;
    accountAvatarBytes_ = res.body;
    accountAvatarType_ = type;
    accountAvatarTag_ = QString::fromLatin1(
        QCryptographicHash::hash(res.body, QCryptographicHash::Md5).toHex().left(8));
    blog(LOG_INFO, "[semirprod-dock] degrade par defaut charge (%lld octets)",
         static_cast<long long>(res.body.size()));
    broadcastToWeb();
  });
}

void SemirprodDock::checkStudioAccess() {
  const QString token = accessToken();
  if (token.isEmpty() || studioCheckInFlight_) return;

  // Fenetre minimale entre deux appels reels. Un changement d'abonnement est
  // pousse par le serveur, donc 10 s de latence au pire n'a aucun impact
  // percu ; en echange une rafale d'evenements (retries de webhook Stripe,
  // reconnexions WS en cascade) ne se traduit plus par une rafale de requetes.
  static constexpr qint64 kStudioCheckMinIntervalMs = 10'000;

  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  const qint64 sinceLast = now - studioCheckLastMs_;
  if (studioCheckLastMs_ > 0 && sinceLast < kStudioCheckMinIntervalMs) {
    // Une seule verification differee en attente : les demandes suivantes de
    // la fenetre se fondent dedans.
    if (studioCheckQueued_) return;
    studioCheckQueued_ = true;
    // Cast explicite : singleShot prend un int, et une conversion implicite
    // depuis qint64 declenche C4244 sur MSVC.
    const int delayMs = static_cast<int>(kStudioCheckMinIntervalMs - sinceLast);
    QTimer::singleShot(delayMs, this, [this]() {
      studioCheckQueued_ = false;
      checkStudioAccess();
    });
    return;
  }

  studioCheckLastMs_ = now;
  studioCheckInFlight_ = true;
  http_->getJson(Config::apiBaseUrl() + QStringLiteral("/api/live/access"), token,
      [this](const HttpResponse& response, const QJsonDocument& json) {
    studioCheckInFlight_ = false;
    // 401: the access token just expired — /api/streams owns the refresh+retry
    // path, so skip this cycle and re-check on the next tick. Any other
    // transport error: fail-open (keep prior state) so a flaky endpoint never
    // blocks a legitimately entitled streamer.
    if (response.statusCode != 200 || !json.isObject()) return;
    const auto data = json.object().value(QStringLiteral("data")).toObject();
    const bool access = data.value(QStringLiteral("access")).toBool();
    const bool isPartner = data.value(QStringLiteral("isPartner")).toBool();
    const QString reason = data.value(QStringLiteral("reason")).toString();
    applyStudioAccess(access, isPartner, reason);
    // Meme declencheur et meme anti-martelage que l'acces : un changement
    // d'abonnement change aussi les fonctions ouvertes.
    checkPluginFeatures();
  });
}

void SemirprodDock::checkPluginFeatures() {
  const QString token = accessToken();
  if (token.isEmpty() || pluginFeaturesInFlight_) return;
  pluginFeaturesInFlight_ = true;
  http_->getJson(Config::apiBaseUrl() + QStringLiteral("/api/obs/plugin/features"), token,
      [this](const HttpResponse& response, const QJsonDocument& json) {
    pluginFeaturesInFlight_ = false;
    // Echec = on garde la derniere matrice connue. Une panne ne doit ni ouvrir
    // ni fermer une fonction a l'ecran ; le serveur tranche de toute facon.
    if (response.statusCode != 200 || !json.isObject()) return;
    applyPluginFeatures(json.object().value(QStringLiteral("data")).toObject());
  });
}

void SemirprodDock::applyPluginFeatures(const QJsonObject& data) {
  if (data.isEmpty()) return;
  const QByteArray next = QJsonDocument(data).toJson(QJsonDocument::Compact);
  if (next == pluginFeaturesJson_) return;
  pluginFeaturesJson_ = next;
  broadcastToWeb();
}

namespace {

// Delai du filet HTTP. L'instantane arrive normalement moins d'une seconde
// apres l'ouverture du socket ; au-dela, le pont est injoignable ou ancien.
constexpr int kInitialFetchFallbackMs = 8000;

// Les routes HTTP enveloppent leur reponse dans `{success, data}` ; le pont
// peut relayer l'une ou l'autre forme. On accepte les deux.
QJsonObject unwrapData(const QJsonValue& value) {
  const QJsonObject o = value.toObject();
  const QJsonValue data = o.value(QStringLiteral("data"));
  return data.isObject() ? data.toObject() : o;
}

} // namespace

void SemirprodDock::armInitialFetchFallback() {
  if (shuttingDown_) return;
  if (!initialFetchTimer_) {
    initialFetchTimer_ = new QTimer(this);
    initialFetchTimer_->setSingleShot(true);
    initialFetchTimer_->setInterval(kInitialFetchFallbackMs);
    connect(initialFetchTimer_, &QTimer::timeout, this, [this]() {
      if (shuttingDown_ || accessToken().isEmpty()) return;
      blog(LOG_INFO, "[semirprod-dock] pas d'instantane d'enregistrement : relecture HTTP");
      runInitialHttpFetch();
    });
  }
  // Redemarre : loadToken puis l'ouverture du socket l'arment tous les deux,
  // un seul filet part.
  initialFetchTimer_->start();
}

void SemirprodDock::runInitialHttpFetch() {
  // checkStudioAccess enchaine /api/streams (si acces) et les fonctions.
  checkStudioAccess();
  refreshAccountIdentity();
}

void SemirprodDock::applyRegistrationResult(const QJsonObject& message) {
  if (initialFetchTimer_) initialFetchTimer_->stop();
  if (!message.value(QStringLiteral("snapshot")).toBool(false)) {
    blog(LOG_INFO, "[semirprod-dock] enregistrement sans instantane : relecture HTTP");
    runInitialHttpFetch();
    return;
  }

  // Identite. L'instantane ne porte ni l'identifiant ni l'avatar : /api/me
  // n'est relu qu'une fois par session OBS, quand on ne les a pas encore.
  const QJsonValue accountValue = message.value(QStringLiteral("account"));
  if (accountValue.isObject()) {
    const QJsonObject account = accountValue.toObject();
    accountName_ = account.value(QStringLiteral("name")).toString().trimmed();
    accountEmail_ = account.value(QStringLiteral("email")).toString().trimmed();
    if (accountId_.isEmpty()) refreshAccountIdentity();
  } else {
    refreshAccountIdentity();
  }
  const QJsonValue orgValue = message.value(QStringLiteral("orgName"));
  if (orgValue.isString()) serverOrgName_ = orgValue.toString().trimmed();

  // Acces. Bloc absent : chemin HTTP complet (il relit aussi flux et fonctions).
  const QJsonValue accessValue = message.value(QStringLiteral("access"));
  if (!accessValue.isObject()) {
    checkStudioAccess();
    broadcastToWeb();
    return;
  }
  const QJsonObject access = unwrapData(accessValue);
  const bool allowed = access.value(QStringLiteral("access")).toBool();
  applyStudioAccess(allowed, access.value(QStringLiteral("isPartner")).toBool(),
                    access.value(QStringLiteral("reason")).toString(),
                    /*refetchStreams=*/false);

  const QJsonValue featuresValue = message.value(QStringLiteral("features"));
  if (featuresValue.isObject()) {
    applyPluginFeatures(unwrapData(featuresValue));
  } else {
    checkPluginFeatures();
  }

  // Flux : lus seulement avec l'acces, comme le chemin HTTP (applyStudioAccess
  // ne lit /api/streams que si l'acces est ouvert). `null` = l'abonnement ne
  // couvre pas l'espace, traite comme le 403 de /api/streams (liste vide).
  if (allowed) {
    const QJsonValue streamsValue = message.value(QStringLiteral("streams"));
    if (streamsValue.isArray()) {
      QJsonObject wrapped;
      wrapped.insert(QStringLiteral("streams"), streamsValue.toArray());
      applyStreamsResponse(QJsonDocument(wrapped));
    } else if (streamsValue.isNull()) {
      applyStreamsResponse(QJsonDocument(QJsonObject()));
    } else {
      refreshStreams();
    }
  }
  broadcastToWeb();
}

void SemirprodDock::applyStudioAccess(bool access, bool isPartner, const QString& reason,
                                      bool refetchStreams) {
  Q_UNUSED(isPartner);
  studioAccessKnown_ = true;
  studioAccess_ = access;
  studioReason_ = reason;
  blog(LOG_INFO, "[semirprod-dock] studio access=%d reason=%s",
       access ? 1 : 0, reason.toUtf8().constData());
  updateStudioNotice();

  if (access) {
    if (refetchStreams) refreshStreams();
  } else {
    // Clear active stream in registry so we stop reading it
    StreamRegistry::instance().setActiveStreamId(QString());
    reapplyActiveStreamToAllInstances();

    streams_.clear();
    broadcastToWeb();

    // PAS de fenetre modale. Une QMessageBox « Accès refusé » en rouge volait le
    // focus d'OBS et disait la meme chose a tout le monde — y compris a un abonne
    // dont seul le paiement avait echoue. Le bandeau de l'interface
    // (`SubscribeNotice`) porte le vrai motif et le bouton qui le resout, lus
    // dans la matrice serveur.
    //
    // Une popup quand meme, mais UNE par session d'OBS : sans elle, un
    // streamer dont l'acces vient de tomber ne comprend pas pourquoi plus rien
    // ne charge. Le journal ne note que le passage a « refuse », pas chaque
    // verification.
    if (!accessToken().isEmpty()) {
      if (!lastAccessDeniedLogged_) {
        logEvent(QStringLiteral("warn"),
                 loc("Journal.AccessUnavailable"));
      }
      lastAccessDeniedLogged_ = true;
      maybePromptAccessDenied();
    }
  }
  if (access) lastAccessDeniedLogged_ = false;
}

void SemirprodDock::maybePromptAccessDenied() {
  // Instance infogeree : personne devant l'ecran.
  if (!edition::showsBlockingPrompts()) return;
  if (shuttingDown_ || !obsLoaded_ || accessDeniedPromptShown_ || pluginBlocked_) return;
  // Jamais par dessus un direct ou un enregistrement.
  if (obs_frontend_streaming_active() || obs_frontend_recording_active()) return;
  accessDeniedPromptShown_ = true;
  auto* box = new QMessageBox(static_cast<QWidget*>(obs_frontend_get_main_window()));
  box->setAttribute(Qt::WA_DeleteOnClose);
  box->setWindowModality(Qt::NonModal);
  box->setIcon(QMessageBox::Information);
  box->setWindowTitle(QStringLiteral("Semirprod"));
  box->setTextFormat(Qt::PlainText);
  box->setText(loc("Box.AccessDenied"));
  QPushButton* subscribe = box->addButton(loc("Box.SeePlans"), QMessageBox::AcceptRole);
  box->addButton(loc("Box.Later"), QMessageBox::RejectRole);
  box->setDefaultButton(subscribe);
  connect(box, &QMessageBox::buttonClicked, this, [this, subscribe](QAbstractButton* b) {
    if (b == subscribe) openSubscribePage();
  });
  box->show();
}

/// Le bandeau paywall du dock a disparu avec l'arbre de widgets ; l'interface
/// web rend l'état depuis studioAccess / studioAccessKnown / studioReason,
/// qu'elle reçoit déjà. Il ne reste que le message d'état.
void SemirprodDock::updateStudioNotice() {
  // Only nag when we positively know access is denied and the account is
  // connected. Unknown (fail-open), allowed, or signed-out -> nothing to say.
  const bool deny = studioAccessKnown_ && !studioAccess_ && !accessToken().isEmpty();
  if (deny) {
    setStatus(QStringLiteral(
        "Accès au direct indisponible. Le motif est affiché dans le panneau Semirprod."));
  }
}

void SemirprodDock::setStatus(const QString& message) {
  statusMessage_ = message;
}

void SemirprodDock::updateSourcePanel() {
  // Plus de panneau : l'état de la source part à l'interface web dans
  // `sourceReady` / `sourceWidth` / `sourceHeight` (broadcastToWeb), et au
  // backend via reportSourceStateIfChanged() en fin de fonction. Ce qui suit
  // est la logique qui n'a jamais été de l'affichage.
  const InstanceState s = queryInstanceState();

  // Notify once when the incoming feed exceeds 1080p and is being capped at
  // ingest. Re-armed when the feed is no longer oversize so a later stream
  // notifies again. No dimensions/tech names in the message.
  if (s.anyOversize) {
    if (!ingestOversizeNotified_) {
      ingestOversizeNotified_ = true;
      setStatus(QStringLiteral(
          "Flux entrant supérieur à 1080p. Affichage limité à 1080p."));
    }
  } else {
    ingestOversizeNotified_ = false;
  }

  // Warn explicitly when the configured scenes are missing from OBS — that's
  // the silent-fail mode that bit Adrien : auto-switch obeys a guard that
  // exits early when current scene matches neither sceneLive nor sceneBug,
  // so a stale "Live" string in settings made every flip look broken.
  const QString live = settings_->sceneLive();
  const QString bug = settings_->sceneBug();
  const QStringList available = sceneController_->availableScenes();
  QStringList missing;
  if (!live.isEmpty() && !available.contains(live)) {
    missing.append(loc("Warn.LiveSceneRef").arg(live));
  }
  if (!bug.isEmpty() && !available.contains(bug)) {
    missing.append(loc("Warn.FallbackSceneRef").arg(bug));
  }
  if (live.isEmpty()) missing.append(loc("Warn.LiveNotConfigured"));
  if (bug.isEmpty()) missing.append(loc("Warn.FallbackNotConfigured"));

  // Cet avertissement s'affichait dans un libellé du dock, donc nulle part. Il
  // part maintenant au Journal (et au log OBS), qui sont vus. Latché sur son
  // propre texte : cette fonction tourne toutes les 200 ms, seul un changement
  // de diagnostic est consigné.
  //
  // Rien à diagnostiquer tant qu'OBS n'a pas publié ses scènes : au démarrage,
  // ce panneau tourne AVANT le chargement de la collection, donc `available`
  // est vide et les deux slots configurés paraissent introuvables. En libellé
  // c'était un affichage fugace ; en journal ça laissait une alerte fausse et
  // permanente. Même raison pendant une bascule de collection.
  const bool sceneListSettled = !available.isEmpty() && !sceneCollectionChanging_;

  // DEUXIEME CAUSE DE PANNE MUETTE, ET LA PLUS FREQUENTE : les deux scenes
  // sont bien configurees et bien presentes, mais la scene de direct ne
  // contient aucune source Semirprod.
  //
  // maybeFlipToLive() interroge isSemirprodSourceReadyInScene(sceneLive), qui
  // demande « cette scene-la montre-t-elle des images ». Source posee dans une
  // autre scene, ou seulement en apercu : la reponse est non, la bascule ne
  // part jamais, et RIEN ne le disait — ni le Journal, ni l'interface. C'etait
  // a l'operateur de deviner qu'une source devait exister, et dans quelle
  // scene.
  //
  // On ne la pose pas d'office pour autant : voir
  // SceneController::placeSemirprodSourceInScene. Ici on nomme le probleme,
  // l'interface offre le geste.
  sourceInLiveScene_ =
      !live.isEmpty() && sceneController_->hasSemirprodSourceInScene(live);
  // La liste « ou est-elle vraiment » n'est calculee que quand elle manque a
  // l'appel. Elle parcourt TOUTES les scenes et enumere les elements de
  // chacune : la payer 5 fois par seconde sur le fil d'interface d'OBS, pour
  // une collection de trente scenes, alors que le cas nominal est « la source
  // est la et il n'y a rien a dire », coute pour rien.
  sourceScenes_ = sourceInLiveScene_
                      ? QStringList()
                      : sceneController_->scenesContainingSemirprodSource();

  const bool liveSceneUsable =
      !live.isEmpty() && available.contains(live) && sceneListSettled;
  QString sourceWarning;
  if (liveSceneUsable && !sourceInLiveScene_) {
    sourceWarning =
        loc("Warn.NoSourceInLive")
            .arg(live);
    // Dire OU elle est. « Introuvable » serait faux dans le cas courant — la
    // source existe, elle est juste ailleurs — et enverrait l'operateur en
    // creer une deuxieme.
    if (!sourceScenes_.isEmpty()) {
      sourceWarning += QStringLiteral(" ") + loc("Warn.SourceElsewhere")
                           .arg(sourceScenes_.join(QStringLiteral(", ")));
    }
  }

  QString warning;
  if (!missing.isEmpty() && sceneListSettled) {
    warning = loc("Warn.ScenesMissing")
                  .arg(missing.join(QStringLiteral(", ")));
  } else {
    // Un seul diagnostic a la fois, et dans cet ordre : sans scene de direct
    // valide, parler de la source qu'elle devrait contenir n'a pas de sens.
    warning = sourceWarning;
  }
  if (warning != sceneMappingWarning_) {
    sceneMappingWarning_ = warning;
    if (!warning.isEmpty()) logEvent(QStringLiteral("warn"), warning);
  }

  reportSourceStateIfChanged(false);
}

void SemirprodDock::reportSourceStateIfChanged(bool force) {
  const QString id = settings_->selectedStreamId();
  const InstanceState s = queryInstanceState();
  const QString activeScene = sceneController_->currentScene();

  // Keep width/height fresh for pushObsState() even when we skip the network
  // send below — they were otherwise never written and obs_state shipped 0×0.
  lastSourceStateWidth_ = s.width;
  lastSourceStateHeight_ = s.height;

  if (id.isEmpty() || !liveClient_->isConnected()) return;

  // Dedupe: this is called on every 200 ms poll tick, but the backend only
  // needs a push when something actually changed — otherwise every connected
  // OBS would fire 5 source_state/s at the bridge for no reason.
  //
  // La resolution est comparee a la DERNIERE ENVOYEE (lastReported*), pas a
  // lastSourceStateWidth_/Height_ : ceux-ci viennent d'etre ecrits juste
  // au-dessus, la comparaison etait donc toujours vraie et un changement de
  // resolution ne partait jamais.
  const bool unchanged = lastSourceStateValid_ &&
                         lastSourceStateReady_ == s.anyReady &&
                         lastReportedSourceWidth_ == s.width &&
                         lastReportedSourceHeight_ == s.height &&
                         lastSourceStateStreamId_ == id &&
                         lastSourceStateActiveScene_ == activeScene;
  if (!force && unchanged) return;

  lastSourceStateValid_ = true;
  lastReportedSourceWidth_ = s.width;
  lastReportedSourceHeight_ = s.height;
  lastSourceStateReady_ = s.anyReady;
  lastSourceStateStreamId_ = id;
  lastSourceStateActiveScene_ = activeScene;
  lastSourceStateSourceScene_ = QString();

  liveClient_->sendSourceState(id, s.anyReady, s.width, s.height, activeScene, "");
}

void SemirprodDock::maybeFlipToLive() {
  if (!settings_->autoSwitchEnabled() || !lastPublishing_) {
    cancelPendingFlipToLive();
    return;
  }
  // L'auto-switch ne doit prendre la main que si l'opérateur est déjà sur la
  // scène Live ou la scène Bug. S'il est sur une scène intro/BRB/outro, on
  // n'interrompt pas son déroulé.
  const QString current = sceneController_->currentScene();
  if (current != settings_->sceneLive() && current != settings_->sceneBug()) {
    cancelPendingFlipToLive();
    return;
  }
  // On ne compte que la source qui est dans la scène live : si une instance
  // existe ailleurs (preview, autre scène), elle n'est pas pertinente — on
  // veut basculer seulement quand le rendu live va effectivement montrer du
  // contenu.
  if (sceneController_->isSemirprodSourceReadyInScene(settings_->sceneLive())) {
    cancelPendingFlipToLive();
    if (sceneController_->currentScene() != settings_->sceneLive()) {
      logEvent(QStringLiteral("success"),
               loc("Journal.StreamRestored")
                   .arg(settings_->sceneLive()));
    }
    sceneController_->switchToScene(settings_->sceneLive());
    return;
  }
  // Publisher OK mais SRT pas encore livré de frames sur la scène live : on
  // attend jusqu'à 30s avant de basculer, sinon l'utilisateur voit l'image
  // fallback "no signal" pendant que ffmpeg_source termine son handshake.
  if (!pendingFlipTimer_->isActive()) {
    pendingFlipDeadlineMs_ = QDateTime::currentMSecsSinceEpoch() + 30000;
    pendingFlipTimer_->start();
    setStatus(QStringLiteral("Diffusion détectée, attente du flux avant bascule..."));
  }
}

void SemirprodDock::cancelPendingFlipToLive() {
  if (pendingFlipTimer_->isActive()) pendingFlipTimer_->stop();
  pendingFlipDeadlineMs_ = 0;
}

void SemirprodDock::maybeFlipToBug() {
  if (!settings_->autoSwitchEnabled()) return;
  const QString bug = settings_->sceneBug();
  if (bug.isEmpty()) return;
  if (!sceneController_->sceneExists(bug)) return;
  // Même garde que maybeFlipToLive : si l'opérateur est sur une scène autre
  // que Live/Bug (intro, BRB, écran de fin...), on ne casse pas son déroulé
  // en basculant brutalement sur Bug.
  const QString current = sceneController_->currentScene();
  if (current != settings_->sceneLive() && current != bug) return;
  if (current != bug) {
    logEvent(QStringLiteral("warn"),
             loc("Journal.StreamLost").arg(bug));
  }
  sceneController_->switchToScene(bug);
}

void SemirprodDock::broadcastToWeb() {
  // Web mirror state payload — same shape consumed by the browser-side
  // Studio UI (resources/ui/index.html). Kept small and self-contained so
  // the mirror can render without holding its own state — every refresh
  // is a full snapshot, no incremental diffs.
  if (!webServer_) return;

  QJsonObject state;
  // Langue de l'interface : celle d'OBS, fixe pour la session. La page choisit
  // ses textes d'apres ce champ ; les textes que le plugin envoie lui-meme
  // (erreurs, raisons) sont deja ecrits dans cette langue (voir Locale.hpp).
  state["locale"] = uiLocaleCode();
  state["connected"] = !accessToken().isEmpty();
  // Poste v2 : « connecté » = enregistré par le serveur, pas seulement socket
  // ouvert. Un poste refusé n'est jamais affiché connecté.
  state["wsConnected"] = liveClient_->isRegistered();
  // L'espace auquel ce poste appartient : c'est ce qui permet de voir d'un
  // coup d'oeil qu'un poste est resté sur l'ancien compte.
  // Le nom pousse par le serveur prime : celui du jeton est fige a l'appairage
  // et ne suit pas un renommage de l'espace.
  state["organizationName"] =
      serverOrgName_.isEmpty() ? tokenStore_->cached().orgName : serverOrgName_;
  state["authFlowRunning"] = authFlow_->isRunning();
  state["pluginVersion"] = QStringLiteral(SEMIRPROD_PLUGIN_VERSION);
  // La page doit savoir qu'elle tourne DANS une instance infogeree : le compte
  // n'y est pas celui d'un poste que le client possede, il est pose par
  // l'appairage de l'instance. Elle masque donc « Deconnecter » — voir le refus
  // symetrique dans WebUiServer, cote plugin.
  state["kiosk"] = edition::isCloud();
  // NOTE: never put the web session token in this payload. It only travels via
  // the WS subprotocol handshake and the dock's "Copier le token web" button.
  // Echoing it into broadcast app data would expose it in DevTools / network
  // captures / any screen-share of the mirror.

  // Account state — surfaced as plain text so the mirror can render the
  // pairing code at the same size as the Qt dock.
  // `accountLabel` / `accountUrl` portent le CODE et l'ADRESSE d'appairage,
  // utiles seulement pendant l'appairage. Ils se vident une fois le compte lié.
  state["accountLabel"] = accountLabel_;
  state["accountUrl"] = accountUrl_;
  state["pairingFailed"] = pairingFailed_;

  // Mise à jour disponible. Vide = rien à signaler ; l'interface n'affiche son
  // bandeau que si `updateVersion` est renseignée. Pas d'URL ici : voir
  // openUpdateDownload().
  state["updateVersion"] = pendingUpdateVersion_;
  state["updateSeverity"] = pendingUpdateSeverity_;
  state["updateNotes"] = pendingUpdateNotes_;
  // Mode securite : la version installee est bloquee, le pilotage est coupe.
  state["pluginBlocked"] = pluginBlocked_;
  state["pluginBlockReason"] = blockReason_ == BlockReason::UpdateRequired
                                   ? QStringLiteral("update_required")
                                   : QStringLiteral("security");
  // Identité du compte lié, elle, valable APRÈS l'appairage. Sans ces deux
  // champs, l'interface ne pouvait pas dire quel compte etait connecté.
  state["accountName"] = accountName_;
  state["accountEmail"] = accountEmail_;
  // Chemin local, jamais l'URL d'origine : l'image est servie par le pont (voir
  // accountAvatarBytes_). L'empreinte casse le cache quand l'avatar change ; la
  // cle `k` (WebUiServer::avatarKey) reserve l'image aux pages authentifiees.
  state["accountAvatar"] = (accountAvatarBytes_.isEmpty() || !webServer_)
      ? QString()
      : QStringLiteral("/avatar?v=%1&k=%2").arg(accountAvatarTag_, webServer_->avatarKey());

  // Stream list — only the fields the mirror actually renders.
  QJsonArray streamArr;
  const QString autoStreamId = settings_->selectedStreamId();
  for (const StreamItem& it : streams_) {
    QJsonObject o;
    o["id"] = it.id;
    o["name"] = it.label;
    o["active"] = (it.id == autoStreamId);
    // Dans combien de scenes ce flux est-il montre. Une source restee en
    // automatique compte pour le flux du poste, celui que l'operateur voit
    // marque « actif ».
    o["scenes"] = sceneController_->sceneCountForStream(it.id, autoStreamId);
    // Connaissance LOCALE : une source punaisee sur ce flux decode des images
    // en ce moment. Un flux qui n'est affiche nulle part rend false sans que
    // rien ne soit en panne — le panneau doit donc l'ecrire ainsi, et ne
    // jamais dire « hors ligne ».
    o["receiving"] = semirprod::isStreamDelivering(it.id.toStdString());
    streamArr.append(o);
  }
  state["streams"] = streamArr;

  // Scene preferences + auto-switch — push them verbatim so a config change
  // done from the mirror stays in sync after a reload.
  state["sceneLive"] = settings_->sceneLive();
  state["sceneBug"] = settings_->sceneBug();
  state["autoSwitch"] = settings_->autoSwitchEnabled();
  state["previewEnabled"] = settings_->previewEnabled();
  state["streaming"] = sceneController_->isStreamingActive();
  state["recording"] = sceneController_->isRecordingActive();
  state["streamingStartedAt"] = streamingStartedAt_;
  state["statsActive"] = webStatsActive_;
  state["statsKbps"] = webStatsKbps_;
  state["statsDroppedFrames"] = webStatsDroppedFrames_;
  state["statsTotalFrames"] = webStatsTotalFrames_;
  state["statsCongestion"] = static_cast<double>(webStatsCongestion_);
  // Nom donne sur le site s'il existe, sinon le nom local.
  state["instanceLabel"] = settings_->displayInstanceLabel();
  state["healthAlert"] = healthAlertActive_;
  state["healthReason"] = healthAlertReason_;
  // Actions distantes sensibles, visibles sur le poste (voir
  // noticeRemoteDestination / refreshRemotePreviewIndicator).
  state["remotePreviewActive"] = remotePreviewIndicated_;
  state["remoteDestinationNotice"] = remoteDestinationNotice_;
  state["connTestRunning"] = connTestRunning_;
  state["connTestApiMs"] = connTestApiMs_;
  state["connTestLiveMs"] = connTestLiveMs_;
  state["connTestError"] = connTestError_;
  // Les 5 dernieres suffisent a l'ecran ; le fichier en garde 20.
  QJsonArray recentSessions;
  for (int i = 0; i < sessionHistory_.size() && i < 5; ++i) {
    recentSessions.append(sessionHistory_.at(i));
  }
  state["sessions"] = recentSessions;

  // Canonical scene list — the mirror's <select> can only render options the
  // dock owns, so ship the full list. Without this the dropdowns stay empty.
  QJsonArray sceneArr;
  for (const QString& name : sceneController_->availableScenes()) sceneArr.append(name);
  state["_scenes"] = sceneArr;

  // Studio entitlement + active banner state.
#ifdef SEMIRPROD_COLLECTIONS
  // Sauvegardes de collections : phase, progression, quota et liste. Un seul
  // objet, pour que l'ecran n'ait pas a recoller des champs eparpilles.
  //
  // Son ABSENCE est le signal qui masque l'onglet dans l'interface : un seul
  // bundle sert donc le binaire qui expose la fonction et celui qui ne l'expose
  // pas. Ne jamais envoyer ici un objet vide « pour la forme » — l'interface le
  // lirait comme une fonction presente et sans aucune sauvegarde.
  state["backups"] = backupService_->toJson();
#endif

  state["studioAccess"] = studioAccess_;
  state["studioAccessKnown"] = studioAccessKnown_;
  state["studioReason"] = studioReason_;

  // Droits par fonction, RESOLUS par le serveur. Absents tant qu'aucune reponse
  // n'est arrivee : l'interface n'en deduit alors rien et n'affiche aucun refus.
  if (!pluginFeaturesJson_.isEmpty()) {
    state["features"] = QJsonDocument::fromJson(pluginFeaturesJson_).object();
  }

  // Source / live status — the mirror reads `sourceReady` + dimensions to
  // show a status pill matching the dock.
  InstanceState s = queryInstanceState();
  state["sourceReady"] = s.anyReady;
  state["sourceWidth"] = s.width;
  state["sourceHeight"] = s.height;

  // Diagnostic de la bascule automatique. `sourceReady` ne suffit pas a le
  // rendre : il vaut false aussi bien quand la source manque que quand elle est
  // la sans flux, et l'operateur n'a aucun moyen de distinguer les deux.
  // `sourceInLiveScene` repond a la seule question qui bloque la bascule, et
  // `sourceScenes` dit ou la source se trouve quand ce n'est pas au bon
  // endroit.
  state["sourceInLiveScene"] = sourceInLiveScene_;
  QJsonArray sourceSceneArr;
  for (const QString& name : sourceScenes_) sourceSceneArr.append(name);
  state["sourceScenes"] = sourceSceneArr;

  webServer_->broadcastState(state);
  emit stateChanged(state);
}

QString SemirprodDock::accessToken() const { return tokenStore_->cached().accessToken; }
int SemirprodDock::selectedStreamIndexById(const QString& id) const {
  for (int i=0; i<streams_.size(); ++i) if (streams_[i].id == id) return i;
  return -1;
}

} // namespace semirprod
