#include "ProgramPreviewOutput.hpp"

#include <obs-module.h>
#include <obs.h>

#include <QCoreApplication>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include "../Config.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>

namespace semirprod {

namespace {

// L'URL WHIP SANS son jeton. Avec SRS, le jeton voyage dans la requete
// (?app=...&stream=...&token=...) et change a chaque rafraichissement du bail :
// comparer l'URL brute faisait passer chaque rafraichissement pour un changement
// de destination, et la sortie etait coupee puis reconnectee toutes les quelques
// secondes (aperçu saccade, reconnexion visible). Le jeton ne sert qu'a la
// poignee de main WHIP : il n'identifie pas la destination.
QString whipIdentity(const QString& url) {
  QUrl u(url);
  QUrlQuery q(u);
  q.removeAllQueryItems(QStringLiteral("token"));
  u.setQuery(q);
  return u.toString();
}

// OBS's native WebRTC egress (plugins/obs-webrtc). The output pushes WHIP; the
// paired "whip_custom" service carries the endpoint URL + bearer token. Both
// ship with OBS 30+.
constexpr const char* kWhipOutputId = "whip_output";
constexpr const char* kWhipServiceId = "whip_custom";

// Always-present software fallback.
constexpr const char* kX264EncoderId = "obs_x264";

// WHIP, c'est du WebRTC : obs-webrtc DÉCLARE et PAQUÉTISE la piste audio en
// Opus (`audio_description.addOpusCodec(...)` + `rtc::OpusRtpPacketizer` dans
// whip-output.cpp). Y brancher un encodeur AAC ne lève aucune erreur — libobs
// n'oppose jamais la liste `encoded_audio_codecs` d'un output à l'encodeur
// qu'on lui donne, elle ne sert qu'aux getters de l'interface — mais le
// navigateur qui lit l'aperçu décode alors des trames AAC avec un décodeur
// Opus : voix de robot du premier échantillon au dernier, quelle que soit la
// qualité du son entrant. L'AAC reste le bon codec pour le direct RTMP ; il
// n'a rien à faire sur cette sortie.
constexpr const char* kOpusEncoderId = "ffmpeg_opus";

// Unique object names — libobs indexes some objects by name, so we avoid
// colliding with the main streaming output's encoders. Le second jeu sert au
// mix de préparation (mode studio) : les deux sorties tournent en même temps
// quand la régie affiche le double aperçu.
constexpr const char* kVideoEncoderName = "semirprod_preview_venc";
constexpr const char* kAudioEncoderName = "semirprod_preview_aenc";
constexpr const char* kOutputName = "semirprod_preview_whip";
constexpr const char* kServiceName = "semirprod_preview_service";

constexpr const char* kStudioVideoEncoderName = "semirprod_studio_venc";
constexpr const char* kStudioAudioEncoderName = "semirprod_studio_aenc";
constexpr const char* kStudioOutputName = "semirprod_studio_whip";
constexpr const char* kStudioServiceName = "semirprod_studio_service";

// Troisième jeu : l'aperçu d'un canvas nommé (édition régie), qui tourne en
// même temps que les deux autres.
constexpr const char* kCanvasVideoEncoderName = "semirprod_canvas_venc";
constexpr const char* kCanvasAudioEncoderName = "semirprod_canvas_aenc";
constexpr const char* kCanvasOutputName = "semirprod_canvas_whip";
constexpr const char* kCanvasServiceName = "semirprod_canvas_service";

// Libellés de journal par mix (anglais pour le démarrage, français ailleurs,
// comme les lignes existantes).
const char* mixLabelEn(ProgramPreviewOutput::Mix mix) {
  switch (mix) {
    case ProgramPreviewOutput::Mix::Program: return "program";
    case ProgramPreviewOutput::Mix::PreviewScene: return "studio-preview";
    case ProgramPreviewOutput::Mix::Canvas: return "canvas";
  }
  return "?";
}

const char* mixLabelFr(ProgramPreviewOutput::Mix mix) {
  switch (mix) {
    case ProgramPreviewOutput::Mix::Program: return "programme";
    case ProgramPreviewOutput::Mix::PreviewScene: return "preparation";
    case ProgramPreviewOutput::Mix::Canvas: return "canvas";
  }
  return "?";
}

// Numéro de génération, ajouté au nom de chaque objet construit.
//
// `obs_output_stop()` est GRACIEUX, donc asynchrone : au retour de teardown(),
// l'ancienne sortie vit encore sur son propre compteur de références et tient
// toujours ses encodeurs. Reconstruire aussitôt sous les MÊMES noms faisait
// cohabiter deux générations homonymes, et libobs le disait avant de mourir :
//
//   obs_service_deactivate: service 'semirprod_preview_service' is not
//                           assigned to an output
//   obs_encoder_set_group:  encoder 'semirprod_preview_venc' is already active
//
// (journal du 13 août, 21:43:32 — crash d'OBS sur deux `start_program_preview`
// reçus dans la même milliseconde.) La mourante détachait ce que la nouvelle
// venait d'attacher.
//
// Un numéro par génération rend toute collision impossible : la sortie qui
// s'éteint garde ses objets, la nouvelle a les siens, et elles s'ignorent.
// L'alternative — attendre l'arrêt réel avant de reconstruire — bloquerait le
// fil d'interface, ce qu'on refuse (cf. WebUiWindow et l'attente de CEF).
std::atomic<unsigned> g_generation{0};

std::string generationalName(const char* base, unsigned gen) {
  return std::string(base) + "_g" + std::to_string(gen);
}

// ---------------------------------------------------------------------------
// Relache DIFFEREE d'une generation qui s'eteint.
//
// C'est la correction du crash du 15 aout, et elle est prouvee au niveau du
// source de libobs. `obs_output_stop()` est asynchrone : au retour, la sortie
// n'est pas arretee, et ses encodeurs sont ENCORE ACTIFS. L'ancien teardown
// relachait les quatre objets dans la foulee ; quand notre reference etait la
// derniere — ce qui arrive des que la sortie n'a jamais atteint l'etat actif,
// typiquement une session WHIP encore en negociation — libobs appelait
// `obs_encoder_destroy()` sur un encodeur actif :
//
//   libobs/obs-encoder.c:431   obs_encoder_set_group(encoder, NULL);
//   libobs/obs-encoder.c:2067  "obs_encoder_set_group: encoder '%s' is already
//                               active"
//
// Ce message dans le journal d'OBS n'est donc pas un avertissement anodin :
// c'est la signature d'une destruction d'encodeur vivant. Juste apres,
// `obs_encoder_destroy` libere les tampons audio, appelle `info.destroy()` sur
// la session NVENC et detruit les mutex, pendant que le fil d'encodage s'en
// sert encore.
//
// On garde donc les quatre objets vivants jusqu'a l'arret REEL de leur sortie,
// signale par libobs. Pas d'attente bloquante : le fil d'interface repart
// immediatement, et la generation suivante porte d'autres noms (voir
// g_generation), donc les deux ne se croisent jamais.
struct DyingGeneration {
  obs_output_t* output = nullptr;
  obs_encoder_t* venc = nullptr;
  obs_encoder_t* aenc = nullptr;
  obs_service_t* service = nullptr;
};

// UN SEUL proprietaire, UN SEUL destructeur.
//
// La premiere version s'abonnait au signal « stop » de la sortie ET armait un
// minuteur de secours, et les DEUX chemins faisaient `delete g`. Le second a
// s'executer lisait donc une structure deja liberee, en tirait un pointeur
// d'output invalide et le passait a libobs :
//
//   obs.dll!obs_output_get_signal_handler+0x2f
//   semirprod-studio.dll!releaseDyingGeneration+0x49   <- depuis le QTimer
//
// (violation d'acces du 15 aout, 19:04:08.) Une garde atomique DANS la
// structure n'y changeait rien : la lire, c'est deja lire de la memoire
// liberee.
//
// Le signal est donc supprime, pas repare. Le minuteur devient l'unique
// proprietaire : il se declenche toujours, il relache, il detruit. On perd la
// liberation « au plus tot » — les objets d'une generation morte vivent
// jusqu'a kDyingGenerationGraceMs — et c'est sans consequence, puisque chaque
// generation porte des noms distincts (voir g_generation) et ne peut donc
// jamais entrer en collision avec la suivante.
void releaseDyingGeneration(DyingGeneration* g) {
  if (!g) return;
  if (g->output) obs_output_release(g->output);
  if (g->venc) obs_encoder_release(g->venc);
  if (g->aenc) obs_encoder_release(g->aenc);
  if (g->service) obs_service_release(g->service);
  delete g;
}

// Delai avant de relacher une generation eteinte. `obs_output_stop()` est
// asynchrone : relacher trop tot detruit un encodeur encore actif et fait
// tomber OBS (c'est tout l'objet de ce mecanisme). Genereux a dessein — mieux
// vaut tenir une generation morte quelques secondes de trop.
//
// Declare dans l'en-tete : les appelants qui alimentent une sortie doivent
// caler la survie de leur mix dessus.
constexpr int kDyingGenerationGraceMs = ProgramPreviewOutput::kRetireGraceMs;

void retireGeneration(obs_output_t* output, obs_encoder_t* venc,
                      obs_encoder_t* aenc, obs_service_t* service, bool force) {
  if (!output && !venc && !aenc && !service) return;

  auto* g = new DyingGeneration();
  g->output = output;
  g->venc = venc;
  g->aenc = aenc;
  g->service = service;

  // FERMETURE D'OBS : relache SYNCHRONE, surtout pas differee.
  //
  // Differer ici bloquait `obs_shutdown()`. La boucle d'evenements Qt s'eteint
  // avant nous : ni le signal « stop » marshalle vers elle, ni le filet de
  // securite QTimer ne se declenchent jamais. Nos references sur la sortie et
  // ses encodeurs n'etaient donc PLUS JAMAIS relachees, et libobs attendait
  // indefiniment de pouvoir les detruire. Symptome : OBS reste en vie apres la
  // disparition de sa fenetre, journal fige sur « Freeing OBS context data »
  // (constate le 15 aout, ~90 s sans fin, avec un apercu WHIP actif).
  //
  // C'est le comportement d'avant la relache differee, et il est sain ICI :
  // OBS s'arrete, plus personne ne va encoder derriere nous. La relache
  // differee reste indispensable pour la reconstruction A CHAUD, ou detruire
  // un encodeur encore actif fait tomber OBS.
  if (force) {
    // On n'attend pas la cloture reseau du WHIP (DELETE HTTP + ICE/DTLS), qui
    // trainait plusieurs secondes. MediaMTX expire la session de son cote.
    if (output) obs_output_force_stop(output);
    releaseDyingGeneration(g);
    return;
  }

  if (output) obs_output_stop(output);

  // Le minuteur est l'UNIQUE proprietaire de `g` : lui seul relache, lui seul
  // detruit. Aucun autre chemin ne doit toucher a cette structure — c'est la
  // double propriete qui avait produit l'usage apres liberation.
  //
  // Sans qApp il n'y a pas de boucle pour le porter : on relache tout de suite
  // plutot que de tenir des references qu'aucun minuteur ne viendra jamais
  // liberer — c'est exactement ce qui bloquait obs_shutdown().
  if (qApp) {
    QTimer::singleShot(kDyingGenerationGraceMs, qApp,
                       [g]() { releaseDyingGeneration(g); });
  } else {
    releaseDyingGeneration(g);
  }
}

bool encoderTypeExists(const char* needle) {
  const char* id = nullptr;
  for (size_t i = 0; obs_enum_encoder_types(i, &id); ++i) {
    if (id && needle && std::strcmp(id, needle) == 0) return true;
  }
  return false;
}

// Pick the best available H.264 encoder: hardware first (NVENC / QSV / AMF),
// then the x264 software fallback. Returns the encoder id and, via `isX264`,
// whether it's the software path (so the caller only sets x264-specific keys
// like preset/tune on it). Both the modern and legacy ids are probed since the
// exact id drifts across OBS/driver versions.
const char* pickVideoEncoderId(bool* isX264) {
  static const char* const kHardwareCandidates[] = {
      "obs_nvenc_h264",    // NVENC H.264 (OBS 31+, réécriture native)
      "jim_nvenc",         // NVENC H.264 (OBS 28-30)
      "ffmpeg_nvenc",      // NVENC H.264 (legacy id)
      "obs_qsv11_v2",      // Intel QSV H.264 (OBS 30+)
      "obs_qsv11",         // Intel QSV H.264 (legacy id)
      "h264_texture_amf",  // AMD AMF H.264 (OBS 28+)
      "amd_amf_h264",      // AMD AMF H.264 (legacy id)
  };
  for (const char* id : kHardwareCandidates) {
    if (encoderTypeExists(id)) {
      *isX264 = false;
      return id;
    }
  }
  *isX264 = true;
  return kX264EncoderId;
}

} // namespace

ProgramPreviewOutput::ProgramPreviewOutput(Mix mix, QObject* parent)
    : QObject(parent), mix_(mix) {}

void ProgramPreviewOutput::setVideoMix(video_t* video) {
  externalVideo_ = video;
}

video_t* ProgramPreviewOutput::videoMix() const {
  return mix_ == Mix::Program ? obs_get_video() : externalVideo_;
}

ProgramPreviewOutput::~ProgramPreviewOutput() {
  teardown();
}

bool ProgramPreviewOutput::isSupported() {
  const char* id = nullptr;
  for (size_t i = 0; obs_enum_output_types(i, &id); ++i) {
    if (id && std::strcmp(id, kWhipOutputId) == 0) return true;
  }
  return false;
}

void ProgramPreviewOutput::clampEncodeParams(int* width, int* height, int* bitrateKbps) {
  constexpr int kMaxLongSide = 1920;
  constexpr int kMaxShortSide = 1080;
  constexpr int kMaxBitrateKbps = 6000;
  // Portrait (9:16) ou paysage : le plafond suit l'orientation demandee, pour
  // ne pas ecraser un apercu vertical 1080x1920 en 1080x1080.
  const bool portrait = *height > *width;
  const int maxW = portrait ? kMaxShortSide : kMaxLongSide;
  const int maxH = portrait ? kMaxLongSide : kMaxShortSide;
  if (*width > 0 && *height > 0) {
    // Reduction PROPORTIONNELLE : un canvas 3840x1600 devient 1920x800, pas
    // une image deformee en 1920x1080.
    const double echelle = std::min({1.0, static_cast<double>(maxW) / *width,
                                     static_cast<double>(maxH) / *height});
    if (echelle < 1.0) {
      *width = static_cast<int>(*width * echelle);
      *height = static_cast<int>(*height * echelle);
    }
  } else {
    if (*width > maxW) *width = maxW;
    if (*height > maxH) *height = maxH;
  }
  if (*bitrateKbps > kMaxBitrateKbps) *bitrateKbps = kMaxBitrateKbps;
}

bool ProgramPreviewOutput::start(const QString& whipUrl,
                                 const QString& bearerToken, int width,
                                 int height, int fps, int bitrateKbps) {
  if (!isSupported()) {
    blog(LOG_WARNING,
         "[semirprod-preview] whip_output type unavailable — OBS too old for "
         "WebRTC program preview");
    return false;
  }
  if (whipUrl.isEmpty()) {
    blog(LOG_WARNING, "[semirprod-preview] start() ignored — empty WHIP URL");
    return false;
  }

  // Defense en profondeur : LiveClient valide deja `whipUrl` avant d emettre le
  // signal, mais cette URL finit litteralement dans obs_data_set_string("server")
  // et emporte le programme complet. Le controle est re-applique ici pour qu un
  // futur appelant ne puisse pas contourner la garde par inadvertance.
  if (QUrl(whipUrl).scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) != 0 ||
      !Config::isAllowedHostFor(whipUrl, Config::HostUse::Whip)) {
    blog(LOG_ERROR,
         "[semirprod-preview] start() REFUSE — hote « %s » hors allowlist "
         "(schema « %s »). Autorises : %s.",
         QUrl(whipUrl).host().toUtf8().constData(),
         QUrl(whipUrl).scheme().toUtf8().constData(),
         Config::allowedHostsForLog().toUtf8().constData());
    return false;
  }

