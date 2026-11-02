#pragma once

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QMessageBox>
#include <QPointer>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include <obs-frontend-api.h>

class QJsonDocument;
class QTimer;

class QDockWidget;

namespace semirprod {

class BackupService;
class HttpClient;
class LiveClient;
class DeviceAuthFlow;
class AudioLevels;
class PreviewSceneMix;
class ProgramPreviewOutput;
class SceneController;
class VerticalBridge;
class MultistreamBridge;
class Settings;
class StudioWindow;
class TokenStore;
struct TokenBundle;
class UpdateChecker;
class WebUiServer;
class WebUiWindow;

struct StreamItem {
  QString id;
  QString label;
  QString rtmpUrl;
  QString streamKey;
  QString srtReadUrl;
};

class SemirprodDock : public QWidget {
  Q_OBJECT

public:
  explicit SemirprodDock(QWidget* parent = nullptr);
  ~SemirprodDock() override;

  LiveClient* liveClient() const { return liveClient_; }

  /// Octets de l'avatar servis par WebUiServer sur `/avatar`. Vide si le compte
  /// n'en a pas, ou si son hébergement est hors allowlist.
  const QByteArray& accountAvatarBytes() const { return accountAvatarBytes_; }
  QString accountAvatarType() const { return accountAvatarType_; }

  // Opens the Semirprod web UI (React, served locally by webServer_) in
  // an embedded-browser window. Falls back to showing this native dock if OBS's
  // embedded browser is unavailable. Called by the "Open Semirprod" menu.
  void showWebUi();
  // Windows / Linux : le Studio en fenetre modale. Appelee par showWebUi(),
  // qui reste le point d'entree unique quelle que soit la plateforme.
  void openStudioWindow();
  /** Ferme la fenetre Studio si elle est ouverte. En file d'attente : la
      demande arrive par le serveur local, jamais depuis la pile de CEF. */
  void closeStudioWindow();
  // Enregistre le dock du Studio aupres d'OBS. Appele au demarrage : c'est ce
  // qui le fait apparaitre dans le menu « Docks ».
  void registerStudioDock();
  // Repli du Studio hors CEF : navigateur du systeme + jeton dans le
  // presse-papiers. Appele quand obs-browser manque, et quand le Qt de
  // l'hote est touche par QTBUG-136165 (voir showWebUi).
  void openWebUiInSystemBrowser();
  // Monte le navigateur quand le dock s'affiche, le relache quand il se ferme.
  void onStudioDockVisibilityChanged(bool visible);
  // Appele une fois, sur OBS_FRONTEND_EVENT_FINISHED_LOADING : referme et
  // detache le Studio que la restauration des docks d'OBS vient peut-etre de
  // rouvrir, puis autorise les montages suivants.
  void settleStudioDockAfterStartup();
  // Detruit le Studio et son navigateur, dans l'ordre qui ne plante pas sur
  // macOS. Appelee par le demontage differe et par la reouverture d'un Studio
  // dont la vue native a disparu.
  void unmountStudioNow();

  // Slots for logic control
  void startAuth();
  /// Un jeton vient d'etre depose dans le magasin par un flux EXTERIEUR au dock
  /// (appairage automatique d'une instance infogeree, cf. cloud/PairingFlow).
  /// Relit le magasin et rejoue toute la chaine d'apres-authentification.
  ///
  /// Le dock ne lisait son magasin qu'UNE fois, dans son constructeur, et
  /// n'exposait rien pour le relire. L'appairage etant asynchrone, il arrivait
  /// toujours apres cette lecture : l'instance terminait son premier demarrage
  /// NON AUTHENTIFIEE — pas de liaison live, pas d'identite de compte, pas
  /// d'acces Studio — et ne se connectait qu'au redemarrage suivant du
  /// conteneur. Le client ouvrait son OBS neuf et rien ne marchait.
  ///
  /// A appeler sur le fil principal Qt, comme tous les rappels de TokenStore.
  /// Idempotent : sans effet si la liaison est deja ouverte ou si l'appairage
  /// interactif tourne.
  void onExternalTokenStored();
  void disconnectAccount();
  void stopSessionWork();
  void onPublishState(const QString& id, bool pub, bool synced);
  void armInitialFetchRetry();
  void onPublishRecheck();
  void stopPublishRecheck();
  // Efface la liaison locale (trousseau, client live) sans rien demander au
  // serveur : suite d'un retrait, ou d'un refus definitif du poste.
  void clearLocalBinding();
  void refreshAll();
  void selectStream(const QString& streamId);
  /// Pose une source punaisee sur ce flux dans la scene EN COURS.
  ///
  /// GESTE EXPLICITE DE L'OPERATEUR, comme placeSourceInLiveScene : c'est le
  /// bouton « Ajouter a la scene » de la liste des flux. Le plugin ne cree
  /// jamais de source de lui-meme.
  void placeStreamSourceInCurrentScene(const QString& streamId);
  void setSceneConfig(const QString& live, const QString& bug, bool autoSwitch);
  /// Pose une source Semirprod dans la scene de direct configuree.
  ///
  /// GESTE EXPLICITE DE L'OPERATEUR, jamais automatique : c'est le bouton
  /// « Corriger » de l'interface, pose derriere le diagnostic « aucune source
  /// dans ta scene de direct ». Le plugin ne cree jamais d'objet dans la
  /// collection de quelqu'un sans qu'il l'ait demande.
  void placeSourceInLiveScene();
  void setPreviewEnabled(bool enabled);
  /// Ferme le bandeau « destination changee a distance » (bouton de la page).
  void dismissRemoteDestinationNotice();
  void setInstanceLabel(const QString& label);
  void runConnectionTest();
  /// Empaquette la collection de scenes ACTIVE et la depose sur le compte.
  void backupCurrentCollection();
  /// Empaquette CES collections, l'une apres l'autre. Liste vide = l'active.
  ///
  /// Des NOMS, jamais des chemins, et chaque nom est verifie contre ce qu'OBS
  /// declare (`CollectionPackager::packageCollection`). Accepter un chemin ferait
  /// du pont local un lecteur de disque arbitraire pour quiconque lui parle.
  void backupCollections(const QStringList& collectionNames);
  /// Relit la liste des sauvegardes et le quota depuis le serveur.
  void refreshBackups();
  /// Importe un paquet dans OBS : medias sur le disque, collection dans la liste.
  ///
  /// Un IDENTIFIANT, pas un chemin : les dossiers sont choisis par le plugin.
  /// Laisser la page nommer un fichier de sortie ferait du pont local un
  /// ecrivain de disque arbitraire pour quiconque lui parle.
  void importBackup(const QString& backupId);
  /// Ouvre dans OBS la collection que le dernier import a installee.
  ///
  /// Sans parametre : le nom vient de BackupService, jamais de la page. Refuse
  /// pendant une diffusion ou un enregistrement (SceneController).
  void openImportedCollection();
  /// Ouvre la page d'abonnement dans le navigateur du système.
  ///
  /// Sans paramètre, volontairement : l'URL vient de Config, jamais de la page.
  /// Une commande « ouvre cette URL » ferait du plugin un lanceur de liens
  /// arbitraires pour quiconque parle au pont local.
  void openSubscribePage();
  /// Ouvre le téléchargement de la mise à jour annoncée par le manifeste.
  /// Sans paramètre, même raison que ci-dessus : l'URL vient du manifeste
  /// signé, jamais de la page.
  void openUpdateDownload();
  /// Ouvre la page qui rouvre une fonction fermee (s'abonner, changer de palier,
  /// regulariser) ou la mise a jour. La page ne donne qu'un NOM de fonction :
  /// le chemin vient de la matrice serveur, l'hote de Config.
  void openFeatureAction(const QString& feature);
  void broadcastToWeb();

