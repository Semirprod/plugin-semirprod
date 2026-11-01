#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

class QJsonObject;
class QSslCertificate;
class QSslError;
class QTimer;

namespace semirprod {

class Settings;
class WebSocketClient;

/** Instantané complet de l'état OBS poussé au backend (`obs_state`).

    C'était douze paramètres positionnels ; l'ajout du mode studio, de la scène
    de préparation, des profils et des collections en aurait fait dix-huit,
    tous du même type ou presque — une inversion d'arguments y serait passée
    inaperçue à la compilation. Une structure nommée est le seul appelant
    possible, donc autant qu'elle porte les noms. */
struct ObsStateReport {
  QString streamId;
  QString activeScene;
  QStringList scenes;
  QString sceneLive;
  QString sceneBug;
  bool streaming = false;
  bool recording = false;
  bool sourceReady = false;
  int width = 0;
  int height = 0;
  // Vides quand la sortie correspondante est à l'arrêt (sérialisés en null).
  QString streamingStartedAt;
  QString recordingStartedAt;
  // Mode studio d'OBS (preview/program). `previewScene` est la scène en
  // préparation — vide hors mode studio.
  bool studioMode = false;
  QString previewScene;
  // Profils et collections de scènes disponibles sur ce poste, avec le courant.
  // La régie n'en propose que la BASCULE : rien ici ne permet d'en créer.
  QStringList profiles;
  QString currentProfile;
  QStringList sceneCollections;
  QString currentSceneCollection;
  // Bascule automatique Live/Bug sur perte d'ingest. Réglée dans le plugin ou
  // depuis la régie ; remontée ici pour que les deux interfaces montrent le
  // même état.
  bool autoSwitch = false;
  // Semirprod Vertical (second canvas 9:16), piloté via VerticalBridge.
  // `verticalAvailable` faux = plugin absent : le site masque la section.
  // Jamais la clé ; `verticalServer` seul identifie la destination.
  bool verticalAvailable = false;
  bool verticalStreaming = false;
  bool verticalConfigured = false;
  bool verticalFollowStream = false;
  QString verticalServer;
  QString verticalError;
  // Semirprod Multistream (edition regie de l'OBS cloud), via
  // MultistreamBridge : { canvases, destinations }, jamais de cle. Null =
  // plugin absent, le site n'affiche pas la regie.
  QJsonValue multistream = QJsonValue(QJsonValue::Null);
};

class LiveClient : public QObject {
  Q_OBJECT

public:
  explicit LiveClient(Settings* settings, QObject* parent = nullptr);

  /**
   * Ouvre la liaison avec ce JWT. Si la liaison est DÉJÀ ouverte, le jeton est
   * renouvelé sur place (`auth_renew`) : un socket sain n'est jamais fermé par
   * le plugin (invariant 1 du plan poste v2).
   */
  void connectWithToken(const QString& token);
  /** Identique à connectWithToken, nommé pour les appels de renouvellement. */
  void renewToken(const QString& token) { connectWithToken(token); }
  void disconnectClient();
  /**
   * Arret ferme : coupe la liaison et refuse toute reconnexion jusqu'a la fin
   * du processus, renouvellement de jeton compris. Version bloquee ou mise a
   * jour obligatoire : seule une nouvelle version relance la liaison.
   */
  void halt() {
    halted_ = true;
    disconnectClient();
  }
  bool isHalted() const { return halted_; }
  /** Appareil v2 : l'identité du poste, tirée par le serveur à l'appairage. */
  void setDeviceId(const QString& deviceId) { deviceId_ = deviceId; }
  /**
   * Flux visé par ce poste. Changé sur le socket ouvert (`set_target`), jamais
   * en fermant la liaison. Vide = poste sans cible, état normal.
   * `sceneLive`/`sceneBug` ne sont plus envoyés : le serveur ne les lisait pas.
   */
  void registerStream(const QString& streamId, const QString& sceneLive, const QString& sceneBug);
  /** Liaison ouverte ET poste enregistré par le serveur. */
  bool isRegistered() const { return connected_ && registered_; }
  void sendSwitchResult(
      const QString& requestId,
      const QString& streamId,
      const QString& target,
      const QString& status,
      const QString& reason,
      const QString& activeScene,
      const QString& appliedScene,
      qint64 latencyMs);