  // Defensive clamps — the encoder rejects odd dimensions and absurd values.
  // Plafonds re-appliques ici : la taille d'un apercu de canvas est deduite du
  // canvas par le dock APRES le controle de LiveClient, et un canvas 4K
  // donnerait sinon un encodage 4K.
  clampEncodeParams(&width, &height, &bitrateKbps);
  const int w = width > 0 ? (width & ~1) : 1280;
  const int h = height > 0 ? (height & ~1) : 720;
  const int bitrate = bitrateKbps > 0 ? bitrateKbps : 2500;
  // fps is intentionally not applied per-encoder: libobs encoders inherit the
  // global OBS video framerate, and there is no portable way to force a lower
  // fps on a single encoder across the OBS 30.x line (obs_encoder_set_frame_
  // rate_divisor only lands in 30.1+ and requires an integer divisor). The 1 s
  // keyframe interval keeps WHEP latency bounded regardless. Accepted here so
  // the protocol field is honoured at the wire even if unused.
  (void)fps;

  // Lease refreshes arrive every few seconds with the same parameters. A full
  // teardown+rebuild here would drop the WHIP session — MediaMTX closes the
  // path and every WHEP viewer sees "connection lost" — so a refresh that
  // changes nothing must be a no-op. The bearer token is deliberately NOT
  // compared: it only matters at the WHIP handshake, and the dashboard mints a
  // fresh one per lease; a token-only change on a live session is irrelevant.
  // `obs_output_active()` ne fait PLUS partie de la condition, et c'est le
  // second correctif du crash du 15 aout. Une sortie WHIP n'est pas « active »
  // pendant toute sa negociation WebRTC (ICE puis DTLS, quelques centaines de
  // millisecondes) : un message recu dans cette fenetre — un doublon du
  // dashboard, ou simplement un rafraichissement rapproche — echouait donc la
  // garde, passait pour un changement de parametres, et declenchait un
  // teardown sur une session en pleine connexion.
  //
  // La liveness vient desormais du signal « stop » de la sortie
  // (handleLiveOutputStopped) : tant qu'il n'est pas arrive, la sortie compte
  // comme vivante, qu'elle soit connectee ou encore en train de l'etre.
  if (running_ && output_ && whipIdentity(whipUrl) == whipIdentity(whipUrl_) && w == width_ &&
      h == height_ && bitrate == bitrateKbps_ && videoMix() == builtVideo_) {
    blog(LOG_DEBUG,
         "[semirprod-preview] lease refresh with identical params — output "
         "kept alive");
    return true;
  }

