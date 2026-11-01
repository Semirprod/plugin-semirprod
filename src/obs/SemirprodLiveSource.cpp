#include "SemirprodLiveSource.hpp"
#include "../StreamRegistry.hpp"

#include <obs-module.h>
#include <obs.h>
#include <media-io/video-scaler.h>
#include <util/platform.h>

#include <QCoreApplication>
#include <QHash>
#include <QList>
#include <QMutex>
#include <QMutexLocker>
#include <QString>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include "../Locale.hpp"

namespace semirprod {

constexpr const char* kFrameTapFilterId = "semirprod_frame_tap";

// Platform ceiling for the incoming feed. A stream above this is downscaled in
// the frame tap before OBS ever sees it, so a client pushing 4K doesn't cost a
// 4K texture upload per frame for a picture that's shown inside a <=1080p canvas.
constexpr uint32_t kMaxIngestWidth = 1920;
constexpr uint32_t kMaxIngestHeight = 1080;

struct SourceData {
  obs_source_t* self = nullptr;
  obs_source_t* inner = nullptr;
  // Flux REELLEMENT applique a cette instance (celui dont on lit l'URL).
  std::string selectedStreamId;
  // Flux PUNAISE par l'operateur dans les proprietes de la source. Vide = mode
  // automatique : l'instance suit le flux du poste.
  //
  // Les deux ne se confondent pas : une instance punaisee sur une camera qui
  // n'est pas encore dans le registre porte son choix (`pinnedStreamId`) sans
  // rien afficher (`selectedStreamId` vide). C'est ce qui lui permet de se
  // raccrocher toute seule quand la camera arrive.
  std::string pinnedStreamId;

  QMutex mutex;
  // Sérialise `applyStream` de bout en bout. Distinct de `mutex` (pris par le
  // frame tap sur le thread de décodage) pour éviter tout ordre de verrous
  // croisé entre reconfiguration et traitement de frame.
  QMutex applyMutex;
  std::atomic<bool> is_ready{false};
  std::atomic<uint32_t> width{0};
  std::atomic<uint32_t> height{0};

  std::atomic<uint64_t> lastAudioLog{0};
  // Derniere IMAGE recue. Le son ne la rafraichit pas : un flux dont seule
  // la piste audio vit encore (decodeur video mort apres un changement de
  // camera) doit se lire comme « plus d'image », pas comme « en direct ».
  std::atomic<uint64_t> lastFreshNs{0};
  std::atomic<uint64_t> lastPlaceholderNs{0};
  // Ouverture de la source interne (applyStream ou relance a la publication) :
  // sert a journaliser le delai jusqu'a la premiere image.
  std::atomic<uint64_t> openedNs{0};
  // Garde-fou des pistes (voir `watchTracks`). `*ResumedNs` : retour de la
  // piste apres une coupure. `*LossHandled` : une relance par perte, retombe
  // quand la piste perdue revient.
  std::atomic<uint64_t> lastAudioNs{0};
  std::atomic<uint64_t> videoResumedNs{0};
  std::atomic<uint64_t> audioResumedNs{0};
  std::atomic<bool> audioLossHandled{false};
  std::atomic<bool> videoLossHandled{false};