  /** Consigne un evenement metier (pas une trame WS brute) : demarrage de
      diffusion, bascule de scene, incident reseau... Alimente le dock
      Journal et le log OBS. */
  void logEvent(const QString& level, const QString& message);

signals:
  /** Meme instantane que celui pousse a la WebUI, rediffuse aux docks natifs
      pour qu'ils partagent une seule source de verite. */
  void stateChanged(const QJsonObject& state);

  /** Un evenement a consigner. `level` vaut info, success, warn ou error. */
  void eventLogged(const QDateTime& at, const QString& level,
                   const QString& message);

private:
  void bindSignals();
  void loadToken();
  void resumeWithBundle(const TokenBundle& bundle);
  void refreshStreams();
  void applyStreamsResponse(const QJsonDocument& json);
  /// `stream_deleted` : retire le flux sans relire la liste, et vide la cible
  /// si c'etait celle du poste.
  void onStreamDeleted(const QString& streamId);
  /// `instance_renamed` : le nom donne sur le site devient le nom affiche.
  void onInstanceRenamed(const QString& label);
  void refreshSceneChoices();
  void applySelectedStream();
  void registerSelectedStream();
  void updateUiState();
  // Studio entitlement (abonnement / partenaire / staff). Queried from
  // GET /api/live/access; surfaces a paywall banner and gates the live
  // workflow when the account has no active Studio access (none / expired).
  void checkStudioAccess();
  /// Interroge /api/me, mémorise l'identité et rediffuse l'état.
  void refreshAccountIdentity();
  /// `refetchStreams` faux : l'appelant fournit deja la liste (instantane
  /// d'enregistrement), inutile de relire /api/streams.
  void applyStudioAccess(bool access, bool isPartner, const QString& reason,
                         bool refetchStreams = true);
  /// Interroge /api/obs/plugin/features : droits par fonction, resolus serveur.
  void checkPluginFeatures();
  /// Retient la matrice des fonctions (`data` de /api/obs/plugin/features).
  void applyPluginFeatures(const QJsonObject& data);
  /// Applique l'instantane porte par `register_instance_result` (contrat temps
  /// reel du 02/10). Bloc absent ou `snapshot` faux : chemin HTTP habituel.
  void applyRegistrationResult(const QJsonObject& message);
  /// Arme le filet HTTP : si aucun instantane n'arrive dans le delai, les
  /// routes d'acces et d'identite sont relues comme avant.
  void armInitialFetchFallback();
  /// Arme le renouvellement du jeton a son echeance (`exp - renewBefore`).
  void armTokenRefresh();
  /// Arme la reprise apres un echec de renouvellement de session.
  void armSessionRetry();
  void superviseLiveLink();
  /// Chemin HTTP de demarrage : acces (puis flux et fonctions) et identite.
  void runInitialHttpFetch();
  void updateStudioNotice();
  void setStatus(const QString& message);
  void handleSwitchCommand(
      const QString& requestId,
      const QString& streamId,
      const QString& target,
      const QDateTime& expiresAt);
  void updateSourcePanel();
  void reportSourceStateIfChanged(bool force);
  void maybeFlipToLive();
  void maybeFlipToBug();
  void cancelPendingFlipToLive();
  void pushObsState();
  // Read the streaming-output telemetry, derive per-tick deltas (kbps,
  // droppedDelta) from the previous snapshot, and push an output_stats message.
  // No-op when the WS is down or nothing is streaming (resets the baseline in
  // the latter case so the next go-live starts clean).
  void pushOutputStats();
  void onSwitchSceneRequested(
      const QString& requestId,
      const QString& streamId,
      const QString& target,
      const QDateTime& expiresAt);
  void onControlRequested(const QString& action, const QString& requestId);
  /** Applique la destination poussée par le site, puis réimpose le débit.
      `streamKey` est un secret : il ne sort pas de cette fonction. */
  void onDestinationRequested(const QString& requestId,
                              const QString& rtmpUrl,
                              const QString& streamKey);
  /** Réponse au site pour `set_destination`. Séparée pour qu'aucun chemin de
      sortie n'oublie de répondre : un site sans réponse laisse l'écran du
      client sur « application en cours » jusqu'au délai d'attente. */
  void sendDestinationResult(const QString& requestId,
                             const QString& action,
                             bool ok,
                             const QString& reason);
  /** Destination de la sortie verticale, relayée à Semirprod Vertical. */
  void onVerticalDestinationRequested(const QString& requestId,
                                      const QString& rtmpUrl,
                                      const QString& streamKey,
                                      bool followStream);
  /** Chaines du profil et pilotage, relayes a Semirprod Multistream. */
  void onCreateSceneCollectionRequested(const QString& requestId, const QString& name);
  void onMultistreamDestinationsRequested(const QString& requestId,
                                          const QJsonArray& destinations);
  void onMultistreamControlRequested(const QString& requestId,
                                     const QString& action,
                                     const QString& destinationId);
  void onPreviewStreamStartRequested(const QString& requestId, int fps,
                                     int quality, int width, int ttlMs);
  void onPreviewStreamStopRequested(const QString& requestId);
  // WebRTC program preview (WHIP), OBS 30+. Start/refresh arms the lease timer;
  // stop / lease expiry tears the output down. See ProgramPreviewOutput.
  void onProgramPreviewStartRequested(const QString& requestId,
                                      const QString& whipUrl,
                                      const QString& publishToken, int ttlMs,
                                      int width, int height, int fps,
                                      int bitrateKbps);
  void onProgramPreviewStopRequested(const QString& requestId);
  void stopProgramPreview(bool force = false);
  // Aperçu WebRTC de la scène de PRÉPARATION (mode studio). Même modèle de
  // bail que l'aperçu programme, avec en plus la création/destruction du
  // deuxième mix vidéo : celui-ci n'existe QUE pendant le bail.
  void onStudioPreviewStartRequested(const QString& requestId,
                                     const QString& whipUrl,
                                     const QString& publishToken, int ttlMs,
                                     int width, int height, int fps,
                                     int bitrateKbps);
  void onStudioPreviewStopRequested(const QString& requestId);
  void stopStudioPreview(bool force = false);
  // Aperçu WebRTC du canvas VERTICAL. La sortie appartient à Semirprod
  // Vertical (qui la coupe quand son canvas disparaît) ; le dock ne tient que
  // le bail, comme pour les deux autres aperçus.
  void onVerticalPreviewStartRequested(const QString& requestId,
                                       const QString& whipUrl,
                                       const QString& publishToken, int ttlMs,
                                       int width, int height, int fps,
                                       int bitrateKbps);
  void onVerticalPreviewStopRequested(const QString& requestId);
  void stopVerticalPreview();
  // Aperçu WebRTC d'un canvas OBS NOMMÉ (édition régie). Le dock tient une
  // référence forte au canvas tant que la sortie tourne : son mix vidéo doit
  // survivre à l'encodeur, comme le deuxième mix de l'aperçu de préparation.
  void onCanvasPreviewStartRequested(const QString& requestId,
                                     const QString& canvas,
                                     const QString& whipUrl,
                                     const QString& publishToken, int ttlMs,
                                     int width, int height, int fps,
                                     int bitrateKbps);
  void onCanvasPreviewStopRequested(const QString& requestId);
  void stopCanvasPreview(bool force = false);
  // Signal « remove » du canvas suivi (collection fermée, canvas retiré par
  // canvas-kit) : coupe l'aperçu. Appelé sur le fil qui retire le canvas.
  static void onPreviewCanvasRemovedSignal(void* data, calldata_t* cd);
  // Flux alimenté par ce poste, changé depuis le site.
  void onSetStreamRequested(const QString& requestId, const QString& streamId);
  // Mapping Live/Bug réglé depuis la régie. Mêmes contrôles que la page du
  // plugin : un nom qui ne désigne aucune scène est refusé, pas créé.
  void onSetSceneMappingRequested(const QString& requestId,
                                  const QString& sceneLive,
                                  const QString& sceneBug,
                                  int autoSwitch);
  // Bascules de configuration pilotées par la régie (aucune création).
  void onSetProfileRequested(const QString& requestId, const QString& name);
  void onSetSceneCollectionRequested(const QString& requestId,
                                     const QString& name);
  // Mode studio piloté par la régie.
  void onSetStudioModeRequested(const QString& requestId, bool enabled);
  void onSetPreviewSceneRequested(const QString& requestId,
                                  const QString& scene);
  void onTriggerTransitionRequested(const QString& requestId);
  // Seuillage des stats de sortie -> alerte sante. Ne touche a AUCUN reglage
  // d'OBS : regler le debit du direct est le travail d'OBS, pas le notre.
  void evaluateStreamHealth(int kbps, float congestion, double droppedPct);
  void beginSessionRecord();
  void finalizeSessionRecord();
  void loadSessionHistory();
  void saveSessionHistory();
  static QString sessionHistoryFilePath();
  void warmStopProgramPreview(const char* reason);
  void pushSourcesState();
  void pushAudioState();
  // Vu-mètres : bail ouvert par la régie, volmètres créés à la demande et
  // détruits à l'expiration. Voir AudioLevels.
  void onAudioLevelsStartRequested(const QString& requestId, int fps, int ttlMs);
  void onAudioLevelsStopRequested(const QString& requestId);
  void stopAudioLevels();
  void pushAudioLevels();
  /** Programme un envoi d'audio_state groupé (voir kAudioSyncDelayMs). Sûr à
      appeler en rafale : le minuteur est relancé, pas empilé. */
  void scheduleAudioSync();
  /** Callback des signaux libobs d'activation/désactivation de source. Tourne
      sur le thread qui a émis le signal (graphics/core, PAS Qt) : il ne fait
      que reposter vers le thread principal. */
  static void onCoreSourceActivity(void* data, calldata_t* cd);
  /** Abonne / désabonne les signaux globaux ci-dessus. */
  void connectCoreSignals();
  void disconnectCoreSignals();
  void onSetSceneItemVisible(const QString& requestId,
                             const QString& sceneName,
                             const QString& sourceName,
                             bool visible);
  void onSetSceneItemLocked(const QString& requestId,
                            const QString& sceneName,
                            const QString& sourceName,
                            bool locked);
  void onSetSourceMuted(const QString& requestId,
                        const QString& sourceName,
                        bool muted);
  void onSetSourceVolume(const QString& requestId,
                         const QString& sourceName,
                         float volumeDb);
  void onSetSourceMonitoring(const QString& requestId,
                             const QString& sourceName,
                             int monitoringType);
  void stopPreviewStream();
  void onPreviewTick();
  // Fermeture OBS (OBS_FRONTEND_EVENT_EXIT) : coupe l'output WHIP, tous les
  // timers périodiques et la connexion WS pendant que libobs/Qt sont encore
  // pleinement vivants. Sans ça, le teardown traîne pendant le shutdown
  // (obs_shutdown attend l'output WHIP, le WS tente des reconnexions…) et la
  // fermeture d'OBS rame. Idempotent.
  void prepareShutdown();
  QString accessToken() const;
  int selectedStreamIndexById(const QString& id) const;