  // Parameters changed, output died, or first start: rebuild from scratch so
  // the new parameters take effect and we never leak a prior output.
  teardown();

  if (!videoMix() || !obs_get_audio()) {
    blog(LOG_ERROR,
         "[semirprod-preview] mix video (%s) ou audio absent — demarrage "
         "impossible",
         mixLabelFr(mix_));
    return false;
  }

  // Hardware d'abord (NVENC/QSV/AMF) : l'ASIC dédié encode l'aperçu sans voler
  // de CPU à la diffusion. Fallback x264 (veryfast, 720p bas débit ≈ gratuit)
  // si le hardware échoue — que ce soit à la CRÉATION (driver absent) ou au
  // DÉMARRAGE de l'output (l'init réelle de l'encodeur n'a lieu que là :
  // sessions NVENC épuisées, driver à moitié mort…). Couvre tous les PC.
  bool isX264 = false;
  const char* venc_id = pickVideoEncoderId(&isX264);

  bool ok = buildAndStart(venc_id, isX264, whipUrl, bearerToken, w, h, bitrate);
  if (!ok && !isX264) {
    blog(LOG_WARNING,
         "[semirprod-preview] hardware encoder %s failed — retrying with x264",
         venc_id);
    teardown();
    isX264 = true;
    venc_id = kX264EncoderId;
    ok = buildAndStart(venc_id, true, whipUrl, bearerToken, w, h, bitrate);
  }
  if (!ok) {
    teardown();
    return false;
  }