  // Downscale state for the 1080p ingest cap. Touched on the inner source's
  // async decode thread (frame tap) and on teardown — both under `mutex`.
  video_scaler_t* scaler = nullptr;
  obs_source_frame* dstFrame = nullptr;
  enum video_format scalerSrcFormat = VIDEO_FORMAT_NONE;
  uint32_t scalerSrcW = 0;
  uint32_t scalerSrcH = 0;
  uint32_t scalerDstW = 0;
  uint32_t scalerDstH = 0;
  // Native (pre-cap) dimensions + a flag the dock polls to notify the operator
  // once that the incoming feed exceeded 1080p. Reset when the stream changes.
  std::atomic<bool> oversize{false};
  std::atomic<uint32_t> nativeWidth{0};
  std::atomic<uint32_t> nativeHeight{0};
  std::atomic<bool> oversizeLogged{false};
};

// Scale (inW,inH) down to fit within (maxW,maxH) preserving aspect ratio, even
// dims (chroma-subsampled formats need them). No-op when it already fits.
void fitIngest(uint32_t inW, uint32_t inH, uint32_t maxW, uint32_t maxH,
               uint32_t* outW, uint32_t* outH) {
  if (inW == 0 || inH == 0) {
    *outW = inW;
    *outH = inH;
    return;
  }
  const double scale = std::min(static_cast<double>(maxW) / inW,
                                static_cast<double>(maxH) / inH);
  if (scale >= 1.0) {
    *outW = inW;
    *outH = inH;
    return;
  }
  uint32_t w = static_cast<uint32_t>(inW * scale) & ~1u;
  uint32_t h = static_cast<uint32_t>(inH * scale) & ~1u;
  if (w == 0) w = 2;
  if (h == 0) h = 2;
  *outW = w;
  *outH = h;
}

namespace {

// Les instances sont détenues par des `shared_ptr` et NON par des pointeurs nus.
//
// Le frame tap tourne sur le thread de décodage du ffmpeg_source interne : il
// récupérait le `SourceData*` sous verrou, RELÂCHAIT le verrou, puis
// déréférençait le pointeur pendant une quarantaine de lignes. En parallèle,
// `sourceDestroy` (thread graphique) faisait `delete d` sans aucune
// synchronisation avec un tap en cours. La fenêtre s'ouvrait à CHAQUE frame
// décodée, 25 à 60 fois par seconde : supprimer la source pendant que le flux
// arrive faisait tomber OBS, donc le direct, de façon non déterministe.
//
// Copier le `shared_ptr` sous verrou garantit que l'objet survit à toute la
// durée d'utilisation, sans sérialiser le scaling sous le verrou global.
// `sourceDestroy` relâche simplement sa référence : la destruction réelle a lieu
// quand le dernier utilisateur a fini.
QMutex g_instancesMutex;
QHash<obs_source_t*, std::shared_ptr<SourceData>> g_instances;

// Récupère une référence forte sur l'instance associée à `self`, ou nullptr.
std::shared_ptr<SourceData> holdInstance(obs_source_t* self) {
  QMutexLocker lock(&g_instancesMutex);
  return g_instances.value(self, nullptr);
}

// Map inner ffmpeg_source -> self. Le filter video greffé sur l'inner regarde
// dans cette map pour savoir vers quelle source de sortie il doit forwarder
// les frames. Protégé par g_innerToSelfMutex.
QMutex g_innerToSelfMutex;
QHash<obs_source_t*, obs_source_t*> g_innerToSelf;

void registerInstance(const std::shared_ptr<SourceData>& d) {
  QMutexLocker lock(&g_instancesMutex);
  g_instances.insert(d->self, d);
}

// Retire l'instance de la table et RENVOIE la référence qu'on y détenait, pour
// que l'appelant contrôle le moment où elle retombe (jamais sous le verrou).
std::shared_ptr<SourceData> unregisterInstance(obs_source_t* self) {
  QMutexLocker lock(&g_instancesMutex);
  return g_instances.take(self);
}

void markFresh(const std::shared_ptr<SourceData>& d, uint32_t w, uint32_t h) {
  if (w > 0 && h > 0) {
    d->width.store(w);
    d->height.store(h);
    d->is_ready.store(true);
  }
  const uint64_t now = os_gettime_ns();
  d->videoLossHandled.store(false);
  // Premiere image depuis l'ouverture : la seule mesure du delai de lecture.
  // `lastFreshNs` repasse a 0 a chaque releaseInner, donc une fois par ouverture.
  const uint64_t prev = d->lastFreshNs.exchange(now);
  if (prev == 0) {
    const uint64_t opened = d->openedNs.load();
    if (opened != 0 && now > opened) {
      blog(LOG_INFO, "[semirprod-live-source] first frame +%llu ms",
           static_cast<unsigned long long>((now - opened) / 1000000ULL));
    }
  }
  if (prev == 0 || now - prev >= 1500000000ULL) d->videoResumedNs.store(now);
}

// Taille forcee de la source quand aucun flux ne l'alimente. OBS derive la
// taille d'une source ASYNC (obs_source_get_width/height) depuis async_width/
// async_height, qui ne sont fixes qu'une fois une frame poussee via
// obs_source_output_video ; definir get_width/get_height ne sert a rien pour une
// source async (OBS les ignore). Sans frame, la source vaut donc 0x0 et devient
// impossible a positionner dans la scene. On pousse un placeholder de cette
// taille pour que la boite reste fixe et deja placable avant meme le premier
// flux.
constexpr uint32_t kPlaceholderWidth = 1920;
constexpr uint32_t kPlaceholderHeight = 1080;

// Pousse une frame noire 1920x1080 sur `self`. NE touche PAS lastFreshNs ni
// is_ready : le placeholder n'est pas un flux live, donc la logique "flux
// present" du dock (isSourceInstanceReady / queryInstanceState) reste correcte.
void pushPlaceholderFrame(SourceData* d) {
  if (!d || !d->self) return;

  // Buffer noir opaque partage (BGRX : octet X ignore, B=G=R=0 => noir).
  // obs_source_output_video copie les donnees en interne (cache_video), donc un
  // buffer statique en lecture seule est safe meme si plusieurs instances
  // poussent en parallele.
  static const std::vector<uint8_t> black(
      static_cast<size_t>(kPlaceholderWidth) * kPlaceholderHeight * 4, 0);

  struct obs_source_frame frame{};
  frame.format = VIDEO_FORMAT_BGRX;
  frame.width = kPlaceholderWidth;
  frame.height = kPlaceholderHeight;
  frame.full_range = true;
  frame.timestamp = os_gettime_ns();
  frame.linesize[0] = kPlaceholderWidth * 4;
  frame.data[0] = const_cast<uint8_t*>(black.data());

  obs_source_output_video(d->self, &frame);
}

// === FRAME TAP FILTER ===
// On ne peut pas modifier ffmpeg_source pour qu'il pousse ses frames vers
// notre source. Mais OBS permet d'attacher un filtre vidéo à n'importe quelle
// source — le filter_video callback est invoqué à chaque frame async décodée.
// On exploite ça pour intercepter les frames de l'inner et les ré-pousser sur
// `self` via obs_source_output_video. Comme on pousse aussi l'audio sur self
// via le capture_callback, OBS dispose des deux flux avec leurs PTS et son
// pipeline async sync les présente alignés — exactement comme le ferait
// ffmpeg_source si self était lui-même un ffmpeg_source.

void* frameTapCreate(obs_data_t*, obs_source_t* source) {
  return source;  // on a juste besoin de la source filtre pour obs_filter_get_parent
}

void frameTapDestroy(void*) {}

const char* frameTapGetName(void*) { return "Semirprod Frame Tap"; }

// Ensure d->scaler + d->dstFrame are built for (frame format/size) -> (dstW,dstH).
// Must be called with d->mutex held. Returns false on any failure so the caller
// can fall back to pushing the original frame unchanged. Fail-safe by design:
// the ingest cap is an optimisation, never a reason to drop a client's feed.
bool ensureIngestScaler(SourceData* d, const obs_source_frame* frame,
                        uint32_t dstW, uint32_t dstH) {
  if (d->scaler && d->scalerSrcFormat == frame->format &&
      d->scalerSrcW == frame->width && d->scalerSrcH == frame->height &&
      d->scalerDstW == dstW && d->scalerDstH == dstH) {
    return true;  // current scaler already matches
  }

  if (d->scaler) {
    video_scaler_destroy(d->scaler);
    d->scaler = nullptr;
  }
  if (d->dstFrame) {
    obs_source_frame_destroy(d->dstFrame);
    d->dstFrame = nullptr;
  }

  // Same format on both ends — a pure resize, no colour-space conversion, which
  // keeps the scaler simple and avoids guessing the source colour matrix.
  struct video_scale_info src{};
  src.format = frame->format;
  src.width = frame->width;
  src.height = frame->height;
  src.range = frame->full_range ? VIDEO_RANGE_FULL : VIDEO_RANGE_PARTIAL;
  src.colorspace = VIDEO_CS_DEFAULT;
  struct video_scale_info dst = src;
  dst.width = dstW;
  dst.height = dstH;

  video_scaler_t* scaler = nullptr;
  if (video_scaler_create(&scaler, &dst, &src, VIDEO_SCALE_BILINEAR) !=
      VIDEO_SCALER_SUCCESS) {
    return false;
  }
  obs_source_frame* out = obs_source_frame_create(frame->format, dstW, dstH);
  if (!out) {
    video_scaler_destroy(scaler);
    return false;
  }

  d->scaler = scaler;
  d->dstFrame = out;
  d->scalerSrcFormat = frame->format;
  d->scalerSrcW = frame->width;
  d->scalerSrcH = frame->height;
  d->scalerDstW = dstW;
  d->scalerDstH = dstH;
  return true;
}

obs_source_frame* frameTapFilterVideo(void* data, obs_source_frame* frame) {
  if (!frame) return frame;
  auto* filterSource = static_cast<obs_source_t*>(data);
  obs_source_t* parent = obs_filter_get_parent(filterSource);
  if (!parent) return frame;

  // Référence FORTE sur la source de sortie, prise sous le verrou. Sans elle, on
  // poussait une frame sur une source déjà en cours de destruction :
  // `obs_source_get_ref` renvoie nullptr dans ce cas, c'est exactement le garde
  // qui manquait. Le release est fait sur tous les chemins de sortie via ce
  // petit RAII (la fonction a plusieurs `return`).
  obs_source_t* target = nullptr;
  {
    QMutexLocker lock(&g_innerToSelfMutex);
    obs_source_t* candidate = g_innerToSelf.value(parent, nullptr);
    if (candidate) target = obs_source_get_ref(candidate);
  }
  if (!target) return frame;

  struct TargetRelease {
    obs_source_t* s;
    ~TargetRelease() { if (s) obs_source_release(s); }
  } targetGuard{target};

  // Référence forte sur l'instance : elle survit à un `sourceDestroy`
  // concurrent pendant toute la durée de ce tap.
  const std::shared_ptr<SourceData> dHold = holdInstance(target);
  SourceData* d = dHold.get();

  const bool oversize =
      frame->width > kMaxIngestWidth || frame->height > kMaxIngestHeight;

  if (oversize && d) {
    d->oversize.store(true);
    d->nativeWidth.store(frame->width);
    d->nativeHeight.store(frame->height);
    if (!d->oversizeLogged.exchange(true)) {
      // No resolution figure that could identify the feed, no url/key — just the
      // fact that we capped it. Once per stream (reset on stream change).
      blog(LOG_INFO,
           "[semirprod-live-source] incoming feed above 1080p — capped to 1080p");
    }

    // Downscale into our owned dstFrame and push that instead. On ANY failure we
    // fall through and push the original frame untouched.
    QMutexLocker lock(&d->mutex);
    // Guard against a concurrent teardown having swapped/cleared the inner.
    if (d->inner == parent) {
      uint32_t dw = frame->width, dh = frame->height;
      fitIngest(frame->width, frame->height, kMaxIngestWidth, kMaxIngestHeight,
                &dw, &dh);
      if (ensureIngestScaler(d, frame, dw, dh) &&
          video_scaler_scale(d->scaler, d->dstFrame->data, d->dstFrame->linesize,
                             frame->data, frame->linesize)) {
        d->dstFrame->timestamp = frame->timestamp;
        d->dstFrame->flip = frame->flip;
        d->dstFrame->full_range = frame->full_range;
        std::memcpy(d->dstFrame->color_matrix, frame->color_matrix,
                    sizeof(frame->color_matrix));
        std::memcpy(d->dstFrame->color_range_min, frame->color_range_min,
                    sizeof(frame->color_range_min));
        std::memcpy(d->dstFrame->color_range_max, frame->color_range_max,
                    sizeof(frame->color_range_max));
        markFresh(dHold, dw, dh);
        obs_source_output_video(target, d->dstFrame);
        return frame;  // original passes down the (empty) filter chain untouched
      }
    }
  } else if (d) {
    d->oversize.store(false);
  }

  if (d) markFresh(dHold, frame->width, frame->height);
  obs_source_output_video(target, frame);
  return frame;  // pass-through (pas de chaîne de rendu en aval, mais conservons l'invariant)
}

void registerFrameTapFilter() {
  static obs_source_info info{};
  info.id = kFrameTapFilterId;
  info.type = OBS_SOURCE_TYPE_FILTER;
  info.output_flags = OBS_SOURCE_ASYNC_VIDEO;
  info.get_name = frameTapGetName;
  info.create = frameTapCreate;
  info.destroy = frameTapDestroy;
  info.filter_video = frameTapFilterVideo;
  obs_register_source(&info);
}

// === AUDIO CAPTURE CALLBACK ===
// L'audio est tappé sur l'inner et ré-emit sur self avec le timestamp original.
// OBS sync engine sur self matche audio (PTS) et vidéo (PTS) → pas de drift.
void audioCaptureCb(void* param, obs_source_t* /*src*/,
                    const struct audio_data* audio, bool muted) {
  auto* d = static_cast<SourceData*>(param);
  if (!d || !d->self || !audio || muted) return;
  if (audio->frames == 0 || !audio->data[0]) return;

  audio_t* ao = obs_get_audio();
  if (!ao) return;
  const struct audio_output_info* aoi = audio_output_get_info(ao);
  if (!aoi) return;

  struct obs_source_audio out{};
  for (size_t i = 0; i < MAX_AV_PLANES; ++i) {
    out.data[i] = audio->data[i];
  }
  out.frames = audio->frames;
  out.timestamp = audio->timestamp;
  out.speakers = aoi->speakers;
  out.format = aoi->format;
  out.samples_per_sec = aoi->samples_per_sec;

  uint64_t now = os_gettime_ns();
  const uint64_t prevAudio = d->lastAudioNs.exchange(now);
  if (prevAudio == 0 || now - prevAudio >= 1500000000ULL) d->audioResumedNs.store(now);
  d->audioLossHandled.store(false);
  if (now - d->lastAudioLog.load() > 5000000000ULL) {
    blog(LOG_INFO, "[semirprod-live-source] audio fwd %u frames @ %u Hz",
         audio->frames, aoi->samples_per_sec);
    d->lastAudioLog.store(now);
  }

  obs_source_output_audio(d->self, &out);
}

// Sondage court. Sans lui, un flux sans piste audio fait attendre ffmpeg
// jusqu'a `analyzeduration` (5 s) une piste qui ne viendra pas.
// Contrepartie : une image cle de plus de 2 Mo arriverait incomplete au
// sondage (premieres images grises), bien au-dela du plafond de 6000 kbps.
constexpr const char* kProbeShort =
    "analyzeduration=1000000 probesize=2000000 fpsprobesize=0";
// Sondage long, pose par `reopenInner` quand une piste manque.
constexpr const char* kProbeLong =
    "analyzeduration=3000000 probesize=4000000 fpsprobesize=0";

obs_source_t* createInnerMedia(const QString& url) {
  if (url.isEmpty()) return nullptr;
  obs_data_t* settings = obs_data_create();
  obs_data_set_string(settings, "input", url.toUtf8().constData());
  obs_data_set_bool(settings, "is_local_file", false);
  obs_data_set_bool(settings, "looping", false);
  obs_data_set_bool(settings, "clear_on_media_end", false);
  // DECODAGE MATERIEL demande. Etait a `false` depuis le premier commit de cette
  // source, sans justification ecrite — un defaut d'echafaudage jamais revisite.
  //
  // Le flux arrive en H.264/HEVC : le decoder en logiciel consomme du CPU pendant
  // qu'un GPU capable reste inactif. Mesure du 17 aout sur une instance en
  // conteneur : le decodeur du GPU affichait 0 % d'utilisation alors que le
  // processeur portait tout.
  //
  // Sans risque de panne : ffmpeg_source retombe SEUL en logiciel si l'accelerateur
  // manque ou refuse. Ce drapeau est une preference, pas une exigence — d'ou le
  // fait qu'on puisse le mettre a `true` sans condition de plateforme.
  //   Windows : NVDEC par le pilote.
  //   Linux   : h264_cuvid / hevc_cuvid / av1_cuvid, presents dans le libavcodec
  //             d'Ubuntu, plus libnvcuvid fournie par le pilote (verifie).
  //
  // A SURVEILLER : le decodage materiel ajoute son propre pipeline. Si une
  // latence supplementaire apparaissait, c'est ici qu'il faudrait revenir — en
  // mesurant (ligne « first frame » du journal), pas en supposant.
  obs_data_set_bool(settings, "hw_decode", true);
  // La cle est `buffering_mb` : l'ancien `buffering` n'existait pas et ne
  // faisait rien. On la garde > 0 : a 0, OBS pose AVFMT_FLAG_NOBUFFER, l'image
  // cle lue pendant le sondage est jetee et le demarrage attend la suivante.
  obs_data_set_int(settings, "buffering_mb", 2);
  obs_data_set_string(settings, "ffmpeg_options", kProbeShort);
  // Delai avant de retenter apres un echec d'ouverture (10 s par defaut chez
  // OBS). La source s'ouvre souvent AVANT que le flux publie : elle tombait
  // alors dans cette attente. La relance immediate a la publication passe par
  // `kickStalledInstances` ; ce delai n'est plus qu'un filet. Pas 1 s : chaque
  // OBS ouvert hors direct interrogerait le serveur chaque seconde.
  obs_data_set_int(settings, "reconnect_delay_sec", 3);
  // OBS >= 30 : sans ce drapeau, la source ecrit ses reglages dans le journal,
  // donc l'URL de lecture avec la cle et le jeton.
  obs_data_set_bool(settings, "log_changes", false);
  obs_data_set_bool(settings, "restart_on_activate", false);
  obs_data_set_bool(settings, "close_when_inactive", false);
  obs_source_t* src = obs_source_create_private("ffmpeg_source", "Semirprod_Internal_Media", settings);
  obs_data_release(settings);
  return src;
}

// Rouvre la source interne avec le sondage long. Appelant : fil Qt, sous
// `applyMutex`.
void reopenInner(SourceData* d, const char* reason) {
  obs_source_t* inner = nullptr;
  {
    QMutexLocker lock(&d->mutex);
    if (d->inner) inner = obs_source_get_ref(d->inner);
  }
  if (!inner) return;
  obs_data_t* settings = obs_data_create();
  obs_data_set_string(settings, "ffmpeg_options", kProbeLong);
  d->openedNs.store(os_gettime_ns());
  // Comme `kickStalledInstances` : ffmpeg_source_update rouvre tout de suite.
  obs_source_update(inner, settings);
  obs_data_release(settings);
  obs_source_release(inner);
  blog(LOG_INFO, "[semirprod-live-source] %s, reopening with a longer probe", reason);
}

// === GARDE-FOU DES PISTES ===
// Une piste meurt, l'autre continue, et rien ne la ramenait avant un
// redemarrage d'OBS :
//  * image sans son. Le streamer change de camera : son flux coupe puis
//    republie, la source interne rouvre (500 ms apres la publication via
//    `kickStalledInstances`, ou par sa propre reconnexion). Si aucun paquet
//    audio n'arrive pendant le sondage court, ffmpeg voit la piste audio sans
//    frequence ni canaux, `av_find_best_stream` l'ecarte et OBS n'ouvre aucun
//    decodeur audio. Meme effet quand le lecteur reste connecte pendant la
//    republication : la nouvelle piste apparait en cours de lecture et OBS ne
//    l'ouvre jamais ;
//  * son sans image. Decodeur video mort (resolution ou codec change en cours
//    de lecture) : l'image restait figee, sans relance ni bascule de secours.
//
// Une piste absente depuis 3 s alors que l'autre est revenue depuis 3 s => une
// reouverture avec le sondage long. Le reglage reste sur la source interne,
// donc les reconnexions suivantes sondent long elles aussi. Pas d'autre
// relance tant que la piste perdue n'est pas revenue : un flux reellement sans
// micro coute une reouverture, il ne coupe pas l'image toutes les 3 s.
constexpr uint64_t kTrackLostNs = 3000000000ULL;
constexpr uint64_t kTrackLiveNs = 1500000000ULL;

void recoverLostTrack(const std::weak_ptr<SourceData>& weak, bool audioLost) {
  std::shared_ptr<SourceData> d = weak.lock();
  if (!d) return;
  QMutexLocker applyLock(&d->applyMutex);
  reopenInner(d.get(), audioLost ? "audio lost while video plays"
                                 : "video lost while audio plays");
}

// Appelee a chaque tick video (fil graphique) : lectures atomiques seulement,
// la reouverture part sur le fil Qt.
void watchTracks(SourceData* d, uint64_t now) {
  const uint64_t video = d->lastFreshNs.load();
  const uint64_t audio = d->lastAudioNs.load();
  const bool videoLive = video != 0 && now - video < kTrackLiveNs;
  const bool audioLive = audio != 0 && now - audio < kTrackLiveNs;
  const bool audioMissing = audio == 0 || now - audio >= kTrackLostNs;
  const bool videoMissing = video == 0 || now - video >= kTrackLostNs;

  bool audioLost = false;
  if (videoLive && audioMissing && now - d->videoResumedNs.load() >= kTrackLostNs) {
    if (d->audioLossHandled.exchange(true)) return;
    audioLost = true;
  } else if (audioLive && videoMissing &&
             now - d->audioResumedNs.load() >= kTrackLostNs) {
    if (d->videoLossHandled.exchange(true)) return;
  } else {
    return;
  }

  std::shared_ptr<SourceData> held = holdInstance(d->self);
  QCoreApplication* app = QCoreApplication::instance();
  if (!held || !app) return;
  const std::weak_ptr<SourceData> weak = held;
  QMetaObject::invokeMethod(
      app, [weak, audioLost]() { recoverLostTrack(weak, audioLost); },
      Qt::QueuedConnection);
}

// `teardown` : appelé depuis sourceDestroy. On saute alors le placeholder, qui
// pousserait une frame sur une source dont la destruction est déjà entamée.
void releaseInner(SourceData* d, bool teardown = false) {
  obs_source_t* inner = nullptr;
  {
    QMutexLocker lock(&d->mutex);
    inner = d->inner;
    d->inner = nullptr;
    d->is_ready = false;
    d->width = 0;
    d->height = 0;
    d->lastFreshNs = 0;
    d->lastAudioNs = 0;
    d->videoResumedNs = 0;
    d->audioResumedNs = 0;
    d->audioLossHandled = false;
    d->videoLossHandled = false;
    // Tear down the ingest-cap scaler under the same lock the frame tap uses,
    // and reset the oversize state so the next stream re-notifies if needed.
    if (d->scaler) {
      video_scaler_destroy(d->scaler);
      d->scaler = nullptr;
    }
    if (d->dstFrame) {
      obs_source_frame_destroy(d->dstFrame);
      d->dstFrame = nullptr;
    }
    d->scalerSrcFormat = VIDEO_FORMAT_NONE;
    d->scalerSrcW = d->scalerSrcH = d->scalerDstW = d->scalerDstH = 0;
    d->oversize.store(false);
    d->oversizeLogged.store(false);
    d->nativeWidth.store(0);
    d->nativeHeight.store(0);
  }
  if (inner) {
    {
      QMutexLocker lock(&g_innerToSelfMutex);
      g_innerToSelf.remove(inner);
    }
    obs_source_remove_audio_capture_callback(inner, audioCaptureCb, d);
    // Le filter video privé attaché à inner est détruit automatiquement quand
    // inner est libéré (obs gère son cycle de vie via la chaîne de filtres).
    obs_source_release(inner);
    // Au lieu de clear (nullptr => taille 0x0), on repasse sur le placeholder
    // 1920x1080 pour garder la source dimensionnee/placable apres une coupure.
    pushPlaceholderFrame(d);
  }
}

// Valeur du reglage `stream_id` qui signifie « suivre le flux du poste ».
// Ecrite dans la collection de scenes du client : ne jamais la changer.
constexpr const char* kStreamSettingAuto = "auto";
constexpr const char* kStreamSettingKey = "stream_id";

// Le flux qu'une instance doit afficher : le sien s'il est punaise, celui du
// poste sinon.
QString resolveStreamId(SourceData* d) {
  if (!d) return QString();
  std::string pinned;
  {
    QMutexLocker lock(&d->mutex);
    pinned = d->pinnedStreamId;
  }
  if (!pinned.empty()) return QString::fromStdString(pinned);
  return StreamRegistry::instance().activeStreamId();
}

// Lit le reglage et le range dans l'instance. Rend le flux a appliquer.
QString adoptSettings(SourceData* d, obs_data_t* settings) {
  if (!d) return QString();
  const char* raw = settings ? obs_data_get_string(settings, kStreamSettingKey) : nullptr;
  std::string pinned = raw ? raw : "";
  if (pinned == kStreamSettingAuto) pinned.clear();
  {
    QMutexLocker lock(&d->mutex);
    d->pinnedStreamId = pinned;
  }
  return resolveStreamId(d);
}

void applyStream(SourceData* d, const QString& streamId) {
  if (!d) return;

  // Sérialisation de TOUTE la séquence (release → create → publication de
  // `inner` → enregistrement du callback audio).
  //
  // Chaque étape prenait et relâchait `d->mutex` séparément, alors que trois
  // appelants peuvent s'exécuter concurremment (`sourceCreate`, `sourceUpdate`
  // déclenché par libobs à la validation des propriétés, et
  // `reapplyActiveStreamToAllInstances` depuis le thread Qt). Deux exécutions
  // simultanées laissaient un `ffmpeg_source` complet orphelin : référence
  // jamais relâchée, entrée `g_innerToSelf` conservée donc son frame tap
  // continuait de pousser des frames (image mélangée, audio dédoublé, un
  // décodeur entier de CPU en trop), et son callback audio restait enregistré
  // avec un `d` qui serait libéré à la destruction.
  //
  // Verrou DÉDIÉ, distinct de `d->mutex` : le frame tap prend `d->mutex` et
  // peut être en cours pendant qu'on reconfigure. Les prendre dans le même
  // ordre ici et là-bas éviterait l'interblocage, mais un verrou séparé rend
  // l'invariant explicite et supprime la question.
  QMutexLocker applyLock(&d->applyMutex);

  releaseInner(d);
  {
    QMutexLocker lock(&d->mutex);
    d->selectedStreamId = streamId.toStdString();
  }
  const QString playbackUrl = StreamRegistry::instance().playbackUrlForId(streamId);
  if (playbackUrl.isEmpty()) return;

  obs_source_t* inner = createInnerMedia(playbackUrl);
  if (!inner) return;

  d->openedNs.store(os_gettime_ns());
  {
    QMutexLocker lock(&d->mutex);
    d->inner = inner;
  }
  {
    QMutexLocker lock(&g_innerToSelfMutex);
    g_innerToSelf.insert(inner, d->self);
  }

  obs_source_add_audio_capture_callback(inner, audioCaptureCb, d);

  // Greffer le filter video sur l'inner pour intercepter chaque frame async.
  obs_source_t* filter = obs_source_create_private(kFrameTapFilterId, "Semirprod_Frame_Tap", nullptr);
  if (filter) {
    obs_source_filter_add(inner, filter);
    obs_source_release(filter);  // inner garde la référence via la chaîne de filtres
  } else {
    blog(LOG_WARNING, "[semirprod-live-source] frame tap filter creation failed");
  }

  // L'URL de lecture embarque la clé de stream — jamais en clair dans les
  // logs OBS (partagés publiquement par les utilisateurs).
  blog(LOG_INFO, "[semirprod-live-source] streamId=%s url=%s",
       streamId.toUtf8().constData(), playbackUrl.isEmpty() ? "absent" : "present");
}

const char* sourceGetName(void*) { return locC("Source.Live"); }

void* sourceCreate(obs_data_t* settings, obs_source_t* source) {
  // La table `g_instances` détient la propriété (shared_ptr). libobs ne reçoit
  // qu'un pointeur nu d'observation : il ne le libère jamais lui-même, c'est
  // `sourceDestroy` qui relâche la référence détenue par la table.
  auto d = std::make_shared<SourceData>();
  d->self = source;
  registerInstance(d);
  obs_source_set_audio_mixers(source, 0xFF);
  // Placeholder immediat : la source doit valoir 1920x1080 des sa creation,
  // meme avant que le flux (qui met du temps a se connecter) ne pousse la
  // moindre frame, sinon elle apparait en 0x0 et n'est pas positionnable.
  pushPlaceholderFrame(d.get());
  // Les reglages sont lus DES LA CREATION : une source punaisee retrouvee dans
  // une collection de scenes doit afficher sa camera au chargement d'OBS, sans
  // attendre qu'on ouvre ses proprietes.
  applyStream(d.get(), adoptSettings(d.get(), settings));
  return d.get();
}

void sourceDestroy(void* data) {
  auto* raw = static_cast<SourceData*>(data);
  if (!raw) return;

  // Ordre volontaire : on retire d'ABORD l'entrée `g_innerToSelf` (dans
  // `releaseInner`) et on relâche la référence de la table ENSUITE. Un frame tap
  // déjà entré tient sa propre référence forte : l'objet ne sera détruit qu'une
  // fois ce tap terminé, au lieu d'être supprimé sous ses pieds.
  std::shared_ptr<SourceData> owned = unregisterInstance(raw->self);
  releaseInner(raw);
  // `owned` retombe ici : destruction réelle si plus aucun tap ne la référence.
}

void sourceVideoTick(void* data, float /*seconds*/) {
  auto* d = static_cast<SourceData*>(data);
  if (!d) return;
  const uint64_t now = os_gettime_ns();
  watchTracks(d, now);
  const uint64_t lastFresh = d->lastFreshNs.load();
  const bool hasLiveFrame = lastFresh != 0 && (now - lastFresh) < 1500000000ULL;
  if (hasLiveFrame) return;  // le flux live alimente deja la source

  // Aucun flux (jamais connecte, ou coupe depuis >1.5s) : on maintient le
  // placeholder noir 1920x1080 pour garder la source dimensionnee. Throttle a
  // ~4 fps, largement suffisant pour une image fixe, et pousse UNIQUEMENT
  // quand il n'y a pas de flux => zero interference avec le flux live.
  if (now - d->lastPlaceholderNs.load() < 250000000ULL) return;
  d->lastPlaceholderNs.store(now);
  pushPlaceholderFrame(d);
}

void sourceUpdate(void* data, obs_data_t* settings) {
  auto* d = static_cast<SourceData*>(data);
  if (!d) return;
  // Cette fonction IGNORAIT son `obs_data_t*` et relisait le flux du poste :
  // toutes les instances affichaient donc la meme camera, et punaiser n'avait
  // aucun effet. C'est le defaut que le multicam venait heurter.
  const QString wanted = adoptSettings(d, settings);
  bool changed = false;
  {
    QMutexLocker lock(&d->mutex);
    if (d->selectedStreamId != wanted.toStdString()) changed = true;
  }
  if (changed) applyStream(d, wanted);
}

void sourceGetDefaults(obs_data_t* settings) {
  obs_data_set_default_string(settings, kStreamSettingKey, kStreamSettingAuto);
}

// Libelle d'un flux du registre : son nom, ou son identifiant a defaut. Un
// identifiant brut est illisible, mais il vaut mieux que rien : c'est le seul
// moyen de reconnaitre une entree dont le nom n'est pas encore arrive.
QString entryLabel(const StreamEntry& e) {
  return e.label.isEmpty() ? e.id : e.label;
}

obs_properties_t* sourceGetProperties(void* data) {
  auto* d = static_cast<SourceData*>(data);
  obs_properties_t* props = obs_properties_create();

  const QList<StreamEntry> entries = StreamRegistry::instance().snapshot();
  const QString active = StreamRegistry::instance().activeStreamId();

  QString activeLabel;
  for (const StreamEntry& e : entries) {
    if (e.id == active) {
      activeLabel = entryLabel(e);
      break;
    }
  }

  obs_property_t* list = obs_properties_add_list(
      props, kStreamSettingKey, locC("SourceProps.Stream"), OBS_COMBO_TYPE_LIST,
      OBS_COMBO_FORMAT_STRING);

  const QString autoLabel =
      activeLabel.isEmpty() ? loc("SourceProps.FollowStation")
                            : loc("SourceProps.FollowStationNamed").arg(activeLabel);
  obs_property_list_add_string(list, autoLabel.toUtf8().constData(), kStreamSettingAuto);

  for (const StreamEntry& e : entries) {
    obs_property_list_add_string(list, entryLabel(e).toUtf8().constData(),
                                 e.id.toUtf8().constData());
  }

  // Le flux punaise a disparu du registre (supprime, ou compte change d'espace).
  // Son entree est CONSERVEE dans la liste : sans elle, OBS afficherait la
  // premiere valeur venue, et rouvrir les proprietes suffirait a repunaiser la
  // source sur une autre camera sans que personne ne l'ait demande.
  std::string pinned;
  if (d) {
    QMutexLocker lock(&d->mutex);
    pinned = d->pinnedStreamId;
  }
  if (!pinned.empty()) {
    const QString pinnedId = QString::fromStdString(pinned);
    bool known = false;
    for (const StreamEntry& e : entries) {
      if (e.id == pinnedId) {
        known = true;
        break;
      }
    }
    if (!known) {
      const QString orphan = loc("SourceProps.StreamNotFound").arg(pinnedId);
      obs_property_list_add_string(list, orphan.toUtf8().constData(),
                                   pinnedId.toUtf8().constData());
    }
  }

  if (entries.isEmpty()) {
    obs_properties_add_text(
        props, "semirprod_info",
        locC("SourceProps.NoStreams"),
        OBS_TEXT_INFO);
  }

  return props;
}

obs_source_info g_info{};

} // namespace

bool isSourceInstanceReady(obs_source_t* source) {
  if (!source) return false;
  QMutexLocker lock(&g_instancesMutex);
  auto it = g_instances.constFind(source);
  if (it == g_instances.constEnd()) return false;
  const std::shared_ptr<SourceData>& d = it.value();
  if (!d->is_ready.load()) return false;
  // Exiger que la dernière image remonte à moins de 1.5s — sinon le
  // ffmpeg_source garde des dimensions cachées d'un flux disconnecté et on
  // tomberait sur du contenu stale après un cut/restart du publisher.
  uint64_t lastFresh = d->lastFreshNs.load();
  if (lastFresh == 0) return false;
  return (os_gettime_ns() - lastFresh) < 1500000000ULL;
}

std::string sourceInstanceStreamId(obs_source_t* source) {
  if (!source) return "";
  QMutexLocker lock(&g_instancesMutex);
  auto it = g_instances.constFind(source);
  if (it == g_instances.constEnd()) return "";
  QMutexLocker innerLock(&it.value()->mutex);
  return it.value()->selectedStreamId.c_str();
}

bool isStreamDelivering(const std::string& streamId) {
  if (streamId.empty()) return false;
  QMutexLocker lock(&g_instancesMutex);
  const uint64_t now = os_gettime_ns();
  for (const std::shared_ptr<SourceData>& d : g_instances) {
    if (!d->is_ready.load()) continue;
    const uint64_t lastFresh = d->lastFreshNs.load();
    if (lastFresh == 0 || (now - lastFresh) >= 1500000000ULL) continue;
    QMutexLocker innerLock(&d->mutex);
    if (d->selectedStreamId == streamId) return true;
  }
  return false;
}

InstanceState queryInstanceState() {
  InstanceState out;
  QMutexLocker lock(&g_instancesMutex);
  out.anyExists = !g_instances.isEmpty();
  const uint64_t now = os_gettime_ns();
  for (const std::shared_ptr<SourceData>& d : g_instances) {
    if (!d->is_ready.load()) continue;
    const uint64_t lastFresh = d->lastFreshNs.load();
    if (lastFresh == 0 || (now - lastFresh) >= 1500000000ULL) continue;
    out.anyReady = true;
    out.width = static_cast<int>(d->width.load());
    out.height = static_cast<int>(d->height.load());
    if (d->oversize.load()) {
      out.anyOversize = true;
      out.nativeWidth = static_cast<int>(d->nativeWidth.load());
      out.nativeHeight = static_cast<int>(d->nativeHeight.load());
    }
    break;
  }
  return out;
}

void reapplyActiveStreamToAllInstances() {
  QList<std::shared_ptr<SourceData>> targets;
  {
    QMutexLocker lock(&g_instancesMutex);
    for (const std::shared_ptr<SourceData>& d : g_instances) targets.push_back(d);
  }
  for (const std::shared_ptr<SourceData>& d : targets) {
    // Chaque instance resout SON flux : les punaisees gardent le leur, seules
    // celles restees en automatique suivent le poste. Avant, changer de flux
    // dans le panneau rebasculait toutes les sources de toutes les scenes.
    const QString wanted = resolveStreamId(d.get());
    bool needsApply = false;
    {
      QMutexLocker lock(&d->mutex);
      if (d->selectedStreamId != wanted.toStdString()) needsApply = true;
      // Meme flux, mais rien a l'ecran : l'URL de lecture manquait au moment de
      // l'application (registre pas encore arrive, camera creee apres la
      // source). Le registre vient de changer, on retente.
      if (!needsApply && d->inner == nullptr && !wanted.isEmpty()) needsApply = true;
    }
    if (needsApply) applyStream(d.get(), wanted);
  }
}

void kickStalledInstances(const std::string& streamId) {
  if (streamId.empty()) return;
  QList<std::shared_ptr<SourceData>> targets;
  {
    QMutexLocker lock(&g_instancesMutex);
    for (const std::shared_ptr<SourceData>& d : g_instances) targets.push_back(d);
  }
  const uint64_t now = os_gettime_ns();
  for (const std::shared_ptr<SourceData>& d : targets) {
    QMutexLocker applyLock(&d->applyMutex);
    obs_source_t* inner = nullptr;
    {
      QMutexLocker lock(&d->mutex);
      if (d->selectedStreamId != streamId || !d->inner) continue;
      // Deja en direct : ne rien rouvrir, ce serait couper l'image.
      const uint64_t last = d->lastFreshNs.load();
      if (last != 0 && now - last < 1500000000ULL) continue;
      inner = obs_source_get_ref(d->inner);
    }
    if (!inner) continue;
    // ffmpeg_source_update (source distante) arrete le fil de reconnexion et
    // rouvre tout de suite, sans attendre `reconnect_delay_sec`.
    d->openedNs.store(now);
    obs_source_update(inner, nullptr);
    obs_source_release(inner);
    blog(LOG_INFO, "[semirprod-live-source] publish seen, reopening streamId=%s",
         streamId.c_str());
  }
}

void registerSemirprodLiveSource() {
  registerFrameTapFilter();

  g_info = {};
  g_info.id = "semirprod_live_source";
  g_info.type = OBS_SOURCE_TYPE_INPUT;
  // ASYNC_VIDEO + AUDIO : exactement les flags de ffmpeg_source. OBS gère le
  // rendu (pick frame from queue) et la sync A/V via les PTS des frames qu'on
  // pousse via obs_source_output_video / obs_source_output_audio.
  g_info.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE;
  g_info.get_name = sourceGetName;
  g_info.create = sourceCreate;
  g_info.destroy = sourceDestroy;
  g_info.update = sourceUpdate;
  g_info.video_tick = sourceVideoTick;
  g_info.get_properties = sourceGetProperties;
  g_info.get_defaults = sourceGetDefaults;
  // Pas de get_width / get_height : inutiles pour une source ASYNC (OBS retourne
  // async_width/async_height). La taille est fixee par les frames poussees via
  // obs_source_output_video — flux live quand present, sinon placeholder
  // 1920x1080 pousse par sourceVideoTick.
  obs_register_source(&g_info);
  blog(LOG_INFO, "[semirprod-live-source] registered (ASYNC_VIDEO + AUDIO)");
}

} // namespace semirprod