  static void obsFrontendEvent(enum obs_frontend_event event, void* data);

  Settings* settings_;
  TokenStore* tokenStore_;
  HttpClient* http_;
  DeviceAuthFlow* authFlow_;
  LiveClient* liveClient_;
  SceneController* sceneController_;
  // Pont vers Semirprod Vertical, s'il est installé (sinon inerte).
  VerticalBridge* verticalBridge_;
  // Pont vers Semirprod Multistream (edition regie), sinon inerte.
  MultistreamBridge* multistreamBridge_;
  // Native WHIP output pushing the program mix for the WebRTC dashboard
  // preview. Only meaningfully used when ProgramPreviewOutput::isSupported()
  // (OBS 30+); otherwise start requests never arrive (capability not
  // advertised).
  ProgramPreviewOutput* programPreview_;
  // Sortie WHIP du deuxième mix (scène en préparation) et le mix lui-même.
  // Tous deux inertes tant que la régie n'a pas demandé le double aperçu.
  ProgramPreviewOutput* studioPreview_;
  PreviewSceneMix* studioMix_;
  // Session Caméra sans fil en cours. La source que l'opérateur a ajoutée
  // dans ses scènes s'y abonne d'elle-même, le plugin n'injecte rien dans la
  // scène. Une seule session à la fois en MVP.
  QTimer* sceneWatchTimer_;
  // Fusionne les register_stream rapproches — voir registerSelectedStream().
  QTimer* registerCoalesceTimer_ = nullptr;
  // Fusionne les streams_changed rapprochés (plusieurs flux supprimés d'affilée)
  // en une seule relecture de /api/streams.
  QTimer* streamsRefreshTimer_ = nullptr;
  // Faux si listen() a echoue (port occupe) : on refuse alors d ouvrir la
  // fenetre Studio plutot que d afficher le contenu d un autre processus.
  bool webServerReady_ = false;
  WebUiServer* webServer_;
  UpdateChecker* updateChecker_;
  // Sauvegardes de collections : empaquetage, reservation de quota, depot.
  // Vit ici parce que le dock porte deja le jeton du compte et l'identite du
  // poste, les deux seules choses dont le service a besoin en plus d'OBS.
  BackupService* backupService_;
  // Lazily created the first time the Studio menu is opened. Parented to the OBS
  // main window, so Qt destroys it at shutdown — never touch it in module unload.
  WebUiWindow* webUiWindow_ = nullptr;
  // Le Studio en fenetre modale (Windows/Linux). Le pointeur ne sert qu'a deux
  // choses : refuser une seconde ouverture, et faire sortir la fenetre de sa
  // boucle si OBS s'arrete pendant qu'elle est ouverte. Il designe un objet de
  // PILE, vivant le temps de openStudioWindow() et remis a nul par son garde de
  // sortie — jamais quelque chose a detruire d'ici.
  StudioWindow* studioWindow_ = nullptr;
  bool studioWindowOpen_ = false;
  // Vrai pendant la construction de WebUiWindow. Ce constructeur fait tourner
  // une boucle d'evenements imbriquee en attendant CEF, qui redelivre les
  // evenements en attente — dont un second visibilityChanged. `webUiWindow_`
  // n'est affecte qu'APRES le constructeur, donc lui seul ne garde rien : sans
  // ce drapeau, deux WebUiWindow se construisent en reentrance et le second
  // CefBrowserHost::CreateBrowserSync fait tomber libcef sur un CHECK.
  bool studioMounting_ = false;
  // Faux jusqu'a ce qu'OBS ait fini de charger (OBS_FRONTEND_EVENT_FINISHED_LOADING).
  //
  // Sert a ignorer le visibilityChanged que produit la RESTAURATION des docks
  // par OBS, et lui seul. Sans ce drapeau, le Studio se montait au demarrage
  // sans que personne ne l'ait demande : OBS relit son `DockState` (un
  // QMainWindow::restoreState) APRES le chargement des modules, donc apres le
  // hide() de registerStudioDock(), et rouvre le dock tel qu'il etait a la
  // fermeture precedente — ancre a droite le cas echeant.
  //
  // Consequence concrete de l'ignorer : le navigateur CEF ne demarre meme pas
  // pendant cette restauration. On ne se contente donc pas de refermer une
  // fenetre ouverte a tort, on evite de payer son ouverture.
  bool frontendReady_ = false;
  // Vrai le temps de fermer le navigateur du Studio. closeBrowser() fait
  // tourner une boucle d'evenements imbriquee : sans ce drapeau, les signaux
  // de visibilite delivres pendant cette boucle relanceraient un montage sur
  // un navigateur en train de mourir.
  bool studioTearingDown_ = false;
  // Une ouverture du Studio a ete demandee PENDANT sa fermeture, et attend que
  // celle-ci soit finie. Rouvrir sur-le-champ tuait OBS (le clic arrive dans la
  // boucle d'evenements imbriquee de closeBrowser, cf. showWebUi) ; ignorer le
  // clic laissait un menu apparemment mort. On le retient donc, et
  // unmountStudioNow() le rejoue une fois le navigateur ferme.
  bool studioReopenPending_ = false;
  // Demontage differe du Studio (Windows/Linux uniquement, cf.
  // mac/StudioDockPolicy.hpp : kDestroyBrowserOnClose). Monter le navigateur
  // provoque un masquage/reaffichage du dock ; sans ce delai, on detruisait et
  // reconstruisait CEF en boucle jusqu'au plantage. Annule si le dock revient.
  // Reste nul sur macOS, ou le navigateur n'est jamais detruit a chaud.
  QTimer* studioTeardownTimer_ = nullptr;
  // Dock du Studio. Enregistre une fois aupres d'OBS, qui en prend la
  // propriete : on ne le detruit jamais nous-memes.
  QDockWidget* studioDock_ = nullptr;
  // macOS : les Studios mis de cote. Leur navigateur est encore VIVANT — le
  // fermer a chaud fait tomber OBS (voir mac/StudioDockPolicy.hpp) — mais il
  // doit imperativement l'etre avant qu'OBS ne decharge obs-browser, sinon
  // CEF meurt en plein `obs_module_unload` et OBS « plante en quittant ».
  // Vide sur les autres plateformes, ou le Studio est detruit normalement.
  QVector<WebUiWindow*> abandonedStudios_;