  /** Notify the backend of whether OBS is currently receiving frames for the
      given stream. Width/height carry the last observed dimensions (0/0 when
      not receiving). No-op when the socket isn't connected. */
  void sendSourceState(
      const QString& streamId,
      bool ready,
      int width,
      int height,
      const QString& activeScene,
      const QString& sourceScene);

  /** Push a full snapshot of the OBS instance state (active scene, scene list,
      streaming/recording flags, ingest readiness). Sent on relevant frontend
      events and on a low-frequency timer. The dashboard caches the snapshot
      with a TTL and uses it to render the Stream Deck. */
  void sendObsState(const ObsStateReport& report);

  /** Push streaming-output telemetry (live bitrate, congestion, dropped/total
      frames plus a per-tick dropped delta, and encoder-lag frame counts). Sent
      every 2 s by the dock while streaming is active and the socket is up. The
      dashboard renders a live health readout from this. No-op when the socket
      isn't connected. */
  void sendOutputStats(
      const QString& streamId,
      float congestion,
      int kbps,
      int droppedFrames,
      int totalFrames,
      int droppedDelta,
      int renderSkipped,
      int renderTotal);

  /** Acknowledge a control command (start_stream/stop_stream/start_record/
      stop_record). Tells the bridge whether OBS accepted the request. */
  void sendControlResult(
      const QString& requestId,
      const QString& action,
      const QString& status,
      const QString& reason);

  /** Push the per-scene source inventory (items with visible/locked/audio
      flags). The dashboard renders the visibility toggles from this snapshot.
      Sent on FRONTEND scene/source events and by the heartbeat timer. */
  void sendSourcesState(const QString& streamId, const QJsonArray& scenes);

  /** Push the audio mixer inventory (mute, volume dB, monitoring) for every
      audio-capable source. Sent on FRONTEND audio events and on heartbeat. */
  void sendAudioState(const QString& streamId, const QJsonArray& inputs);

  /** Vu-mètres des pistes, en dBFS. Envoyé à cadence fixe tant que le bail
      tient (voir audioLevelsStartRequested) : c'est une mesure, elle n'a de
      sens qu'en continu. Jamais mis en cache côté backend — une valeur de
      niveau vieille d'une seconde ne veut plus rien dire. */
  void sendAudioLevels(const QString& streamId, const QJsonArray& levels);

  /** Acknowledge a source/audio control command — same pattern as
      sendControlResult but for the source/audio family of actions. */
  void sendSourceControlResult(
      const QString& requestId,
      const QString& action,
      const QString& status,
      const QString& reason);

  /** Sends a base64 encoded JPG screenshot of the current program output. */
  void sendPreviewFrame(const QString& streamId, const QString& base64);

  /** Heartbeat with the persistent label so the bridge can keep the org-wide
      instance directory fresh. Sent every few seconds by the dock. */
  void sendInstanceHeartbeat();
  /** Oublie l'empreinte d'un etat (`obs_state`, `sources_state`,
      `audio_state`), ou de tous si `type` est vide : le prochain envoi part
      meme s'il est identique au precedent. Sert aux demandes explicites
      (`request_state`, `get_sources`, `get_audio`). */
  void forgetStateFingerprint(const QString& type = QString());

