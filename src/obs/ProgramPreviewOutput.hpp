#pragma once

#include <QObject>
#include <QString>

typedef struct obs_output obs_output_t;
typedef struct obs_encoder obs_encoder_t;
typedef struct obs_service obs_service_t;
typedef struct video_output video_t;
// Meme typedef que libobs (callback/calldata.h) : redeclare a l'identique pour
// garder cet en-tete libre de <obs.h>.
typedef struct calldata calldata_t;

namespace semirprod {

/** Manages a dedicated OBS WHIP output that pushes the composited PROGRAM mix
    (obs_get_video() / obs_get_audio()) to MediaMTX over WebRTC, on demand.

    This is deliberately independent of the main streaming output: it taps the
    program mix directly, so the WebRTC program preview works even when the
    broadcast is stopped — that's the whole point of the pre-live safety check
    (see docs/features/live-program-preview-webrtc.md).

    The output is lease-gated by the dashboard: SemirprodDock arms a TTL timer
    on start/refresh and calls stop() on expiry. This class owns only the
    libobs plumbing (output + dedicated encoders + WHIP service); the lease
    lifetime lives in the dock.

    Single output per plugin instance — the backend handles multi-consumer
    fan-out, so no refcount is needed here. Meant to be driven from the Qt main
    thread. */
class ProgramPreviewOutput : public QObject {
  Q_OBJECT

public:
  /** Quel mix cette sortie pousse. `Program` tape `obs_get_video()` (ce qui
      part à l'antenne). Les deux autres tapent un MIX EXTERNE fourni par
      l'appelant via setVideoMix() : `PreviewScene` celui de PreviewSceneMix
      (scène en préparation du mode studio), `Canvas` celui d'un canvas OBS
      nommé (obs_canvas_get_video, édition régie). Les instances coexistent,
      d'où des noms d'objets libobs distincts par mix : libobs indexe certains
      objets par nom, deux sorties homonymes se marcheraient dessus. */
  enum class Mix { Program, PreviewScene, Canvas };

  explicit ProgramPreviewOutput(QObject* parent = nullptr)
      : ProgramPreviewOutput(Mix::Program, parent) {}
  explicit ProgramPreviewOutput(Mix mix, QObject* parent = nullptr);
  ~ProgramPreviewOutput() override;

  /** Mix externe à encoder (`Mix::PreviewScene`, `Mix::Canvas`). À poser AVANT start() et à
      garder vivant tant que la sortie tourne. Ignoré pour `Mix::Program`, qui
      lit toujours le mix programme. Un changement de mix force la
      reconstruction au prochain start(). */
  void setVideoMix(video_t* video);

  /** Delai avant qu'une generation eteinte soit reellement relachee.

      Expose parce que ce n'est pas un detail interne : `obs_output_stop()` est
      ASYNCHRONE, donc pendant tout ce delai la sortie tourne encore et
      consomme toujours son mix video. Tout ce qui ALIMENTE une sortie doit
      donc lui survivre au moins aussi longtemps — voir
      SemirprodDock::stopStudioPreview, ou detruire le mix trop tot faisait
      tomber OBS dans end_data_capture_thread. */
  static constexpr int kRetireGraceMs = 5000;

  /** True only when this OBS build ships the native WHIP output type
      (`whip_output`, OBS 30+). Probed at runtime via obs_enum_output_types so
      the plugin can advertise the `program_preview_whip` capability only when
      it can actually honour it. Safe to call before any output is created. */
  static bool isSupported();

  /** Plafonds de TOUS les apercus WebRTC (programme, preparation, vertical,
      canvas), comme ceux de start_preview : 1920 px sur le grand cote,
      1080 px sur le petit, 6000 kbps. Les valeurs arrivent du serveur ; sans
      plafond, un message demandait un encodage 8K a 100 Mbit/s et saturait le
      GPU et la liaison montante du poste pendant le direct. Une valeur nulle
      ou negative est laissee telle quelle : elle veut dire « deduire du
      canvas » ou « valeur par defaut » pour l'appelant. */
  static void clampEncodeParams(int* width, int* height, int* bitrateKbps);

  /** (Re)start the WHIP output against `whipUrl` with `bearerToken`, encoding
      the program mix scaled to width×height at the given fps / bitrate.

      Idempotent: a lease refresh with identical url/size/bitrate while the
      output is live is a no-op (rebuilding would drop the WHIP session and
      kick every WHEP viewer). Changed parameters, or a dead output, trigger a
      clean teardown + rebuild. Returns false and leaves nothing running if
      the WHIP type is unavailable or any libobs object failed to build. */
  bool start(const QString& whipUrl, const QString& bearerToken, int width,
             int height, int fps, int bitrateKbps);

  /** Stop the output and release every libobs reference. Idempotent — safe to
      call when nothing is running (e.g. lease expiry after a manual stop). */
  void stop(bool force = false);

  /** True between a successful start() and a stop(). Reflects our own intent,
      not the async OBS output state. */
  bool isRunning() const { return running_; }

private:
  // La sortie vivante s'est arretee d'elle-meme (echec WHIP, coupure reseau,
  // arret cote MediaMTX). Rebascule `running_` a faux pour que le prochain
  // rafraichissement de bail reconstruise au lieu de croire la sortie en vie.
  //
  // Necessaire depuis que la garde d'idempotence ne s'appuie plus sur
  // obs_output_active() : cet etat est faux pendant TOUTE la negociation WHIP
  // (ICE/DTLS), donc un rafraichissement arrive dans cette fenetre passait
  // pour un changement de parametres et declenchait un teardown au pire
  // moment. Le signal est desormais la seule source de verite sur la mort de
  // la sortie.
  void handleLiveOutputStopped();
  static void onLiveOutputStoppedSignal(void* data, calldata_t* cd);
  // Build encoders + service + output for `vencId` and start the output.
  // Returns false on any failure WITHOUT cleaning up — the caller tears down
  // and may retry with the x264 fallback (hardware encoders only truly
  // initialise inside obs_output_start, e.g. NVENC out of sessions).
  bool buildAndStart(const char* vencId, bool isX264, const QString& whipUrl,
                     const QString& bearerToken, int w, int h, int bitrate);

  // Tear down + release output / encoders / service in dependency order. Used
  // by stop() and by start() before a rebuild.
  void teardown(bool force = false);

  // Mix vidéo réellement encodé : le programme, ou le mix externe posé par
  // setVideoMix(). nullptr = rien à encoder, start() refuse.
  video_t* videoMix() const;

  const Mix mix_;
  video_t* externalVideo_ = nullptr;
  obs_output_t* output_ = nullptr;
  obs_encoder_t* videoEncoder_ = nullptr;
  obs_encoder_t* audioEncoder_ = nullptr;
  obs_service_t* service_ = nullptr;
  bool running_ = false;

  // Last applied parameters — lets start() detect a same-params lease refresh
  // and keep the live WHIP session instead of rebuilding (which drops viewers).
  QString whipUrl_;
  // Mix effectivement branché sur l'encodeur en cours. Un mix différent (vue de
  // préparation recréée) impose une reconstruction, même à paramètres égaux.
  video_t* builtVideo_ = nullptr;
  int width_ = 0;
  int height_ = 0;
  int bitrateKbps_ = 0;
};

} // namespace semirprod