  QVector<StreamItem> streams_;

  /// Dernier message d'état posé par setStatus().
  ///
  /// C'était un QLabel du dock. Le dock n'affiche plus rien — l'interface est
  /// celle de `webui/` — donc la valeur est conservée comme DONNÉE. Elle n'est
  /// pas encore diffusée : l'interface web n'a pas de ligne d'état où la
  /// mettre. Le jour où elle en aura une, il suffira d'ajouter le champ dans
  /// broadcastToWeb().
  QString statusMessage_;
  /// Identité du compte connecté, lue depuis /api/me.
  ///
  /// Stockée comme DONNÉE et non comme widget : l'interface visible est celle
  /// de `webui/`, le dock Qt ne sert plus que de détenteur d'état et construit
  /// l'instantané JSON diffusé. Un QLabel de plus ici ne s'afficherait nulle
  /// part.
  QString accountName_;
  QString accountEmail_;
  /// Identifiant du compte, lu depuis /api/me. Sert de graine au dégradé par
  /// défaut : c'est la seule donnée stable du compte (le nom et l'adresse
  /// changent, la couleur ne doit pas changer avec eux).
  QString accountId_;
  /// Avatar du compte, rapatrié dans le plugin puis servi par WebUiServer sur
  /// `/avatar`.
  ///
  /// Pourquoi ne pas donner l'URL à la page : la CSP qu'on sert interdit les
  /// images distantes (`img-src 'self' data: blob:`), et l'assouplir ferait du
  /// dock un client qui va chercher une ressource chez un tiers — lequel
  /// apprendrait l'adresse IP du streamer et l'heure de chaque ouverture.
  /// Pourquoi pas non plus en base64 dans l'instantané : celui-ci repart
  /// toutes les 2 s pendant un direct (pushOutputStats), l'image y serait
  /// retransmise en boucle.
  QString accountAvatarSourceUrl_;  // URL RÉSOLUE, pour ne pas re-télécharger
  QByteArray accountAvatarBytes_;
  QString accountAvatarType_;
  QString accountAvatarTag_;  // empreinte courte, casse le cache de la page
  /// `rawUrl` vient de `/api/me` : c'est un chemin relatif
  /// (`/api/avatar/<userId>?v=…`), résolu contre l'API avant tout contrôle.
  /// Tout échec (champ vide, hôte refusé, réponse illisible) retombe sur
  /// fetchDefaultAvatar() : la page ne décide jamais d'un repli elle-même.
  void fetchAccountAvatar(const QString& rawUrl);
  /// Rapatrie le dégradé par défaut du compte, rendu par le site sur
  /// `/avatars/<id>`.
  ///
  /// L'interface web dessinait ce repli elle-même, avec un dégradé neutre sans
  /// rapport avec celui du site : le même compte apparaissait donc sous deux
  /// couleurs selon l'écran, ce qui défait ce à quoi sert cette couleur.
  /// Le dégradé est choisi à un seul endroit, côté site, et arrive ici comme
  /// une image parmi d'autres — rien à porter, rien à garder synchronisé.
  void fetchDefaultAvatar();
  // Mise à jour disponible, telle que publiée par le manifeste. Diffusée à
  // l'interface web (`updateVersion` / `updateSeverity` / `updateNotes`) qui en
  // fait un bandeau. L'URL reste ici : la page demande `open_update`, elle ne
  // choisit jamais la destination.
  QString pendingUpdateUrl_;
  QString pendingUpdateVersion_;
  QString pendingUpdateSeverity_;
  QString pendingUpdateNotes_;
  // Manifeste signe : plus rien de plus recent. Retire l'offre (popup et bandeau)
  // sans attendre un redemarrage d'OBS.
  void clearPendingUpdate();
  // Popup native de mise a jour, sur la fenetre d'OBS : visible meme quand le
  // Studio n'est pas ouvert. « Me rappeler plus tard » = pas avant 24 h
  // (Settings::snoozeUpdate) ; « Ne plus me rappeler » = cette version, dans
  // Settings. Une mise a jour critique passe par enterPluginBlocked. Attend la fin du chargement d'OBS, et la
  // fin d'un direct ou d'un enregistrement.
  bool obsLoaded_ = false;
  // Capture `get_preview` en cours : une seule a la fois, les suivantes sont
  // refusees jusqu'au rappel de la premiere.
  bool previewCaptureInFlight_ = false;
  QString updatePromptedVersion_;
  QPointer<QMessageBox> updatePrompt_;
  void maybePromptUpdate();
  // Pause : version bloquee par le serveur ou mise a jour critique. La liaison
  // est coupee (LiveClient::halt), une popup propose la mise a jour avec « Plus
  // tard » pour seul autre choix, et revient au prochain lancement.
  bool pluginBlocked_ = false;
  // Pourquoi, et donc qui peut lever la pause. Security et UpdateRequired sont
  // des refus du SERVEUR : un manifeste n'a pas a les contredire, le serveur
  // refuserait de nouveau a la reconnexion. ManifestCritical vient du manifeste
  // signe, et un manifeste signe peut la lever (maybeLiftUpdatePause).
  enum class BlockReason { Security, UpdateRequired, ManifestCritical };
  BlockReason blockReason_ = BlockReason::Security;
  bool blockedPromptShown_ = false;
  QPointer<QMessageBox> blockedPrompt_;
  // Pause venue du manifeste, que le dernier manifeste signe ne justifie plus.
  // Lue dans l'etat de l'offre (seul un manifeste verifie l'ecrit) : un retour en
  // critique pendant l'attente annule donc la levee sans rien avoir a defaire.
  bool updatePauseNoLongerJustified() const;
  // Rend le pilotage sans redemarrer OBS. Jamais pendant un direct ou un
  // enregistrement : reprise a l'arret, comme l'entree en pause.
  void maybeLiftUpdatePause();
  // Acces au direct refuse : UNE popup par session d'OBS, jamais a chaque
  // verification (le plugin 0.1.5 en ouvrait une a chaque reconnexion).
  bool accessDeniedPromptShown_ = false;
  bool lastAccessDeniedLogged_ = false;
  void maybePromptAccessDenied();
  void enterPluginBlocked(BlockReason reason = BlockReason::Security);
  void showBlockedPrompt();
  // Le texte de la popup de pause suit l'etat courant. Pause devenue sans objet :
  // une popup ouverte se retire, sans en rouvrir une par dessus un direct.
  void applyBlockedPromptText(QMessageBox* box) const;
  void refreshBlockedPrompt();
  // Studio paywall — GET /api/live/access dit si le compte peut passer en
  // direct (abonnement absent / expiré). L'interface web le rend depuis
  // studioAccess / studioAccessKnown / studioReason ; le dock ne porte plus
  // que l'état, plus le bandeau.
  //
  // Tri-state: unknown until the first /api/live/access reply lands. We
  // fail-open (treat as allowed) while unknown so a flaky endpoint never
  // blocks an otherwise-entitled streamer.
  bool studioAccessKnown_ = false;
  bool studioAccess_ = true;
  QString studioReason_;
  bool studioCheckInFlight_ = false;
  // Anti-martelage de /api/live/access. Le declencheur normal est un push
  // serveur (`check_studio_access`), mais un retry de webhook Stripe en emet
  // plusieurs d'affilee, et rien n'empeche un client local ayant le token du
  // pont WS d'en demander en boucle. Une verification au plus toutes les
  // kStudioCheckMinIntervalMs ; les demandes de la fenetre fusionnent en UNE
  // verification differee (jamais une file : rejouer la rafale plus tard ne
  // ferait que la deplacer).
  qint64 studioCheckLastMs_ = 0;
  bool studioCheckQueued_ = false;
  // Matrice des fonctions (`data` de /api/obs/plugin/features), gardee telle que
  // le serveur l'a resolue et rediffusee a l'interface sous `features`. Le
  // plugin n'en tire AUCUNE decision de droit : il grise, la route refuse.
  // Vide tant qu'aucune reponse n'est arrivee.
  QByteArray pluginFeaturesJson_;
  bool pluginFeaturesInFlight_ = false;
  // Filet du demarrage et des reconnexions : un coup, annule par l'instantane
  // de `register_instance_result`. Remplace le double appel « loadToken puis
  // connexion WS » a moins de 10 s d'intervalle.
  QTimer* initialFetchTimer_ = nullptr;
  // Rien n'a encore été lu dans cette session : sans ces essais, un OBS lancé
  // pendant une panne restait sans flux une fois le serveur revenu.
  QTimer* initialFetchRetryTimer_ = nullptr;
  int initialFetchRetryMs_ = 5000;
  bool initialDataLoaded_ = false;
  // Nom de l'espace pousse par le serveur (instantane, `org_renamed`). Prime
  // sur celui du jeton, fige a l'appairage.
  QString serverOrgName_;