  // S'abonner a la mort de CETTE sortie. Sans cet abonnement, `running_`
  // resterait vrai apres une coupure reseau ou un refus de MediaMTX, et la
  // garde d'idempotence ci-dessus garderait indefiniment une sortie morte
  // « vivante » — le bail se rafraichirait dans le vide.
  if (signal_handler_t* sh = obs_output_get_signal_handler(output_)) {
    signal_handler_connect(sh, "stop", onLiveOutputStoppedSignal, this);
  }

  running_ = true;
  whipUrl_ = whipUrl;
  builtVideo_ = videoMix();
  width_ = w;
  height_ = h;
  bitrateKbps_ = bitrate;
  blog(LOG_INFO,
       "[semirprod-preview] WHIP %s preview started (%dx%d @ %d kbps, "
       "encoder=%s)",
       mixLabelEn(mix_), w, h, bitrate,
       venc_id);
  return true;
}

bool ProgramPreviewOutput::buildAndStart(const char* vencId, bool isX264,
                                         const QString& whipUrl,
                                         const QString& bearerToken, int w,
                                         int h, int bitrate) {
  video_t* video = videoMix();
  audio_t* audio = obs_get_audio();
  if (!video) return false;

  // Une génération par construction : voir g_generation. Les quatre objets de
  // ce démarrage la portent, donc aucun ne peut entrer en collision avec ceux
  // d'une sortie encore en train de s'éteindre.
  const unsigned gen = ++g_generation;
  const bool studio = mix_ == Mix::PreviewScene;
  const bool canvas = mix_ == Mix::Canvas;
  const std::string vencName = generationalName(
      canvas ? kCanvasVideoEncoderName : studio ? kStudioVideoEncoderName : kVideoEncoderName,
      gen);
  const std::string aencName = generationalName(
      canvas ? kCanvasAudioEncoderName : studio ? kStudioAudioEncoderName : kAudioEncoderName,
      gen);
  const std::string serviceName = generationalName(
      canvas ? kCanvasServiceName : studio ? kStudioServiceName : kServiceName, gen);
  const std::string outputName = generationalName(
      canvas ? kCanvasOutputName : studio ? kStudioOutputName : kOutputName, gen);

  // --- Video encoder (dedicated, low-bitrate, keyframe-per-second) ---------
  // Les quirks qui causaient l'écran gris navigateur sont neutralisés sur TOUS
  // les chemins : WebRTC ne décode ni les B-frames ni CABAC (profils main/
  // high), et un lecteur WHEP qui rejoint en cours de route a besoin des
  // SPS/PPS répétés à chaque keyframe — d'où baseline strict + bf=0 +
  // repeat_headers. Les clés inconnues d'un encodeur sont ignorées ; une
  // valeur d'enum invalide retombe sur le défaut de l'encodeur. Ça permet de
  // poser les knobs des trois vendeurs sans sonder les propriétés.
  obs_data_t* vsettings = obs_data_create();
  obs_data_set_int(vsettings, "bitrate", bitrate);
  obs_data_set_string(vsettings, "rate_control", "CBR");
  // 1 s GOP keeps WHEP join latency low (a viewer can decode within a second).
  obs_data_set_int(vsettings, "keyint_sec", 1);
  obs_data_set_bool(vsettings, "repeat_headers", true);
  obs_data_set_string(vsettings, "profile", "baseline");
  if (isX264) {
    // veryfast + zerolatency : léger même sur un CPU modeste à 720p bas débit
    // (zerolatency coupe aussi les B-frames et le lookahead).
    obs_data_set_string(vsettings, "preset", "veryfast");
    obs_data_set_string(vsettings, "tune", "zerolatency");
  } else {
    // Coupe explicite des B-frames ("bf" = NVENC/AMF, "bframes" = QSV) et
    // preset le plus rapide de chaque vendeur pour minimiser la charge GPU à
    // côté de l'encodeur de diffusion : "p1" (NVENC preset2), "speed" (AMF),
    // "TU7"/"speed" (QSV target_usage selon la version).
    obs_data_set_int(vsettings, "bf", 0);
    obs_data_set_int(vsettings, "bframes", 0);
    obs_data_set_string(vsettings, "preset2", "p1");
    obs_data_set_string(vsettings, "preset", "speed");
    obs_data_set_string(vsettings, "target_usage", "TU7");
  }

  videoEncoder_ =
      obs_video_encoder_create(
      vencId, vencName.c_str(), vsettings, nullptr);
  obs_data_release(vsettings);
  if (!videoEncoder_) {
    blog(LOG_ERROR, "[semirprod-preview] failed to create video encoder (%s)",
         vencId);
    return false;
  }
  // Scale the program canvas down to the preview size on the encoder — the
  // main pipeline resolution is untouched.
  obs_encoder_set_scaled_size(videoEncoder_, static_cast<uint32_t>(w),
                              static_cast<uint32_t>(h));
  // Les encodeurs texture (NVENC/AMF) ne savent pas scaler côté CPU : sans
  // scaling GPU, libobs rapatrie chaque frame en RAM pour la réduire, ce qui
  // annule le zéro-copie. Ce setter (OBS 30+) garde le downscale 720p sur le
  // GPU. Sans effet sur les encodeurs non-texture (x264).
  obs_encoder_set_gpu_scale_type(videoEncoder_, OBS_SCALE_BILINEAR);
  obs_encoder_set_video(videoEncoder_, video);

  // --- Audio encoder (Opus 128k — the dashboard VU-meter needs audio) ------
  //
  // Pas de repli sur un autre codec si Opus manque : ce serait re-livrer la
  // voix de robot. Tout OBS qui fournit `whip_output` fournit `ffmpeg_opus`
  // (les deux sont arrivés ensemble), donc l'absence signale une installation
  // abîmée — on le dit et on renonce à l'aperçu plutôt que d'émettre un son
  // indécodable.
  if (!encoderTypeExists(kOpusEncoderId)) {
    blog(LOG_ERROR,
         "[semirprod-preview] encodeur Opus (%s) absent de cet OBS : l'apercu "
         "exige Opus (WHIP est du WebRTC), apercu non demarre",
         kOpusEncoderId);
    return false;
  }
  obs_data_t* asettings = obs_data_create();
  obs_data_set_int(asettings, "bitrate", 128);
  audioEncoder_ = obs_audio_encoder_create(
      kOpusEncoderId, aencName.c_str(), asettings, 0, nullptr);
  obs_data_release(asettings);
  if (!audioEncoder_) {
    blog(LOG_ERROR, "[semirprod-preview] failed to create Opus audio encoder");
    return false;
  }
  obs_encoder_set_audio(audioEncoder_, audio);

  // --- WHIP service (endpoint URL + bearer token) --------------------------
  obs_data_t* ssettings = obs_data_create();
  obs_data_set_string(ssettings, "server", whipUrl.toUtf8().constData());
  obs_data_set_string(ssettings, "bearer_token",
                      bearerToken.toUtf8().constData());
  service_ = obs_service_create(
      kWhipServiceId, serviceName.c_str(), ssettings, nullptr);
  obs_data_release(ssettings);
  if (!service_) {
    blog(LOG_ERROR, "[semirprod-preview] failed to create WHIP service");
    return false;
  }

  // --- Output --------------------------------------------------------------
  output_ = obs_output_create(
      kWhipOutputId, outputName.c_str(), nullptr, nullptr);
  if (!output_) {
    blog(LOG_ERROR, "[semirprod-preview] failed to create whip_output");
    return false;
  }
  obs_output_set_video_encoder(output_, videoEncoder_);
  obs_output_set_audio_encoder(output_, audioEncoder_, 0);
  obs_output_set_service(output_, service_);

  // C'est ici que l'encodeur s'initialise réellement (obs_output_start fait
  // l'init des encodeurs en synchrone) — un NVENC à court de sessions échoue
  // ICI, pas à la création. false → le caller retente en x264.
  if (!obs_output_start(output_)) {
    const char* err = obs_output_get_last_error(output_);
    blog(LOG_ERROR, "[semirprod-preview] obs_output_start failed (%s): %s",
         vencId, err ? err : "(no error message)");
    return false;
  }
  return true;
}

