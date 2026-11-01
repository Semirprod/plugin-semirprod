#pragma once

#include <QJsonArray>
#include <QObject>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <functional>

namespace semirprod {

/** Snapshot of the current streaming-output telemetry. Cumulative counters are
    read straight from libobs; the dock derives per-tick deltas (kbps, dropped
    delta) from two successive snapshots. `active` is false when no streaming
    output exists or it isn't running — all other fields are then zero. */
struct OutputStats {
  bool active = false;
  // Total bytes pushed by the streaming output since it started (cumulative).
  uint64_t totalBytes = 0;
  // Frames dropped by the network layer (cumulative) and the encoder's total.
  int droppedFrames = 0;
  int totalFrames = 0;
  // Network congestion estimate reported by the output, 0..1.
  float congestion = 0.0f;
  // Encoder-lag counters (video_output_get_skipped_frames / _total_frames):
  // frames the video encoder skipped because it couldn't keep up, over the
  // total it processed. Global to the OBS video pipeline, not the output.
  uint32_t renderSkipped = 0;
  uint32_t renderTotal = 0;
};

/** Outcome of a streaming-output clamp pass — which of the two limits (video
    bitrate, output resolution) had to be pulled down to fit the platform cap.
    `any()` drives whether the dock surfaces a notice to the operator. */
struct OutputClampResult {
  bool bitrateClamped = false;
  bool resolutionClamped = false;
  bool any() const { return bitrateClamped || resolutionClamped; }
};

class SceneController : public QObject {
  Q_OBJECT

public:
  explicit SceneController(QObject* parent = nullptr);

  QStringList availableScenes() const;
  QString currentScene() const;
  bool sceneExists(const QString& sceneName) const;
  bool switchToScene(const QString& sceneName);
  bool isStreamingActive() const;
  bool isRecordingActive() const;

  // === Profils et collections de scènes ====================================
  // Pilotage à distance : on BASCULE entre des profils / collections qui
  // existent déjà. Aucune création, aucun renommage, aucune suppression n'est
  // exposée — le dashboard ne doit pas pouvoir fabriquer de configuration sur
  // le poste d'un client, seulement choisir parmi ce que l'opérateur a préparé.

  QStringList availableProfiles() const;
  QString currentProfile() const;
  /** Bascule vers un profil EXISTANT. Refuse si le nom est inconnu, ou si une
      diffusion / un enregistrement tourne : OBS interdit lui-même le
      changement de profil pendant une sortie active (le menu est grisé), et le
      forcer casserait l'encodeur en cours. */
  bool switchToProfile(const QString& profileName);

  QStringList availableSceneCollections() const;
  QString currentSceneCollection() const;
  /** Bascule vers une collection de scènes EXISTANTE. Mêmes refus que
      switchToProfile : nom inconnu, ou sortie active (changer de collection
      démonte toutes les sources à l'antenne). */
  bool switchToSceneCollection(const QString& collectionName);
  // Cree une collection de scenes vide et y bascule (c'est ce que fait OBS).
  // Refus : nom vide, trop long ou deja pris, ou sortie active. `reason`
  // recoit le motif envoye au site.
  bool createSceneCollection(const QString& collectionName, QString* reason);

  // === Mode studio (preview/program) =======================================

  /** True quand OBS est en mode studio (deux canevas : préparation + antenne). */
  bool isStudioModeEnabled() const;
  /** Active/désactive le mode studio d'OBS. Retourne true si l'état demandé
      est celui d'OBS après l'appel (un appel sans changement retourne true). */
  bool setStudioMode(bool enabled);
  /** Nom de la scène en préparation. Vide hors mode studio. */
  QString previewScene() const;
  /** Place une scène EXISTANTE en préparation, SANS la diffuser. No-op hors
      mode studio (il n'y a alors pas de canevas de préparation) — utiliser
      switchToScene pour mettre à l'antenne. */
  bool setPreviewScene(const QString& sceneName);
  /** Déclenche la transition préparation -> antenne. Refuse hors mode studio. */
  bool triggerStudioTransition();

  /** Read the current streaming-output telemetry (bytes, dropped/total frames,
      congestion, encoder-lag). Returns {active:false} when nothing is
      streaming. Cheap getters — safe to call on the Qt main thread. Releases
      the reference returned by obs_frontend_get_streaming_output(). */
  OutputStats collectOutputStats() const;