  // Zone d'appairage, ex-QLabel : le CODE puis l'ADRESSE à ouvrir. Diffusés
  // tels quels par broadcastToWeb() dans `accountLabel` / `accountUrl`, que
  // l'interface web affiche. Ce sont les seules valeurs de l'ancien arbre de
  // widgets qui servaient de stockage : elles doivent rester des champs, pas
  // redevenir de l'affichage.
  QString accountLabel_;
  QString accountUrl_;
  // Le dernier appairage a echoue : accountLabel_ porte alors l'erreur. Envoye
  // a l'interface sous `pairingFailed`, pour qu'elle ne devine pas d'apres un
  // texte qui change avec la langue.
  bool pairingFailed_ = false;
  // Levé entre SCENE_COLLECTION_CHANGING et SCENE_COLLECTION_CHANGED. Gèle
  // l'écriture du mapping Live/Bug : pendant la bascule, OBS annonce déjà la
  // nouvelle collection alors que ses scènes n'ont pas fini d'être chargées.
  bool sceneCollectionChanging_ = false;

  bool lastSourceStateValid_ = false;
  bool lastSourceStateReady_ = false;
  int lastSourceStateWidth_ = 0;
  int lastSourceStateHeight_ = 0;
  // Resolution portee par le dernier source_state ENVOYE (base du dedoublonnage).
  int lastReportedSourceWidth_ = 0;
  int lastReportedSourceHeight_ = 0;
  QString lastSourceStateStreamId_;
  QString lastSourceStateActiveScene_;
  QString lastSourceStateSourceScene_;
  bool lastPublishing_ = false;
  // La scène Bug vient d'une dégradation : le retour à la normale peut avoir eu
  // lieu pendant une coupure de la liaison, et cet événement est alors perdu.
  bool flippedToBugForQuality_ = false;
  // « Plus de publication » relu au retour de la liaison, source encore vivante :
  // on surveille la source au lieu de croire un état peut-être périmé.
  QTimer* publishRecheckTimer_ = nullptr;
  QString publishRecheckStreamId_;
  // Latches the "incoming feed above 1080p" operator notice so the 200 ms
  // source-panel poll surfaces it once, not every tick. Cleared when the feed
  // is no longer oversize so a later oversize stream notifies again.
  bool ingestOversizeNotified_ = false;
  // Dernier avertissement « scène Live/Bug absente de la collection » consigné.
  // Latch : updateSourcePanel() tourne toutes les 200 ms, on ne consigne qu'au
  // changement de diagnostic (vide = mapping sain).
  QString sceneMappingWarning_;
  // Presence de la source dans la scene de direct, et scenes qui en tiennent
  // une. Calcules par updateSourcePanel() (toutes les 200 ms) et publies a
  // l'interface : elle rend l'alerte et le bouton de correction sans refaire
  // le parcours des scenes de son cote.
  bool sourceInLiveScene_ = false;
  QStringList sourceScenes_;
  QTimer* pendingFlipTimer_;
  qint64 pendingFlipDeadlineMs_ = 0;
  QTimer* heartbeatTimer_ = nullptr;
  // Streaming-output telemetry, pushed every 2 s while live (faster than the
  // 5 s heartbeat because bitrate/drops are what an operator watches in real
  // time). Baseline of the previous cumulative counters used to derive kbps
  // and the dropped-frames delta; invalidated whenever streaming isn't active.
  QTimer* outputStatsTimer_ = nullptr;
  // Dernieres stats de sortie, rejouees vers la WebUI a chaque tick (2 s).
  bool webStatsActive_ = false;
  int webStatsKbps_ = 0;
  int webStatsDroppedFrames_ = 0;
  int webStatsTotalFrames_ = 0;
  float webStatsCongestion_ = 0.0f;
  // --- Alertes sante -------------------------------------------------------
  // Seuils franchis de maniere SOUTENUE (pas un pic d'une mesure) : on latch
  // une alerte que la WebUI affiche en bandeau. La reprise demande une
  // periode calme plus longue que le declenchement pour eviter le clignotement.
  bool healthAlertActive_ = false;
  QString healthAlertReason_;
  qint64 healthBadSinceMs_ = 0;
  qint64 healthGoodSinceMs_ = 0;