  bool isConnected() const { return connected_; }
  /** Refus definitif du serveur (poste revoque, version bloquee) : on ne
      reconnecte plus jusqu'au prochain jeton. */
  bool isRefused() const { return refused_; }
  /** Une reprise est-elle en cours : reconnexion programmee, ou socket en train
      de s'ouvrir ? Faux = plus rien ne ramenera la liaison tout seul. */
  bool isReconnecting() const;
  /** Marge de renouvellement du jeton annoncee par `hello` (secondes). */
  int renewBeforeSeconds() const { return renewBeforeS_; }
  QString token() const { return token_; }
  QString registeredStreamId() const { return registeredStreamId_; }

signals:
  /**
   * Le serveur a refusé ce poste pour de bon (`device_revoked`,
   * `identity_changed`, `update_required`). Aucune reconnexion n'est tentée :
   * l'état est terminal jusqu'à une action de la personne.
   */
  void sessionRefused(const QString& reason);
  /**
   * Le jeton doit être renouvelé : fermeture 4001 (filet), ou `auth_renew`
   * refusé. Le dock rouvre une session puis rappelle connectWithToken().
   */
  void tokenRenewalNeeded();
  /** Le poste vient d'être enregistré (ou ne l'est plus). « Connecté » ne
      s'affiche qu'enregistré : un socket ouvert ne dit pas que le serveur a
      accepté le poste (invariant 7 du plan poste v2). */
  void registrationChanged(bool registered);
  /** `register_instance_result` accepte, message entier. Porte l'instantane
      du contrat temps reel quand `snapshot` vaut vrai : `access`, `features`,
      `streams`, `account {name,email}`, `orgName`. */
  void registrationResult(const QJsonObject& message);
  void connectionChanged(bool connected);
  void streamRegistered(const QString& streamId, bool ok);
  void switchSceneRequested(
      const QString& requestId,
      const QString& streamId,
      const QString& target,
      const QDateTime& expiresAt);
  /** Fires when MediaMTX reports the publisher has started or stopped pushing
      frames on this stream. Used by the dock to switch to the Bug scene
      instantly instead of waiting for ffmpeg_source to notice the TCP drop.

      Also fires once per registration, carrying the snapshot the backend puts
      in register_stream_result — that is what tells an OBS started mid-live
      that the stream is already publishing, since the transition event itself
      was emitted long before this socket existed. */
  void publishStateChanged(const QString& streamId, bool publishing);
  /** Fires when the backend quality monitor crosses the bitrate / freeze
      threshold for the registered stream. ok=true means the ingest is healthy
      again; ok=false means the ingest is degraded and the dock should swap to
      the Bug scene without waiting for a full publish drop. */
  void qualityStateChanged(
      const QString& streamId,
      bool ok,
      const QString& reason,
      int bitrateKbps);
  /** Stream-Deck control commands routed from the dashboard via the bridge.
      `action` is one of: "start_stream", "stop_stream", "start_record",
      "stop_record". The dock applies the action and replies with
      sendControlResult(). */
  void controlRequested(const QString& action, const QString& requestId);
  /** Le site pousse la destination de diffusion : URL d'ingest et clé de flux
      du client, déchiffrée juste avant l'envoi et jamais stockée ici.

      `streamKey` est un SECRET. Ne jamais le journaliser, ne jamais le renvoyer
      dans un `control_result`, ne jamais le remonter dans `obs_state` : le
      journal d'une instance est lisible par l'exploitation, et l'état est
      poussé au navigateur. */
  void destinationRequested(const QString& requestId,
                            const QString& rtmpUrl,
                            const QString& streamKey);
  /** Même chose pour la sortie VERTICALE (plugin Semirprod Vertical).
      `followStream` : le vertical démarre et s'arrête avec le direct
      principal. Mêmes règles de secret que destinationRequested. */
  void verticalDestinationRequested(const QString& requestId,
                                    const QString& rtmpUrl,
                                    const QString& streamKey,
                                    bool followStream);
  /** Les chaines du profil pour Semirprod Multistream (edition regie). Chaque
      entree porte sa cle : jamais journalisee. */
  void multistreamDestinationsRequested(const QString& requestId,
                                        const QJsonArray& destinations);
  /** start_multistream / stop_multistream. `destinationId` vide = toutes. */
  void multistreamControlRequested(const QString& requestId,
                                   const QString& action,
                                   const QString& destinationId);
  /** Dashboard asks the plugin to start streaming preview frames at a given
      cadence and quality. The plugin should auto-stop after `ttlMs` if no
      refresh is received — the dashboard re-issues start_preview every few
      seconds while the panel is open so closing the tab automatically
      releases CPU/bandwidth here. */
  void previewStreamStartRequested(const QString& requestId, int fps,
                                   int quality, int width, int ttlMs);
  /** Dashboard explicitly cancelled the preview stream. */
  void previewStreamStopRequested(const QString& requestId);
  /** Dashboard asks the plugin to start (or refresh) the WebRTC program
      preview: push the composited program mix to `whipUrl` via a native WHIP
      output, authenticated with `publishToken`. This is the WebRTC path that
      replaces the screenshot preview on OBS 30+ (capability
      `program_preview_whip`). The dock arms a `ttlMs` lease timer and re-arms
      it on every refresh (same requestId, same params) — the output stops when
      the lease expires without a refresh. Independent of the broadcast output,
      so it works stream-at-rest. */
  void programPreviewStartRequested(const QString& requestId,
                                    const QString& whipUrl,
                                    const QString& publishToken, int ttlMs,
                                    int width, int height, int fps,
                                    int bitrateKbps);
  /** Dashboard explicitly cancelled the WebRTC program preview. */
  void programPreviewStopRequested(const QString& requestId);
  /** Dashboard wants to flip a scene item's visibility / lock. */
  void setSceneItemVisibleRequested(const QString& requestId,
                                    const QString& sceneName,
                                    const QString& sourceName,
                                    bool visible);
  void setSceneItemLockedRequested(const QString& requestId,
                                   const QString& sceneName,
                                   const QString& sourceName,
                                   bool locked);
  /** Dashboard wants to mute / change volume / monitoring on an audio source. */
  void setSourceMutedRequested(const QString& requestId,
                               const QString& sourceName,
                               bool muted);
  void setSourceVolumeRequested(const QString& requestId,
                                const QString& sourceName,
                                float volumeDb);
  void setSourceMonitoringRequested(const QString& requestId,
                                    const QString& sourceName,
                                    int monitoringType);
  /** Bascule de profil / de collection de scènes demandée par la régie. Seule
      la BASCULE circule : il n'existe aucune commande de création, de
      renommage ou de suppression, côté protocole comme côté plugin. */
  void setProfileRequested(const QString& requestId, const QString& name);
  void setSceneCollectionRequested(const QString& requestId, const QString& name);
  /** Cree une collection de scenes (vide) et y bascule. */
  void createSceneCollectionRequested(const QString& requestId, const QString& name);
  /** La régie ouvre (ou rafraîchit) les vu-mètres. Bail borné comme l'aperçu :
      sans rafraîchissement dans ttlMs, le plugin détruit les volmètres. Mesurer
      chaque piste pour un onglet fermé serait du calcul rendu à personne. */
  void audioLevelsStartRequested(const QString& requestId, int fps, int ttlMs);
  void audioLevelsStopRequested(const QString& requestId);
  /** Change le FLUX que ce poste alimente. Le nom du flux n'est pas transmis :
      seul son identifiant l'est, et le plugin ne l'accepte que s'il figure
      dans la liste que l'API lui a servie pour ce compte. */
  void setStreamRequested(const QString& requestId, const QString& streamId);
  /** Réglage à distance du mapping du plugin : quelle scène est la scène Live,
      laquelle est la scène Bug, et si la bascule automatique est armée. Ne
      CRÉE aucune scène — les deux noms doivent désigner des scènes existantes
      (ou être vides, ce qui vaut « non configuré »).

      `autoSwitch` est un TRI-ÉTAT : -1 = laisser le réglage du poste tel
      quel, 0 = désarmer, 1 = armer. Sans lui, un dashboard qui ne veut changer
      que les scènes désarmerait la bascule au passage. */
  void setSceneMappingRequested(const QString& requestId,
                                const QString& sceneLive,
                                const QString& sceneBug,
                                int autoSwitch);
  /** Mode studio d'OBS piloté depuis la régie : activation, choix de la scène
      en préparation, déclenchement de la transition vers l'antenne. */
  void setStudioModeRequested(const QString& requestId, bool enabled);
  void setPreviewSceneRequested(const QString& requestId, const QString& scene);
  void triggerTransitionRequested(const QString& requestId);
  /** Aperçu WebRTC de la scène en PRÉPARATION (deuxième mix). Même modèle de
      bail que start_program_preview : sans refresh dans ttlMs, le plugin coupe
      la sortie et libère le mix. */
  void studioPreviewStartRequested(const QString& requestId,
                                   const QString& whipUrl,
                                   const QString& publishToken, int ttlMs,
                                   int width, int height, int fps,
                                   int bitrateKbps);
  void studioPreviewStopRequested(const QString& requestId);
  /** Aperçu WebRTC du canvas 9:16 de Semirprod Vertical. Même bail que
      start_program_preview ; la sortie vit chez Vertical. */
  void verticalPreviewStartRequested(const QString& requestId,
                                     const QString& whipUrl,
                                     const QString& publishToken, int ttlMs,
                                     int width, int height, int fps,
                                     int bitrateKbps);
  void verticalPreviewStopRequested(const QString& requestId);
  /** Aperçu WebRTC d'un canvas OBS nommé (`canvas`, édition régie). Même
      bail que start_program_preview ; width/height à 0 = taille déduite du
      canvas. */
  void canvasPreviewStartRequested(const QString& requestId,
                                   const QString& canvas,
                                   const QString& whipUrl,
                                   const QString& publishToken, int ttlMs,
                                   int width, int height, int fps,
                                   int bitrateKbps);
  void canvasPreviewStopRequested(const QString& requestId);
  /** Dashboard explicitly asks for a fresh sources_state / audio_state push. */
  void sourcesRefreshRequested();
  void audioRefreshRequested();
  /** Bridge asks for a fresh obs_state snapshot. Dock answers with
      sendObsState() once. */
  void stateRefreshRequested();
  void checkStudioAccessRequested();
  /** La liste des flux de l'espace a changé côté site (création, renommage,
      suppression) : le dock relit /api/streams. Poussé par le serveur, jamais
      sondé. */
  void streamsChanged();
  /** `stream_deleted {streamId}` : le flux n'existe plus, le pont l'a deja
      detache de la liaison. */
  void streamDeleted(const QString& streamId);
  /** `features_changed` : la matrice des fonctions est a relire. */
  void featuresChanged();
  /** `org_renamed {name}`. `name` peut etre vide si le serveur ne le porte pas. */
  void orgRenamed(const QString& name);
  /** `identity_changed` (message, pas fermeture) : nom ou adresse du compte
      modifie, l'identite est a relire. */
  void identityChanged();
  /** `instance_renamed {label}` : nom du poste donne sur le site. Vide = nom
      retire. */
  void instanceRenamed(const QString& label);
  void statusMessage(const QString& message);
  /** The reconnect loop has failed repeatedly while we still hold a token —
      the dominant cause is an expired/rejected bearer that keeps getting
      re-sent verbatim on every attempt. The dock responds by refreshing the
      token (which reopens the socket with a fresh bearer via
      connectWithToken). Emitted once per stall episode, reset on a successful
      connect. Without this the socket would hammer the dead token until the
      dock's 5-minute connectionWatchdog eventually re-ran loadToken(). */
  void reconnectStalled();
  /** direction is "in" or "out". text is the raw JSON payload. */
  void wsTraffic(const QString& direction, const QString& text);

private:
  /** Annonce le poste seul, sans flux. Émis dès la connexion. */
  void sendRegisterInstance();
  void handleTextMessage(const QString& text);
  void sendJson(const QJsonObject& payload);
  /** Envoie un message d'etat seulement si son contenu (hors `ts`) differe du
      dernier envoye pour ce type. */
  void sendStateIfChanged(QJsonObject payload);
  // Type de message d'etat -> JSON compact du dernier envoi (sans `ts`).
  // Vide a chaque enregistrement du poste.
  QHash<QString, QByteArray> stateFingerprints_;
  // Messages jetables écartés depuis le dernier envoi normal (voir sendJson).
  qint64 droppedWhileBacklogged_ = 0;
  /** Re-arm a one-shot timer to reopen the socket. Backoff doubles each cycle
      (1s → 2s → 4s …) and caps at 30s. No-op if a retry is already pending or
      we have no token (i.e. user-initiated disconnect). */
  void scheduleReconnect();
  void attemptReconnect();
  void cancelReconnect();
  /** Force the socket dead (abort + emit disconnected) and queue a reconnect.
      Used by the keepalive watchdog and the connect-timeout watchdog when
      Qt's own disconnected/errorOccurred signals haven't fired. */
  void forceReconnect(const char* reason);
  void startKeepalive();
  void stopKeepalive();
  /** Returns true if the message either has no targetInstanceId (broadcast)
      or its targetInstanceId equals our own. False = ignore the message. */
  bool messageTargetsThisInstance(const QJsonObject& obj) const;