// Signal libobs — arrive depuis le fil de la sortie, jamais depuis Qt.
void ProgramPreviewOutput::onLiveOutputStoppedSignal(void* data,
                                                     calldata_t* /*cd*/) {
  auto* self = static_cast<ProgramPreviewOutput*>(data);
  if (!self) return;
  // Marshalling vers le fil Qt : `self` n'est manipule que la, ce qui evite
  // d'avoir a verrouiller running_/output_ contre le fil de la sortie.
  QMetaObject::invokeMethod(
      self, [self]() { self->handleLiveOutputStopped(); },
      Qt::QueuedConnection);
}

void ProgramPreviewOutput::handleLiveOutputStopped() {
  if (!running_) return;  // deja demontee de notre cote
  blog(LOG_INFO,
       "[semirprod-preview] la sortie WHIP %s s'est arretee d'elle-meme — "
       "prochain rafraichissement de bail reconstruira",
       mixLabelFr(mix_));
  // On ne relache rien ici : teardown() cede les objets a la generation
  // mourante, qui sait quand les detruire. On note juste que la sortie n'est
  // plus vivante, pour que la garde d'idempotence cesse de la croire en vie.
  running_ = false;
}

void ProgramPreviewOutput::stop(bool force) {
  if (!running_ && !output_) return;
  blog(LOG_INFO, "[semirprod-preview] stopping WHIP program preview%s",
       force ? " (forced)" : "");
  teardown(force);
}

void ProgramPreviewOutput::teardown(bool force) {
  // On ne relache PLUS rien ici. `obs_output_stop()` est asynchrone : au
  // retour, les encodeurs de cette generation sont encore actifs, et les
  // detruire fait tomber OBS (voir retireGeneration, qui documente la preuve
  // dans le source de libobs). On cede donc les quatre objets a la generation
  // mourante, qui les relachera quand la sortie aura reellement fini.
  if (output_) {
    // Notre abonnement « la sortie vivante s'est arretee » n'a plus lieu
    // d'etre : c'est nous qui l'arretons, et la generation mourante prend le
    // relais avec son propre abonnement.
    if (signal_handler_t* sh = obs_output_get_signal_handler(output_)) {
      signal_handler_disconnect(sh, "stop", onLiveOutputStoppedSignal, this);
    }
  }
  retireGeneration(output_, videoEncoder_, audioEncoder_, service_, force);
  output_ = nullptr;
  videoEncoder_ = nullptr;
  audioEncoder_ = nullptr;
  service_ = nullptr;
  running_ = false;
  builtVideo_ = nullptr;
  whipUrl_.clear();
  width_ = 0;
  height_ = 0;
  bitrateKbps_ = 0;
}

} // namespace semirprod