  // --- Test de connexion ---------------------------------------------------
  bool connTestRunning_ = false;
  // Horodatage du dernier tir : fenetre minimale entre deux tests (chacun
  // envoie deux requetes reelles vers l'infra).
  qint64 connTestLastMs_ = 0;
  int connTestApiMs_ = -1;
  int connTestLiveMs_ = -1;
  QString connTestError_;
  int connTestPending_ = 0;

  // --- Session en cours ----------------------------------------------------
  // Accumulateurs remis a zero a chaque demarrage de diffusion, consolides en
  // une entree d'historique a l'arret.
  qint64 sessionStartedMs_ = 0;
  qint64 sessionKbpsSum_ = 0;
  int sessionKbpsSamples_ = 0;
  int sessionKbpsPeak_ = 0;
  int sessionDroppedFrames_ = 0;
  int sessionTotalFrames_ = 0;
  int sessionIncidents_ = 0;
  QJsonArray sessionHistory_;

  bool outputStatsBaselineValid_ = false;
  quint64 lastOutputBytes_ = 0;
  qint64 lastOutputStatsMs_ = 0;
  int lastOutputDroppedFrames_ = 0;
  // Reprise apres un ECHEC de renouvellement de session (reseau coupe au
  // lancement d'OBS, serveur d'auth injoignable). Un coup, arme par
  // deviceFlowFailed, delai double a chaque echec (30 s -> 5 min), remis a zero
  // au premier succes. Remplace l'ancien chien de garde de 5 min, qui tournait
  // en permanence.
  QTimer* sessionRetryTimer_ = nullptr;
  int sessionRetryDelayMs_ = 30000;