  /** Enforce the platform ceiling on the STREAMING output only (never touches
      the recording output): video bitrate <= 8000 kbps and resolution within
      1920x1080 (scaled down preserving aspect ratio). Meant to run just before
      the output starts — Simple mode is clamped via the profile config, Advanced
      mode / an already-built encoder via its settings. Resolution is only
      re-scaled while the encoder is idle (a live encoder can't be resized).
      Returns which limits were actually pulled down. */
  OutputClampResult clampStreamingConfig();

  /** Safety net run right after the output goes live: re-read the streaming
      video encoder's real settings and, if the bitrate is still above the
      8000 kbps ceiling, push it down (x264/NVENC honour a live bitrate update).
      Resolution can't change on a live encoder, so only the bitrate is touched.
      Returns true if a clamp was applied. */
  bool clampStreamingEncoderLive();

  /** Clamp the active profile's video settings to 1920x1080 (canvas BaseCX/CY
      and scaled output OutputCX/CY), preserving aspect ratio. Rewrites the
      persisted config only — applying it live would require obs_reset_video,
      which is unsafe while any output runs, so the change takes effect at the
      next video reset (profile switch / restart). The encoder scaled-size clamp
      is what guarantees the wire output stays within 1080p in the meantime.
      Returns true when it changed anything. */
  bool clampVideoCanvas();

  bool startStreaming();
  bool stopStreaming();
  bool startRecording();
  bool stopRecording();
  /** Pose la destination de diffusion (URL d'ingest + clé de flux) sur le
      service de la sortie de DIFFUSION, puis la persiste dans le profil.

      REFUSE et rend `false` si une diffusion est en cours : `obs_service_update`
      ne redirige pas une sortie déjà démarrée, elle continue vers l'ancienne
      destination. Rendre `true` dans ce cas ferait afficher un succès pendant
      que le direct part ailleurs. L'appelant doit arrêter, appliquer, relancer. */
  bool applyStreamingService(const QString& rtmpUrl, const QString& streamKey);

  /** Impose le débit vidéo de diffusion des instances Cloud (6000 kbps), en
      montant comme en descendant — c'est une valeur, pas un plafond, à la
      différence de `clampStreamingConfig()`.

      S'applique en mode Simple (config du profil) comme en mode Avancé
      (réglages de l'encodeur), et sur un encodeur DÉJÀ actif : x264 et NVENC
      honorent une mise à jour de débit en plein direct. Rend `true` si quelque
      chose a bougé. Edition cloud seulement : ailleurs, ne fait rien et rend
      `false` (edition::pinsStreamingBitrate). */
  bool pinStreamingBitrate();

  /** Returns true if a scene item referencing the source `sourceName` exists
      inside the scene named `sceneName` (one level deep — group descent not
      supported by this helper yet). */
  bool isSourceInScene(const QString& sourceName, const QString& sceneName) const;

  /** Returns the names of the scenes that contain `sourceName`, or an empty
      list when absent everywhere. */
  QStringList scenesContainingSource(const QString& sourceName) const;

  /** Add an existing global source to a scene (no-op if already present or if
      either name is unknown). Returns true on success. */
  bool addSourceToScene(const QString& sourceName, const QString& sceneName);

  /** Returns true if any `semirprod_live_source` inside the scene named
      `sceneName` is currently delivering frames (inner ffmpeg_source has
      non-zero dimensions). Used to gate auto-switch commands — we want to
      flip to the Live scene only once playback is actually up. */
  bool isSemirprodSourceReadyInScene(const QString& sceneName) const;

  /** Returns true if the scene named `sceneName` contains ANY
      `semirprod_live_source`, ready or not.

      Distinct de isSemirprodSourceReadyInScene, et la difference est tout le
      sujet : cette derniere repond « la source diffuse-t-elle des images »,
      donc elle rend false aussi bien quand la source est absente que quand
      elle est la mais sans flux. Impossible d'en tirer un diagnostic — c'est
      pourtant la seule question que l'operateur se pose quand la bascule
      automatique ne part pas. */
  bool hasSemirprodSourceInScene(const QString& sceneName) const;