  /** Portier de TOUTE commande distante mutante, applique avant le moindre
      effet de bord. Rend faux quand le message doit etre ignore.

      Deux filtres, pour deux menaces distinctes :

      1. **Rejeu** — deux messages OCTET POUR OCTET identiques a moins de
         kReplayWindowMs d'intervalle. C'est ce qui a fait tomber OBS le
         15 aout : le dashboard a envoye deux fois le meme
         `start_program_preview` (meme requestId, meme createdAt) a 58 ms
         d'ecart. On deduplique sur l'empreinte du message ENTIER plutot que
         sur le seul `requestId` : ca ne suppose rien de la facon dont le
         backend numerote ses messages, et un vrai rafraichissement de bail
         differe forcement (jeton et horodatage neufs).

      2. **Debit** — un emetteur peut contourner le filtre de rejeu en variant
         un octet a chaque envoi. Sans plafond, mille demarrages d'apercu par
         seconde restent une facon de faire tomber le plugin a distance. Deux
         seaux glissants : un par type de commande, un global.

      Le plugin ne fait confiance a aucun emetteur — ni au backend, ni a ce qui
      parle au pont local. */
  bool acceptRemoteCommand(const QString& type, const QString& rawText);
  QString instanceId() const;
  QString instanceLabel() const;