  // Filet de la liaison live (`superviseLiveLink`) : toutes les 30 s, n'agit
  // que si le compte est lié, la liaison coupée et aucune reprise en cours.
  QTimer* liveSupervisorTimer_ = nullptr;
  int liveStuckTicks_ = 0;

  // Continuous preview-frame stream — driven by start_preview/stop_preview
  // commands from the dashboard. The timer tickles takeScreenshot at the
  // configured FPS; previewExpiresAtMs_ implements a leash so a closed
  // dashboard tab doesn't leave OBS encoding JPGs forever.
  QTimer* previewTimer_ = nullptr;
  int previewQuality_ = 75;
  int previewWidth_ = 1280;
  qint64 previewExpiresAtMs_ = 0;
  // Number of captures currently being processed. Capped at 2 so the 2-3-tick
  // capture latency pipelines instead of halving throughput (one bool would
  // gate the pipeline to a single in-flight job → ~15 fps ceiling).
  int previewInFlightCount_ = 0;
  // Monotonic tags for ordered delivery: the JPEG encode runs on a thread pool,
  // so captures can finish out of order. Each job gets the next seq; only a seq
  // newer than the last one sent is forwarded, older completions are dropped.
  quint64 previewSeqNext_ = 0;
  quint64 previewSeqLastSent_ = 0;