  /** Names of the scenes holding a `semirprod_live_source`. Sert a dire a
      l'operateur ou sa source se trouve REELLEMENT quand elle n'est pas dans
      sa scene de direct : c'est le cas le plus frequent, et « introuvable »
      serait faux. */
  QStringList scenesContainingSemirprodSource() const;

  /** Pose un `semirprod_live_source` dans la scene nommee, ajuste au canevas.

      Reutilise l'instance deja presente dans la collection s'il y en a une,
      et n'en cree une que s'il n'en existe aucune — sinon reparer une scene
      en dupliquerait une a chaque appel.

      APPELE UNIQUEMENT SUR GESTE EXPLICITE DE L'OPERATEUR. Le plugin ne pose
      jamais de source de lui-meme : creer des objets dans la collection de
      quelqu'un sans qu'il l'ait demande est pire que le probleme qu'on
      corrige. Rend true si la scene contient la source en sortie. */
  bool placeSemirprodSourceInScene(const QString& sceneName);

  /** Pose dans la scene EN COURS une source punaisee sur `streamId`, ajustee
      au canevas, nommee `label`.

      REUTILISE la source deja punaisee sur ce flux si elle existe, ou l'ajoute
      simplement a la scene si elle vit ailleurs : deux sources sur la meme
      camera, ce sont deux decodeurs pour une seule image, et le processeur du
      client paie deux fois. Ne detourne JAMAIS une source restee en
      automatique — celle-la appartient au flux du poste.

      GESTE EXPLICITE DE L'OPERATEUR, comme placeSemirprodSourceInScene : le
      plugin ne pose jamais de source de lui-meme. */
  bool placeStreamSourceInCurrentScene(const QString& streamId, const QString& label);

  /** Combien de scenes montrent ce flux. `autoStreamId` est le flux du poste :
      une source restee en automatique compte pour lui. */
  int sceneCountForStream(const QString& streamId, const QString& autoStreamId) const;

  /** Returns one entry per scene, each with its scene items (sceneItemId,
      sourceName, sourceType, visible, locked, hasAudio). The dashboard uses
      this to render a per-scene visibility/lock panel. */
  QJsonArray collectSceneSources() const;

  /** Returns one entry per source that OBS's own audio mixer shows a channel
      for: { name, type, muted, volumeDb, monitoringType, audioActive }.
      Membership mirrors OBSBasic::ActivateAudioSource — audio flag set, not
      hidden from the mixer, active, and audio_active. The OBS_SOURCE_AUDIO flag
      alone is NOT enough: it only means the source type can produce sound, so
      it also matches browser sources with "Control audio via OBS" off (the
      default) whose sound never reaches the mixer. `audioActive` is therefore
      always true in the result; it is kept for payload compatibility. */
  QJsonArray collectAudioInputs() const;

  /** Toggle visibility of a scene item identified by (scene, sourceName).
      First match wins when a scene contains the same source twice. */
  bool setSceneItemVisible(const QString& sceneName,
                           const QString& sourceName,
                           bool visible);

  /** Lock/unlock a scene item — locked items cannot be moved/resized in the
      OBS preview, useful to stop accidental drags during a live show. */
  bool setSceneItemLocked(const QString& sceneName,
                          const QString& sourceName,
                          bool locked);

  /** Mute/unmute the source globally. Audio mute is at the SOURCE level in
      OBS — the same mic muted in one scene is muted everywhere it appears. */
  bool setSourceMuted(const QString& sourceName, bool muted);

  /** Set the source volume in dB. Clamped to [-100, 26] dB to match the
      OBS audio mixer slider range. */
  bool setSourceVolumeDb(const QString& sourceName, float volumeDb);

  /** Sets monitoring type: 0=none, 1=monitor only, 2=monitor + output. */
  bool setSourceMonitoringType(const QString& sourceName, int monitoringType);

  /** Captures a snapshot of the current program output, scaled to `scaledWidth`
      and JPG-compressed at `jpgQuality` (0-100), then invokes `callback` (on
      the Qt main thread) with a Base64 string — empty on failure. The capture
      spans ~3 OBS ticks so the function returns immediately; callers must not
      depend on synchronous delivery. The job owns its own lifetime. */
  void takeScreenshot(int scaledWidth, int jpgQuality,
                      std::function<void(const QString&)> callback);
};

} // namespace semirprod