  /** Inspect every error in `errors` and decide whether the cert chain can
      be trusted. Returns the subset that the socket should silence (i.e. the
      ones corresponding to our pinned leaf) — everything else stays loud so
      Qt aborts the handshake. In release builds, the empty list is the only
      acceptable outcome. */
  QList<QSslError> evaluateSslErrors(const QList<QSslError>& errors);

  /** Compute the SHA-256 of the SubjectPublicKeyInfo (DER) of the chain's
      leaf certificate, base64-encoded. Used for the pin comparison. */
  static QString spkiPinOfChain(const QList<QSslCertificate>& chain);

  Settings* settings_ = nullptr;
  WebSocketClient* socket_;
  QTimer* reconnectTimer_ = nullptr;
  // Sends ws-level pings while connected so a silently-dead TCP (NAT / LB
  // idle drop) is detected instead of sitting half-open until the next OS
  // keepalive (which on Windows defaults to ~2h).
  QTimer* pingTimer_ = nullptr;
  // Fires once when no pong came back in time. The disconnected signal isn't
  // reliable on a half-open TCP, so this is what actually triggers the
  // reconnect cycle in that case.
  QTimer* pongWatchdog_ = nullptr;
  // Guards socket_->open(): if the handshake hasn't reached the connected
  // state in N seconds we give up and retry — open() can otherwise hang in
  // ConnectingState for minutes on a black-holed SYN without ever erroring.
  QTimer* connectWatchdog_ = nullptr;
  int reconnectAttempts_ = 0;
  // Set once we've asked the dock to refresh the token for the current stall
  // episode, so we don't fire reconnectStalled() on every backoff tick. Reset
  // to false on a successful connect.
  bool refreshRequestedThisEpisode_ = false;
  bool connected_ = false;
  QString token_;
  QString registeredStreamId_;
  // Poste v2.
  QString deviceId_;
  bool registered_ = false;
  // Refus définitif : plus aucune reconnexion tant qu'un nouveau jeton n'arrive pas.
  bool refused_ = false;
  bool halted_ = false;
  // Annoncés par `hello`.
  int renewBeforeS_ = 120;
  qint64 maxPayloadBytes_ = 4 * 1024 * 1024;
  // `server_restarting` reçu : la prochaine reconnexion part sans reprise
  // exponentielle, après ce délai.
  int restartRetryMs_ = -1;
  void sendSetTarget();
  QString sceneLive_;
  QString sceneBug_;

  // --- Etat du portier (voir acceptRemoteCommand) ---------------------------
  // Horloge MONOTONE : QDateTime sauterait avec l'heure systeme, et une
  // horloge qui recule rouvrirait les deux fenetres d'un coup.
  QElapsedTimer commandClock_;
  // Empreinte du message -> instant de traitement. Purgee a chaque passage.
  QHash<QByteArray, qint64> recentMessages_;
  // Type de commande -> instants des acceptations encore dans la fenetre.
  QHash<QString, QVector<qint64>> commandHistory_;
  // Toutes commandes confondues, pour le plafond global.
  QVector<qint64> allCommandHistory_;
};

} // namespace semirprod