  // WebRTC program-preview lease. Single-shot: armed to ttlMs on every
  // start/refresh; on expiry the WHIP output is stopped. A refresh (same
  // params) re-arms it, so a dashboard tab left open keeps the output alive
  // and a closed tab releases CPU/uplink after one TTL — same leash model as
  // the screenshot preview above, but for the WHIP output.
  QTimer* programPreviewLeaseTimer_ = nullptr;

  // Keep-warm : a l'expiration du lease (ou sur stop explicite du viewer),
  // l'output WHIP reste vivant pendant une periode de grace au lieu d'etre
  // detruit. Une reouverture de l'apercu dans la fenetre raccroche en
  // quelques centaines de ms (pas de rebuild encodeurs ni de re-handshake
  // WHIP). Ce timer arme le vrai teardown.
  QTimer* programPreviewWarmTimer_ = nullptr;

  // Bail de l'aperçu de préparation. Pas de keep-warm ici : contrairement à
  // l'aperçu programme, laisser tourner garde EN PLUS un deuxième mix vidéo
  // composé en continu — on rend la ressource dès l'expiration.
  QTimer* studioPreviewLeaseTimer_ = nullptr;
  // Bail de l'aperçu vertical. Pas de keep-warm : l'encodage se fait chez
  // Vertical, on le rend dès l'expiration.
  QTimer* verticalPreviewLeaseTimer_ = nullptr;
  bool verticalPreviewActive_ = false;
  // Aperçu d'un canvas nommé : sortie WHIP (mix externe), canvas tenu en
  // référence forte pendant l'aperçu, son nom, et le bail. Pas de keep-warm.
  ProgramPreviewOutput* canvasPreview_ = nullptr;
  obs_canvas_t* previewCanvas_ = nullptr;
  QString previewCanvasName_;
  QTimer* canvasPreviewLeaseTimer_ = nullptr;

  // Regroupement des envois d'audio_state déclenchés par les signaux libobs.
  // Une transition émet une rafale d'activations/désactivations (une par
  // source des deux scènes) : sans regroupement, chaque bascule enverrait une
  // dizaine d'instantanés identiques en quelques millisecondes.
  QTimer* audioSyncTimer_ = nullptr;
  // Vu-mètres. `audioLevelsTimer_` cadence l'envoi, `audioLevelsLeaseTimer_`
  // coupe tout sans rafraîchissement — même modèle de bail que l'aperçu.
  AudioLevels* audioLevels_;
  QTimer* audioLevelsTimer_ = nullptr;
  QTimer* audioLevelsLeaseTimer_ = nullptr;
  bool coreSignalsConnected_ = false;

  // Renouvellement du JWT : UN coup, arme a `exp - renewBefore` et rearme a
  // chaque changement de jeton (armTokenRefresh). Plus de verification toutes
  // les 30 s.
  QTimer* tokenRefreshTimer_ = nullptr;

  // Latched by prepareShutdown() — OBS is exiting, don't start anything new.
  bool shuttingDown_ = false;

  // --- Avis locaux des actions distantes sensibles -------------------------
  //
  // Un compte d'espace detourne pouvait rediriger le direct (set_destination)
  // ou regarder le programme (apercus) sans que rien ne se voie sur le poste.

  /** Une commande distante vient de poser une destination de diffusion :
      ligne dans le Journal, et bandeau dans l'interface jusqu'a ce que le
      streamer le ferme. `hosts` : hotes vises, JAMAIS la cle. */
  void noticeRemoteDestination(const char* journalKey, const QStringList& hosts,
                               bool hostChanged);
  QString remoteDestinationNotice_;

  /** Vrai tant qu'une sortie d'apercu envoie le programme a la regie, ou
      qu'une capture `get_preview` est partie il y a moins de 30 s. */
  bool remotePreviewActive() const;
  /** Recalcule l'indicateur ; au changement : Journal + etat de la page. */
  void refreshRemotePreviewIndicator();
  void markRemoteCapture();
  bool remotePreviewIndicated_ = false;
  qint64 lastRemoteCaptureMs_ = 0;
  QTimer* remoteCaptureTimer_ = nullptr;

  // ISO-8601 UTC timestamps recorded when OBS reports STREAMING_STARTED /
  // RECORDING_STARTED. Empty when the corresponding output is idle. Pushed
  // verbatim to the dashboard via obs_state so the UI can derive a live
  // elapsed counter without a per-second push.
  QString streamingStartedAt_;
  QString recordingStartedAt_;
};

} // namespace semirprod
