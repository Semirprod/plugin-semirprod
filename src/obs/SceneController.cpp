#include "SceneController.hpp"

#include "SemirprodLiveSource.hpp"
#include "../Edition.hpp"
#include "../Locale.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <obs.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <util/config-file.h>

#include <QBuffer>
#include <QCoreApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QMetaObject>
#include <QPointer>
#include <QSet>
#include <QThreadPool>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace semirprod {

namespace {

struct SourcePresenceCtx {
  QString needle;
  bool found;
};

bool scenePresenceEnum(obs_scene_t* /*scene*/, obs_sceneitem_t* item, void* param) {
  auto* ctx = static_cast<SourcePresenceCtx*>(param);
  obs_source_t* itemSource = obs_sceneitem_get_source(item);
  if (!itemSource) return true;
  if (QString::fromUtf8(obs_source_get_name(itemSource)) == ctx->needle) {
    ctx->found = true;
    return false; // stop enumeration
  }
  return true;
}

obs_source_t* findSceneSourceByName(const QString& sceneName) {
  obs_source_t* match = nullptr;
  obs_frontend_source_list sourceList{};
  obs_frontend_get_scenes(&sourceList);
  for (size_t i = 0; i < sourceList.sources.num; ++i) {
    obs_source_t* source = sourceList.sources.array[i];
    if (!source) continue;
    if (QString::fromUtf8(obs_source_get_name(source)) == sceneName) {
      match = obs_source_get_ref(source);
      break;
    }
  }
  obs_frontend_source_list_free(&sourceList);
  return match;
}

} // namespace

SceneController::SceneController(QObject* parent) : QObject(parent) {}

QStringList SceneController::availableScenes() const {
  QStringList scenes;
  obs_frontend_source_list sourceList{};
  obs_frontend_get_scenes(&sourceList);
  for (size_t i = 0; i < sourceList.sources.num; ++i) {
    obs_source_t* source = sourceList.sources.array[i];
    if (!source) continue;
    scenes.push_back(QString::fromUtf8(obs_source_get_name(source)));
  }
  obs_frontend_source_list_free(&sourceList);
  scenes.removeDuplicates();
  return scenes;
}

QString SceneController::currentScene() const {
  obs_source_t* source = obs_frontend_get_current_scene();
  if (!source) {
    return {};
  }

  const QString name = QString::fromUtf8(obs_source_get_name(source));
  obs_source_release(source);
  return name;
}

bool SceneController::sceneExists(const QString& sceneName) const {
  return availableScenes().contains(sceneName);
}

bool SceneController::switchToScene(const QString& sceneName) {
  obs_source_t* target = findSceneSourceByName(sceneName);
  if (!target) {
    // Silent failure used to mask the most common misconfiguration : a
    // sceneLive/sceneBug saved in plugin settings that no longer matches a
    // real OBS scene name (renamed, deleted, fresh install with different
    // defaults). Surface it in obs-log.txt so support can catch it without
    // forensic guesswork.
    blog(LOG_WARNING,
         "[semirprod] switchToScene(\"%s\") ignored — scene not found in OBS",
         sceneName.toUtf8().constData());
    return false;
  }

  // In studio (preview/program) mode, obs_frontend_set_current_scene only
  // moves the PREVIEW pane. To actually flip the broadcast we also need to
  // trigger the configured transition. In regular mode, set_current_scene
  // switches the Program scene directly and the transition call is a no-op.
  if (obs_frontend_preview_program_mode_active()) {
    obs_frontend_set_current_preview_scene(target);
    obs_frontend_preview_program_trigger_transition();
  } else {
    obs_frontend_set_current_scene(target);
  }
  obs_source_release(target);
  return true;
}

namespace {

// obs_frontend_get_profiles / _get_scene_collections rendent un char** alloué
// par OBS, terminé par un pointeur nul, à libérer avec bfree.
QStringList takeFrontendNameList(char** raw) {
  QStringList names;
  if (!raw) return names;
  for (char** cursor = raw; *cursor; ++cursor) {
    names.push_back(QString::fromUtf8(*cursor));
  }
  bfree(raw);
  names.removeDuplicates();
  return names;
}

QString takeFrontendName(char* raw) {
  if (!raw) return {};
  const QString name = QString::fromUtf8(raw);
  bfree(raw);
  return name;
}

} // namespace

QStringList SceneController::availableProfiles() const {
  return takeFrontendNameList(obs_frontend_get_profiles());
}

QString SceneController::currentProfile() const {
  return takeFrontendName(obs_frontend_get_current_profile());
}

bool SceneController::switchToProfile(const QString& profileName) {
  if (profileName.isEmpty()) return false;
  if (!availableProfiles().contains(profileName)) {
    // Pas de création implicite : OBS créerait un profil vide sur simple appel
    // avec un nom inconnu dans certaines versions. On refuse plutôt que de
    // laisser le dashboard fabriquer de la configuration sur le poste.
    blog(LOG_WARNING,
         "[semirprod] switchToProfile(\"%s\") refuse — profil inconnu",
         profileName.toUtf8().constData());
    return false;
  }
  if (isStreamingActive() || isRecordingActive()) {
    blog(LOG_WARNING,
         "[semirprod] switchToProfile(\"%s\") refuse — sortie active",
         profileName.toUtf8().constData());
    return false;
  }
  if (profileName == currentProfile()) return true;
  obs_frontend_set_current_profile(profileName.toUtf8().constData());
  return true;
}

QStringList SceneController::availableSceneCollections() const {
  return takeFrontendNameList(obs_frontend_get_scene_collections());
}

QString SceneController::currentSceneCollection() const {
  return takeFrontendName(obs_frontend_get_current_scene_collection());
}

bool SceneController::switchToSceneCollection(const QString& collectionName) {
  if (collectionName.isEmpty()) return false;
  if (!availableSceneCollections().contains(collectionName)) {
    blog(LOG_WARNING,
         "[semirprod] switchToSceneCollection(\"%s\") refuse — collection inconnue",
         collectionName.toUtf8().constData());
    return false;
  }
  if (isStreamingActive() || isRecordingActive()) {
    blog(LOG_WARNING,
         "[semirprod] switchToSceneCollection(\"%s\") refuse — sortie active",
         collectionName.toUtf8().constData());
    return false;
  }
  if (collectionName == currentSceneCollection()) return true;
  obs_frontend_set_current_scene_collection(collectionName.toUtf8().constData());
  return true;
}

bool SceneController::createSceneCollection(const QString& collectionName,
                                            QString* reason) {
  const QString name = collectionName.trimmed();
  if (name.isEmpty() || name.size() > 64) {
    if (reason) *reason = QStringLiteral("collection_invalid");
    return false;
  }
  if (availableSceneCollections().contains(name)) {
    if (reason) *reason = QStringLiteral("collection_exists");
    return false;
  }
  if (isStreamingActive() || isRecordingActive()) {
    if (reason) *reason = QStringLiteral("output_active");
    return false;
  }
  if (!obs_frontend_add_scene_collection(name.toUtf8().constData())) {
    if (reason) *reason = QStringLiteral("collection_failed");
    return false;
  }
  return true;
}

bool SceneController::isStudioModeEnabled() const {
  return obs_frontend_preview_program_mode_active();
}

bool SceneController::setStudioMode(bool enabled) {
  if (obs_frontend_preview_program_mode_active() == enabled) return true;
  obs_frontend_set_preview_program_mode(enabled);
  return obs_frontend_preview_program_mode_active() == enabled;
}

QString SceneController::previewScene() const {
  if (!obs_frontend_preview_program_mode_active()) return {};
  obs_source_t* source = obs_frontend_get_current_preview_scene();
  if (!source) return {};
  const QString name = QString::fromUtf8(obs_source_get_name(source));
  obs_source_release(source);
  return name;
}

bool SceneController::setPreviewScene(const QString& sceneName) {
  if (!obs_frontend_preview_program_mode_active()) {
    blog(LOG_WARNING,
         "[semirprod] setPreviewScene(\"%s\") ignore — mode studio inactif",
         sceneName.toUtf8().constData());
    return false;
  }
  obs_source_t* target = findSceneSourceByName(sceneName);
  if (!target) {
    blog(LOG_WARNING,
         "[semirprod] setPreviewScene(\"%s\") ignore — scene introuvable",
         sceneName.toUtf8().constData());
    return false;
  }
  obs_frontend_set_current_preview_scene(target);
  obs_source_release(target);
  return true;
}

bool SceneController::triggerStudioTransition() {
  if (!obs_frontend_preview_program_mode_active()) return false;
  obs_frontend_preview_program_trigger_transition();
  return true;
}

bool SceneController::isStreamingActive() const {
  return obs_frontend_streaming_active();
}

bool SceneController::isRecordingActive() const {
  return obs_frontend_recording_active();
}

OutputStats SceneController::collectOutputStats() const {
  OutputStats stats;

  // obs_frontend_get_streaming_output() returns a NEW reference — release it
  // before returning on every path.
  obs_output_t* output = obs_frontend_get_streaming_output();
  if (!output) return stats;
  if (!obs_output_active(output)) {
    obs_output_release(output);
    return stats;
  }

  stats.active = true;
  stats.totalBytes = obs_output_get_total_bytes(output);
  stats.droppedFrames = obs_output_get_frames_dropped(output);
  stats.totalFrames = obs_output_get_total_frames(output);
  stats.congestion = obs_output_get_congestion(output);
  obs_output_release(output);

  // Encoder-lag globals — indépendants de l'output, lus sur le pipeline vidéo
  // OBS. video_output_get_skipped_frames = frames sautées faute de temps
  // d'encodage (surcharge CPU/GPU), sur le total encodé.
  if (video_t* video = obs_get_video()) {
    stats.renderSkipped = video_output_get_skipped_frames(video);
    stats.renderTotal = video_output_get_total_frames(video);
  }
  return stats;
}

namespace {

// Platform ceiling for the streaming output. Recording is never touched.
constexpr int kMaxVideoBitrateKbps = 8000;
constexpr uint32_t kMaxOutputWidth = 1920;
constexpr uint32_t kMaxOutputHeight = 1080;

// Débit IMPOSÉ aux instances Cloud, en kbps.
//
// Ce n'est pas un plafond, c'est une valeur : `pinStreamingBitrate()` la pose
// qu'on soit au-dessus OU en dessous. La différence compte. Un plafond laisse
// un client à 2500 kbps sans qu'il le sache, découvrir en direct que son image
// est molle, et nous appeler ; une valeur imposée donne à tout le monde la
// même qualité sans réglage à comprendre.
//
// POURQUOI 6000 ET PAS PLUS. La machine GPU partage UN moteur NVENC entre
// toutes les instances. Le débit ne coûte pas de silicium d'encodage, mais il
// coûte de la bande passante sortante, et surtout il détermine ce que la
// plateforme d'en face accepte : Twitch coupe au-delà de 6000 kbps pour les
// comptes sans partenariat, et une diffusion refusée à l'ingest est un direct
// perdu, pas une image dégradée.
//
// POURQUOI 6000 ET PAS MOINS. C'est le débit qui tient le 1080p60 sans
// bouillie sur les scènes chargées (jeu rapide, particules). En dessous,
// l'économie ne se voit pas sur la facture et la perte se voit à l'écran.
//
// Uniquement dans l'édition cloud (edition::pinsStreamingBitrate) : sur l'OBS
// que le client héberge lui-même, c'est sa machine, sa bande passante et son
// choix. Seul le plafond de 8000
// s'applique là-bas, pour la même raison d'ingest.
constexpr int kCloudPinnedBitrateKbps = 6000;

// Scale (inW, inH) down to fit within (maxW, maxH) preserving aspect ratio.
// No-op when it already fits. Result forced to even dimensions — encoders
// reject odd width/height.
void fitWithin(uint32_t inW, uint32_t inH, uint32_t maxW, uint32_t maxH,
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
  uint32_t w = static_cast<uint32_t>(std::lround(inW * scale)) & ~1u;
  uint32_t h = static_cast<uint32_t>(std::lround(inH * scale)) & ~1u;
  *outW = w;
  *outH = h;
}

} // namespace

OutputClampResult SceneController::clampStreamingConfig() {
  OutputClampResult result;

  // --- Bitrate, Simple mode : stored in the active profile config. Clamping
  // the key is harmless in Advanced mode (unused there), so no mode branch. ---
  if (config_t* profile = obs_frontend_get_profile_config()) {
    const int64_t vbitrate = config_get_int(profile, "SimpleOutput", "VBitrate");
    if (vbitrate > kMaxVideoBitrateKbps) {
      config_set_int(profile, "SimpleOutput", "VBitrate", kMaxVideoBitrateKbps);
      config_save(profile);
      result.bitrateClamped = true;
    }
  }

  // --- Bitrate (Advanced mode) + resolution : via the streaming output's
  // video encoder. STREAMING output only — we never fetch the recording one. ---
  obs_output_t* output = obs_frontend_get_streaming_output();
  if (output) {
    // Borrowed reference — do NOT release the encoder.
    obs_encoder_t* venc = obs_output_get_video_encoder(output);
    if (venc) {
      obs_data_t* settings = obs_encoder_get_settings(venc);
      if (settings) {
        const long long bitrate = obs_data_get_int(settings, "bitrate");
        if (bitrate > kMaxVideoBitrateKbps) {
          obs_data_set_int(settings, "bitrate", kMaxVideoBitrateKbps);
          obs_encoder_update(venc, settings);
          result.bitrateClamped = true;
        }
        obs_data_release(settings);
      }

      // Resolution can only be re-scaled while the encoder is idle; a live
      // encoder would reject/ignore the resize.
      if (!obs_encoder_active(venc)) {
        uint32_t w = obs_encoder_get_width(venc);
        uint32_t h = obs_encoder_get_height(venc);
        if (w == 0 || h == 0) {
          obs_video_info ovi{};
          if (obs_get_video_info(&ovi)) {
            w = ovi.output_width;
            h = ovi.output_height;
          }
        }
        if (w > kMaxOutputWidth || h > kMaxOutputHeight) {
          uint32_t sw = w, sh = h;
          fitWithin(w, h, kMaxOutputWidth, kMaxOutputHeight, &sw, &sh);
          if (sw && sh && (sw != w || sh != h)) {
            obs_encoder_set_scaled_size(venc, sw, sh);
            result.resolutionClamped = true;
          }
        }
      }
    }
    obs_output_release(output);
  }

  return result;
}

bool SceneController::clampVideoCanvas() {
  config_t* profile = obs_frontend_get_profile_config();
  if (!profile) return false;

  bool changed = false;

  // Rewrite one width/height config pair if it exceeds the ceiling. We only
  // touch the persisted config — never obs_reset_video here (it's unsafe with
  // an output active and heavy otherwise). Takes effect at the next video reset.
  auto clampPair = [&](const char* keyW, const char* keyH) {
    const uint64_t w = config_get_uint(profile, "Video", keyW);
    const uint64_t h = config_get_uint(profile, "Video", keyH);
    if (w == 0 || h == 0) return;  // key absent / not yet written
    if (w <= kMaxOutputWidth && h <= kMaxOutputHeight) return;
    uint32_t sw = static_cast<uint32_t>(w), sh = static_cast<uint32_t>(h);
    fitWithin(static_cast<uint32_t>(w), static_cast<uint32_t>(h),
              kMaxOutputWidth, kMaxOutputHeight, &sw, &sh);
    if (sw && sh && (sw != w || sh != h)) {
      config_set_uint(profile, "Video", keyW, sw);
      config_set_uint(profile, "Video", keyH, sh);
      changed = true;
    }
  };

  clampPair("BaseCX", "BaseCY");      // canvas resolution
  clampPair("OutputCX", "OutputCY");  // scaled (output) resolution

  if (changed) config_save(profile);
  return changed;
}

bool SceneController::clampStreamingEncoderLive() {
  bool clamped = false;
  obs_output_t* output = obs_frontend_get_streaming_output();
  if (!output) return false;

  obs_encoder_t* venc = obs_output_get_video_encoder(output); // borrowed
  if (venc) {
    obs_data_t* settings = obs_encoder_get_settings(venc);
    if (settings) {
      const long long bitrate = obs_data_get_int(settings, "bitrate");
      if (bitrate > kMaxVideoBitrateKbps) {
        obs_data_set_int(settings, "bitrate", kMaxVideoBitrateKbps);
        obs_encoder_update(venc, settings);
        clamped = true;
      }
      obs_data_release(settings);
    }
  }
  obs_output_release(output);
  return clamped;
}

bool SceneController::startStreaming() {
  if (obs_frontend_streaming_active()) return true;
  obs_frontend_streaming_start();
  return true;
}

bool SceneController::stopStreaming() {
  if (!obs_frontend_streaming_active()) return true;
  obs_frontend_streaming_stop();
  return true;
}

bool SceneController::startRecording() {
  if (obs_frontend_recording_active()) return true;
  obs_frontend_recording_start();
  return true;
}

bool SceneController::stopRecording() {
  if (!obs_frontend_recording_active()) return true;
  obs_frontend_recording_stop();
  return true;
}

bool SceneController::pinStreamingBitrate() {
  // Edition publique : rien. Le commentaire de kCloudPinnedBitrateKbps le
  // disait deja, sans garde pour le faire respecter : un poste de bureau qui
  // recevait une destination voyait son debit force a 6000 kbps.
  if (!edition::pinsStreamingBitrate()) return false;

  bool changed = false;
  bool profileSeen = false;
  bool encoderSeen = false;

  // --- Mode Simple : la valeur vit dans la configuration du profil. Poser la
  // clé est sans effet en mode Avancé (elle n'y est pas lue), donc aucun
  // branchement selon le mode — comme dans clampStreamingConfig().
  if (config_t* profile = obs_frontend_get_profile_config()) {
    profileSeen = true;
    if (config_get_int(profile, "SimpleOutput", "VBitrate") != kCloudPinnedBitrateKbps) {
      config_set_int(profile, "SimpleOutput", "VBitrate", kCloudPinnedBitrateKbps);
      config_save(profile);
      changed = true;
    }
  }

  // --- Mode Avancé : sur l'encodeur vidéo de la sortie de DIFFUSION. Jamais
  // celui de l'enregistrement : un client qui enregistre en local a le droit
  // d'y mettre le débit qu'il veut, ça ne traverse pas notre bande passante ni
  // l'ingest d'une plateforme.
  obs_output_t* output = obs_frontend_get_streaming_output();
  if (output) {
    obs_encoder_t* venc = obs_output_get_video_encoder(output);  // emprunté
    if (venc) {
      encoderSeen = true;
      obs_data_t* settings = obs_encoder_get_settings(venc);
      if (settings) {
        if (obs_data_get_int(settings, "bitrate") != kCloudPinnedBitrateKbps) {
          obs_data_set_int(settings, "bitrate", kCloudPinnedBitrateKbps);
          // x264 comme NVENC honorent une mise à jour de débit sur un encodeur
          // DÉJÀ actif : la valeur s'applique donc aussi en plein direct, sans
          // couper la diffusion.
          obs_encoder_update(venc, settings);
          changed = true;
        }
        obs_data_release(settings);
      }
    }
    obs_output_release(output);
  }

  // Journalise DANS TOUS LES CAS, y compris quand rien n'a bouge.
  //
  // La premiere version ne parlait que si elle avait change quelque chose. Le
  // resultat : aucune ligne dans le journal, aucune valeur ecrite dans
  // basic.ini, et rien pour distinguer « le debit etait deja bon » de « la
  // configuration du profil etait injoignable et la fonction n'a rien fait ».
  // Un verrou silencieux qu'on ne peut pas distinguer d'un verrou absent n'est
  // pas un verrou.
  blog(LOG_INFO,
       "[semirprod] debit : cible %d kbps | profil %s | sortie de diffusion %s | %s",
       kCloudPinnedBitrateKbps,
       profileSeen ? "lu" : "INTROUVABLE",
       encoderSeen ? "lue" : "absente (normal hors direct)",
       changed ? "modifie" : "deja conforme");
  return changed;
}

bool SceneController::applyStreamingService(const QString& rtmpUrl, const QString& streamKey) {
  // REFUS EN DIRECT, et ce n'est pas une précaution de confort. `obs_service_update`
  // sur une sortie active ne redirige rien : la sortie garde l'URL avec laquelle
  // elle a été démarrée. Accepter silencieusement rendrait un « succès » à
  // l'écran pendant que la diffusion continue vers l'ANCIENNE destination —
  // c'est-à-dire le pire des deux mondes, une interface qui ment sur l'endroit
  // où passe le direct. Le site doit arrêter, changer, redémarrer.
  if (obs_frontend_streaming_active()) {
    blog(LOG_WARNING,
         "[semirprod] changement de destination refuse : diffusion en cours");
    return false;
  }

  // UN SERVICE NEUF DE TYPE `rtmp_custom`, ET PAS UNE MISE A JOUR DE L'EXISTANT.
  //
  // La version precedente faisait `obs_service_update` sur le service en place
  // en y posant `service: "Custom"`. Le fichier ecrit le montrait :
  //
  //   {"type":"rtmp_common","settings":{"service":"Custom","server":"rtmp://...
  //
  // `rtmp_common` est le service des plateformes CONNUES : il resout son URL
  // depuis la liste de services d'OBS a partir du nom, et « Custom » n'y figure
  // pas. Le `server` pose a cote est ignore par ce type. Autrement dit, tout
  // avait l'air applique — fichier ecrit, succes rendu au site — et le direct
  // serait parti nulle part.
  //
  // `rtmp_custom` est le type qui prend `server` et `key` tels quels. On cree
  // donc un service de ce type et on l'INSTALLE comme service de diffusion.
  obs_data_t* settings = obs_data_create();
  obs_data_set_string(settings, "server", rtmpUrl.toUtf8().constData());
  obs_data_set_string(settings, "key", streamKey.toUtf8().constData());
  // Les plateformes refusent une publication non authentifiee ; ces deux champs
  // vides sont ce qu'OBS ecrit lui-meme pour un service personnalise sans
  // authentification par mot de passe, et les omettre laisse le service dans un
  // etat que l'interface d'OBS relit mal.
  obs_data_set_bool(settings, "use_auth", false);

  obs_service_t* service =
      obs_service_create("rtmp_custom", "semirprod_destination", settings, nullptr);
  obs_data_release(settings);
  if (!service) {
    blog(LOG_ERROR, "[semirprod] creation du service rtmp_custom impossible");
    return false;
  }

  obs_frontend_set_streaming_service(service);
  obs_frontend_save_streaming_service();
  // `obs_frontend_set_streaming_service` prend sa propre reference : la notre
  // doit partir, sinon chaque changement de destination fuit un service.
  obs_service_release(service);

  // L'hote seul, jamais l'URL entiere : certaines plateformes mettent la cle,
  // ou un jeton, dans le chemin de l'URL d'ingest, et le journal d'OBS se
  // recopie dans les tickets de support.
  blog(LOG_INFO, "[semirprod] destination appliquee : hote %s (cle masquee)",
       QUrl(rtmpUrl).host().toUtf8().constData());
  return true;
}

bool SceneController::isSourceInScene(const QString& sourceName, const QString& sceneName) const {
  if (sourceName.isEmpty() || sceneName.isEmpty()) return false;

  obs_source_t* sceneSource = findSceneSourceByName(sceneName);
  if (!sceneSource) return false;

  obs_scene_t* scene = obs_scene_from_source(sceneSource);
  SourcePresenceCtx ctx{sourceName, false};
  if (scene) {
    obs_scene_enum_items(scene, scenePresenceEnum, &ctx);
  }
  obs_source_release(sceneSource);
  return ctx.found;
}

QStringList SceneController::scenesContainingSource(const QString& sourceName) const {
  QStringList result;
  if (sourceName.isEmpty()) return result;
  for (const QString& name : availableScenes()) {
    if (isSourceInScene(sourceName, name)) result.push_back(name);
  }
  return result;
}

namespace {
struct ReadinessCtx {
  bool ready;
};

bool readinessEnum(obs_scene_t* /*scene*/, obs_sceneitem_t* item, void* param) {
  auto* ctx = static_cast<ReadinessCtx*>(param);
  obs_source_t* itemSource = obs_sceneitem_get_source(item);
  if (!itemSource) return true;
  const char* id = obs_source_get_id(itemSource);
  if (id && std::strcmp(id, "semirprod_live_source") == 0) {
    if (isSourceInstanceReady(itemSource)) {
      ctx->ready = true;
      return false; // stop enumeration
    }
  }
  return true;
}
} // namespace

bool SceneController::isSemirprodSourceReadyInScene(const QString& sceneName) const {
  if (sceneName.isEmpty()) return false;
  obs_source_t* sceneSource = findSceneSourceByName(sceneName);
  if (!sceneSource) return false;

  obs_scene_t* scene = obs_scene_from_source(sceneSource);
  ReadinessCtx ctx{false};
  if (scene) obs_scene_enum_items(scene, readinessEnum, &ctx);
  obs_source_release(sceneSource);
  return ctx.ready;
}

namespace {

// Identifiant de TYPE de notre source. Le nom affiche, lui, appartient a
// l'operateur : il peut le renommer, donc on ne cherche jamais par nom.
constexpr const char* kLiveSourceTypeId = "semirprod_live_source";

struct PresenceCtx {
  bool found = false;
};

bool presenceByTypeEnum(obs_scene_t* /*scene*/, obs_sceneitem_t* item, void* param) {
  auto* ctx = static_cast<PresenceCtx*>(param);
  obs_source_t* itemSource = obs_sceneitem_get_source(item);
  if (!itemSource) return true;
  const char* id = obs_source_get_id(itemSource);
  if (id && std::strcmp(id, kLiveSourceTypeId) == 0) {
    ctx->found = true;
    return false;  // stop enumeration
  }
  return true;
}

struct FindSourceCtx {
  obs_source_t* found = nullptr;  // reference tenue, a relacher par l'appelant
};

bool findLiveSourceEnum(void* param, obs_source_t* source) {
  auto* ctx = static_cast<FindSourceCtx*>(param);
  if (!source) return true;
  const char* id = obs_source_get_id(source);
  if (id && std::strcmp(id, kLiveSourceTypeId) == 0) {
    // obs_enum_sources prete la source sans incrementer : sans ref, elle peut
    // disparaitre entre la fin de l'enumeration et son utilisation.
    ctx->found = obs_source_get_ref(source);
    return false;
  }
  return true;
}

// Ajuste l'element au canevas, comme le « Ajuster a l'ecran » d'OBS. Sans ca la
// source arrive a sa taille native dans un coin, et l'operateur doit encore la
// redimensionner a la main — on n'aurait deplace la corvee, pas supprimee.
void fitItemToCanvas(obs_sceneitem_t* item) {
  if (!item) return;
  obs_video_info ovi;
  if (!obs_get_video_info(&ovi)) return;
  vec2 bounds;
  vec2_set(&bounds, static_cast<float>(ovi.base_width),
           static_cast<float>(ovi.base_height));
  obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_SCALE_INNER);
  obs_sceneitem_set_bounds_alignment(item, OBS_ALIGN_CENTER);
  obs_sceneitem_set_bounds(item, &bounds);
}

// Cle du reglage porte par nos sources. Recopiee ici plutot qu'incluse :
// SemirprodLiveSource ne l'expose pas, et la faire voyager par un en-tete
// obligerait SceneController a dependre du type de source qu'il manipule par
// identifiant depuis toujours.
constexpr const char* kStreamSettingKey = "stream_id";
constexpr const char* kStreamSettingAuto = "auto";

// Le flux punaise sur une source, ou une chaine vide si elle suit le poste.
QString pinnedStreamOf(obs_source_t* source) {
  if (!source) return QString();
  const char* typeId = obs_source_get_id(source);
  if (!typeId || std::strcmp(typeId, kLiveSourceTypeId) != 0) return QString();
  obs_data_t* settings = obs_source_get_settings(source);
  if (!settings) return QString();
  const char* raw = obs_data_get_string(settings, kStreamSettingKey);
  QString out = raw ? QString::fromUtf8(raw) : QString();
  obs_data_release(settings);
  if (out == QString::fromUtf8(kStreamSettingAuto)) return QString();
  return out;
}

struct FindByStreamCtx {
  QString wanted;
  obs_source_t* found = nullptr;  // reference tenue, a relacher par l'appelant
};

bool findByStreamEnum(void* param, obs_source_t* source) {
  auto* ctx = static_cast<FindByStreamCtx*>(param);
  if (!source) return true;
  if (pinnedStreamOf(source) != ctx->wanted) return true;
  ctx->found = obs_source_get_ref(source);
  return false;
}

struct StreamPresenceCtx {
  QString wanted;
  QString autoId;
  bool found = false;
};

bool streamPresenceEnum(obs_scene_t* /*scene*/, obs_sceneitem_t* item, void* param) {
  auto* ctx = static_cast<StreamPresenceCtx*>(param);
  obs_source_t* itemSource = obs_sceneitem_get_source(item);
  if (!itemSource) return true;
  const char* typeId = obs_source_get_id(itemSource);
  if (!typeId || std::strcmp(typeId, kLiveSourceTypeId) != 0) return true;
  const QString pinned = pinnedStreamOf(itemSource);
  const QString effective = pinned.isEmpty() ? ctx->autoId : pinned;
  if (effective == ctx->wanted) {
    ctx->found = true;
    return false;
  }
  return true;
}

} // namespace

bool SceneController::placeStreamSourceInCurrentScene(const QString& streamId,
                                                      const QString& label) {
  if (streamId.isEmpty()) return false;
  const QString sceneName = currentScene();
  if (sceneName.isEmpty()) return false;

  obs_source_t* sceneSource = findSceneSourceByName(sceneName);
  if (!sceneSource) return false;
  obs_scene_t* scene = obs_scene_from_source(sceneSource);
  if (!scene) {
    obs_source_release(sceneSource);
    return false;
  }

  FindByStreamCtx finder;
  finder.wanted = streamId;
  obs_enum_sources(findByStreamEnum, &finder);
  obs_source_t* source = finder.found;

  if (!source) {
    // Nom unique : obs_source_create n'interdit pas les homonymes, et deux
    // sources de meme nom rendent obs_get_source_by_name ambigu pour tout le
    // reste du plugin. Le nom vient du flux, pour que l'operateur reconnaisse
    // sa camera dans la liste des sources d'OBS.
    QString base = label.trimmed();
    if (base.isEmpty()) base = loc("Source.Live");
    QString name = base;
    for (int suffix = 2; suffix < 100; ++suffix) {
      obs_source_t* clash = obs_get_source_by_name(name.toUtf8().constData());
      if (!clash) break;
      obs_source_release(clash);
      name = QStringLiteral("%1 (%2)").arg(base).arg(suffix);
    }

    obs_data_t* settings = obs_data_create();
    obs_data_set_string(settings, kStreamSettingKey, streamId.toUtf8().constData());
    source = obs_source_create(kLiveSourceTypeId, name.toUtf8().constData(), settings,
                               nullptr);
    obs_data_release(settings);
  }

  bool ok = false;
  if (source) {
    // Deja dans cette scene : on ne l'y ajoute pas une seconde fois, sinon le
    // meme flux se superpose a lui-meme.
    StreamPresenceCtx presence;
    presence.wanted = streamId;
    presence.autoId = QString();
    obs_scene_enum_items(scene, streamPresenceEnum, &presence);

    if (presence.found) {
      ok = true;
    } else {
      obs_sceneitem_t* item = obs_scene_add(scene, source);
      if (item) {
        fitItemToCanvas(item);
        ok = true;
      }
    }
    obs_source_release(source);
  }

  obs_source_release(sceneSource);
  return ok;
}

int SceneController::sceneCountForStream(const QString& streamId,
                                         const QString& autoStreamId) const {
  if (streamId.isEmpty()) return 0;
  int count = 0;
  for (const QString& sceneName : availableScenes()) {
    obs_source_t* sceneSource = findSceneSourceByName(sceneName);
    if (!sceneSource) continue;
    obs_scene_t* scene = obs_scene_from_source(sceneSource);
    if (scene) {
      StreamPresenceCtx ctx;
      ctx.wanted = streamId;
      ctx.autoId = autoStreamId;
      obs_scene_enum_items(scene, streamPresenceEnum, &ctx);
      if (ctx.found) ++count;
    }
    obs_source_release(sceneSource);
  }
  return count;
}

bool SceneController::hasSemirprodSourceInScene(const QString& sceneName) const {
  if (sceneName.isEmpty()) return false;
  obs_source_t* sceneSource = findSceneSourceByName(sceneName);
  if (!sceneSource) return false;

  obs_scene_t* scene = obs_scene_from_source(sceneSource);
  PresenceCtx ctx;
  if (scene) obs_scene_enum_items(scene, presenceByTypeEnum, &ctx);
  obs_source_release(sceneSource);
  return ctx.found;
}

QStringList SceneController::scenesContainingSemirprodSource() const {
  QStringList result;
  for (const QString& name : availableScenes()) {
    if (hasSemirprodSourceInScene(name)) result.push_back(name);
  }
  return result;
}

bool SceneController::placeSemirprodSourceInScene(const QString& sceneName) {
  if (sceneName.isEmpty()) return false;
  if (hasSemirprodSourceInScene(sceneName)) return true;

  obs_source_t* sceneSource = findSceneSourceByName(sceneName);
  if (!sceneSource) return false;
  obs_scene_t* scene = obs_scene_from_source(sceneSource);
  if (!scene) {
    obs_source_release(sceneSource);
    return false;
  }

  // Reutiliser avant de creer. Le cas courant n'est pas « aucune source » mais
  // « source posee dans la mauvaise scene » : creer une deuxieme instance
  // laisserait deux sources concurrentes dans la collection, et l'operateur
  // devrait deviner laquelle compte.
  FindSourceCtx finder;
  obs_enum_sources(findLiveSourceEnum, &finder);
  obs_source_t* source = finder.found;

  const bool created = (source == nullptr);
  if (created) {
    // Nom unique : obs_source_create n'interdit pas les homonymes, et deux
    // sources de meme nom rendent obs_get_source_by_name ambigu pour tout le
    // reste du plugin.
    const QString baseName = loc("Source.Live");
    QString name = baseName;
    for (int suffix = 2; suffix < 100; ++suffix) {
      obs_source_t* clash = obs_get_source_by_name(name.toUtf8().constData());
      if (!clash) break;
      obs_source_release(clash);
      name = QStringLiteral("%1 (%2)").arg(baseName).arg(suffix);
    }
    source = obs_source_create(kLiveSourceTypeId, name.toUtf8().constData(),
                               nullptr, nullptr);
  }

  bool ok = false;
  if (source) {
    obs_sceneitem_t* item = obs_scene_add(scene, source);
    if (item) {
      fitItemToCanvas(item);
      ok = true;
    }
    obs_source_release(source);
  }

  obs_source_release(sceneSource);
  return ok;
}

bool SceneController::addSourceToScene(const QString& sourceName, const QString& sceneName) {
  if (sourceName.isEmpty() || sceneName.isEmpty()) return false;
  if (isSourceInScene(sourceName, sceneName)) return true;

  obs_source_t* sceneSource = findSceneSourceByName(sceneName);
  if (!sceneSource) return false;

  obs_scene_t* scene = obs_scene_from_source(sceneSource);
  obs_source_t* source = obs_get_source_by_name(sourceName.toUtf8().constData());

  bool ok = false;
  if (scene && source) {
    obs_sceneitem_t* item = obs_scene_add(scene, source);
    ok = (item != nullptr);
  }

  if (source) obs_source_release(source);
  obs_source_release(sceneSource);
  return ok;
}

namespace {

struct SceneItemsCtx {
  QJsonArray items;
  int zOrder = 0;
};

bool collectSceneItemsEnum(obs_scene_t* /*scene*/, obs_sceneitem_t* item, void* param) {
  auto* ctx = static_cast<SceneItemsCtx*>(param);
  obs_source_t* source = obs_sceneitem_get_source(item);
  if (!source) return true;

  QJsonObject entry;
  // RANG DE SUPERPOSITION, EXPLICITE. 0 = calque du FOND, le plus grand = celui
  // de DEVANT.
  //
  // `obs_scene_enum_items` enumere du fond vers l'avant : le premier element
  // recu est celui du dessous. L'interface d'OBS, elle, affiche le calque de
  // DEVANT en HAUT de sa liste. Le tableau brut est donc dans l'ordre inverse
  // de ce que l'operateur a sous les yeux dans OBS — et le dashboard, qui
  // l'affichait tel quel, montrait « navigateur sous camera » la ou OBS montre
  // « navigateur sur camera ». En direct, c'est un clic sur le mauvais calque.
  //
  // On envoie donc le rang plutot que de compter sur l'ordre du tableau : un
  // ordre implicite se reinverse au premier `sort` ou `reverse` de passage,
  // sans que rien ne le signale. L'ordre du tableau reste inchange, pour ne
  // rien casser chez les lecteurs existants.
  entry.insert(QStringLiteral("zOrder"), ctx->zOrder++);
  entry.insert(QStringLiteral("sceneItemId"),
               static_cast<double>(obs_sceneitem_get_id(item)));
  entry.insert(QStringLiteral("sourceName"),
               QString::fromUtf8(obs_source_get_name(source)));
  const char* sid = obs_source_get_id(source);
  entry.insert(QStringLiteral("sourceType"),
               sid ? QString::fromUtf8(sid) : QString());
  entry.insert(QStringLiteral("visible"), obs_sceneitem_visible(item));
  entry.insert(QStringLiteral("locked"), obs_sceneitem_locked(item));
  const uint32_t flags = obs_source_get_output_flags(source);
  entry.insert(QStringLiteral("hasAudio"),
               (flags & OBS_SOURCE_AUDIO) != 0);
  entry.insert(QStringLiteral("hasVideo"),
               (flags & OBS_SOURCE_VIDEO) != 0);
  ctx->items.append(entry);
  return true;
}

struct SceneItemFinder {
  QString needle;
  obs_sceneitem_t* match = nullptr;
};

bool findSceneItemEnum(obs_scene_t* /*scene*/, obs_sceneitem_t* item, void* param) {
  auto* ctx = static_cast<SceneItemFinder*>(param);
  obs_source_t* source = obs_sceneitem_get_source(item);
  if (!source) return true;
  if (QString::fromUtf8(obs_source_get_name(source)) == ctx->needle) {
    ctx->match = item;
    return false;
  }
  return true;
}

obs_sceneitem_t* findSceneItem(const QString& sceneName, const QString& sourceName) {
  obs_source_t* sceneSource = findSceneSourceByName(sceneName);
  if (!sceneSource) return nullptr;
  obs_scene_t* scene = obs_scene_from_source(sceneSource);
  SceneItemFinder ctx{sourceName, nullptr};
  if (scene) obs_scene_enum_items(scene, findSceneItemEnum, &ctx);
  obs_source_release(sceneSource);
  return ctx.match;
}

float linearToDb(float linear) {
  if (linear <= 0.0f) return -100.0f;
  return 20.0f * std::log10(linear);
}

float dbToLinear(float db) {
  if (db <= -100.0f) return 0.0f;
  return std::pow(10.0f, db / 20.0f);
}

struct AudioCollectCtx {
  QJsonArray items;
  QSet<QString> seen;  // dédoublonnage par nom de source
};

// Vrai quand l'opérateur a masqué la source du mélangeur dans OBS (clic droit
// « Masquer dans le mélangeur »). Stocké dans les réglages privés de la source
// sous "mixer_hidden" — même lecture que SourceMixerHidden() côté OBS.
bool sourceMixerHidden(obs_source_t* source) {
  obs_data_t* priv = obs_source_get_private_settings(source);
  const bool hidden = obs_data_get_bool(priv, "mixer_hidden");
  obs_data_release(priv);
  return hidden;
}

// Ajoute une entrée mixer pour UNE source audio, dédoublonnée par nom. Ignore
// les sources sans flag audio, les scènes et les transitions (elles portent
// un flag audio mais ne sont pas des entrées du mixer utilisateur).
void appendAudioEntry(AudioCollectCtx* ctx, obs_source_t* source) {
  if (!source) return;
  const uint32_t flags = obs_source_get_output_flags(source);
  if (!(flags & OBS_SOURCE_AUDIO)) return;
  if (obs_source_is_scene(source) || obs_source_is_group(source)) return;

  // Le flag OBS_SOURCE_AUDIO dit seulement « ce type de source SAIT faire du
  // son », pas « ce son passe par le mélangeur ». OBS lui-même ajoute une
  // tranche via ActivateAudioSource(), qui exige trois choses : non masquée du
  // mélangeur, source active, et audio_active. On reproduit les deux qui
  // manquaient ici (l'activité est testée par l'appelant côté énumération).
  //
  // Sans elles on remontait des sources dont le son n'atteint jamais le
  // mélangeur OBS, avec un fader et un mute qui ne pilotent rien :
  //   - navigateur dont « Contrôler l'audio via OBS » est décoché
  //     (obs-browser fait obs_source_set_audio_active(source, reroute_audio),
  //     et ce réglage est décoché PAR DÉFAUT — donc tous les navigateurs) ;
  //   - webcam dshow sans piste audio (win-dshow met audio_active à false).
  if (!obs_source_audio_active(source)) return;
  if (sourceMixerHidden(source)) return;
  const char* idCStr = obs_source_get_id(source);
  if (idCStr && (std::strcmp(idCStr, "scene") == 0 ||
                 std::strcmp(idCStr, "group") == 0)) {
    return;
  }

  const QString name = QString::fromUtf8(obs_source_get_name(source));
  if (ctx->seen.contains(name)) return;
  ctx->seen.insert(name);

  QJsonObject entry;
  entry.insert(QStringLiteral("name"), name);
  entry.insert(QStringLiteral("type"),
               idCStr ? QString::fromUtf8(idCStr) : QString());
  entry.insert(QStringLiteral("muted"), obs_source_muted(source));
  entry.insert(QStringLiteral("volumeDb"),
               static_cast<double>(linearToDb(obs_source_get_volume(source))));
  entry.insert(QStringLiteral("monitoringType"),
               static_cast<int>(obs_source_get_monitoring_type(source)));
  entry.insert(QStringLiteral("audioActive"),
               obs_source_audio_active(source));
  ctx->items.append(entry);
}

// Callback d'énumération globale des sources → collecte toute source audio
// ACTIVE dans le programme (obs_source_active), quelle que soit sa profondeur
// (scène imbriquée, groupe…). C'est la sémantique du mixer OBS lui-même : il
// s'abonne à source_activate/deactivate, pas aux items de la scène courante.
bool activeAudioEnum(void* param, obs_source_t* source) {
  if (source && obs_source_active(source)) {
    appendAudioEntry(static_cast<AudioCollectCtx*>(param), source);
  }
  return true;
}

} // namespace

QJsonArray SceneController::collectSceneSources() const {
  QJsonArray result;
  obs_frontend_source_list sourceList{};
  obs_frontend_get_scenes(&sourceList);
  for (size_t i = 0; i < sourceList.sources.num; ++i) {
    obs_source_t* sceneSource = sourceList.sources.array[i];
    if (!sceneSource) continue;
    obs_scene_t* scene = obs_scene_from_source(sceneSource);
    if (!scene) continue;

    SceneItemsCtx ctx;
    obs_scene_enum_items(scene, collectSceneItemsEnum, &ctx);

    QJsonObject sceneObj;
    sceneObj.insert(QStringLiteral("scene"),
                    QString::fromUtf8(obs_source_get_name(sceneSource)));
    sceneObj.insert(QStringLiteral("items"), ctx.items);
    result.append(sceneObj);
  }
  obs_frontend_source_list_free(&sourceList);
  return result;
}

QJsonArray SceneController::collectAudioInputs() const {
  // Reproduit le dock « Mélangeur audio » d'OBS : (1) les canaux audio globaux
  // (Desktop / Micro-Aux, réglés dans Paramètres > Audio — toujours affichés)
  // et (2) toute source audio active dans le programme. L'énumération globale
  // + obs_source_active couvre les scènes imbriquées et les groupes, qu'un
  // parcours des items de la scène courante (un seul niveau) ratait — d'où
  // des caméras présentes dans le mixer OBS mais absentes du dashboard.
  // Le tri fin (audio réellement routé vers le mélangeur, source non masquée)
  // est fait par appendAudioEntry, commun aux deux chemins.
  AudioCollectCtx ctx;

  // (1) Canaux audio globaux — MAX_CHANNELS (6) sorties audio racine.
  for (uint32_t i = 0; i < MAX_CHANNELS; ++i) {
    obs_source_t* s = obs_get_output_source(i);
    if (s) {
      appendAudioEntry(&ctx, s);
      obs_source_release(s);
    }
  }

  // (2) Sources audio actives dans le programme (toute profondeur).
  obs_enum_sources(activeAudioEnum, &ctx);

  return ctx.items;
}

bool SceneController::setSceneItemVisible(const QString& sceneName,
                                          const QString& sourceName,
                                          bool visible) {
  obs_sceneitem_t* item = findSceneItem(sceneName, sourceName);
  if (!item) return false;
  obs_sceneitem_set_visible(item, visible);
  return true;
}

bool SceneController::setSceneItemLocked(const QString& sceneName,
                                         const QString& sourceName,
                                         bool locked) {
  obs_sceneitem_t* item = findSceneItem(sceneName, sourceName);
  if (!item) return false;
  obs_sceneitem_set_locked(item, locked);
  return true;
}

bool SceneController::setSourceMuted(const QString& sourceName, bool muted) {
  obs_source_t* source = obs_get_source_by_name(sourceName.toUtf8().constData());
  if (!source) return false;
  obs_source_set_muted(source, muted);
  obs_source_release(source);
  return true;
}

bool SceneController::setSourceVolumeDb(const QString& sourceName, float volumeDb) {
  obs_source_t* source = obs_get_source_by_name(sourceName.toUtf8().constData());
  if (!source) return false;
  const float clamped = std::clamp(volumeDb, -100.0f, 26.0f);
  obs_source_set_volume(source, dbToLinear(clamped));
  obs_source_release(source);
  return true;
}

bool SceneController::setSourceMonitoringType(const QString& sourceName,
                                              int monitoringType) {
  if (monitoringType < 0 || monitoringType > 2) return false;
  obs_source_t* source = obs_get_source_by_name(sourceName.toUtf8().constData());
  if (!source) return false;
  obs_source_set_monitoring_type(
      source, static_cast<obs_monitoring_type>(monitoringType));
  obs_source_release(source);
  return true;
}

namespace {

// Mirrors OBS's own ScreenshotObj pipeline (frontend/utility/ScreenshotObj.cpp):
// stage 0 renders the program output into a texrender, stage 1 stages the
// texture down to a CPU-readable surface, stage 2 maps + copies pixels into a
// QImage. JPG-encoding and the user callback then run on the Qt main thread.
// libobs has no synchronous "give me the canvas as bytes" API — anything
// claiming otherwise (`obs_video_get_canvas_snapshot`) is not exported.
class ScreenshotJob {
 public:
  using Callback = std::function<void(const QString&)>;

  ScreenshotJob(int scaledWidth, int jpgQuality, Callback callback)
      : scaledWidth_(scaledWidth),
        jpgQuality_(std::clamp(jpgQuality, 1, 100)),
        callback_(std::move(callback)) {}

  void start() { obs_add_tick_callback(&ScreenshotJob::tickThunk, this); }

 private:
  static void tickThunk(void* param, float /*seconds*/) {
    static_cast<ScreenshotJob*>(param)->tick();
  }

  void tick() {
    obs_enter_graphics();
    switch (stage_) {
      case 0: doRender(); break;
      case 1: doDownload(); break;
      case 2: doCopy(); break;
      default: break;
    }
    obs_leave_graphics();
    ++stage_;

    if (stage_ >= 3) {
      obs_remove_tick_callback(&ScreenshotJob::tickThunk, this);
      finish();
    }
  }

  void doRender() {
    obs_video_info ovi{};
    if (!obs_get_video_info(&ovi)) return;
    cx_ = ovi.base_width;
    cy_ = ovi.base_height;
    if (!cx_ || !cy_) return;

    // SDR pipeline only — HDR projects fall back to the sRGB tonemap that
    // libobs applies for non-extended color spaces.
    constexpr enum gs_color_space space = GS_CS_SRGB;
    const enum gs_color_format format = gs_get_format_from_space(space);

    // Target size for the GPU downscale: preserve aspect, never upscale, even
    // dims. When the requested width already covers the canvas we capture full.
    uint32_t tw = cx_;
    uint32_t th = cy_;
    if (scaledWidth_ > 0 && static_cast<uint32_t>(scaledWidth_) < cx_) {
      tw = static_cast<uint32_t>(scaledWidth_) & ~1u;
      th = static_cast<uint32_t>(std::lround(static_cast<double>(tw) * cy_ / cx_)) & ~1u;
      if (tw == 0) tw = 2;
      if (th == 0) th = 2;
    }

    // Preferred path: render the composited program straight into a target-sized
    // texrender so the readback AND the JPEG both operate on the small image —
    // the GPU does the scale for free during the draw. The full-canvas readback
    // and the CPU SmoothTransformation downscale (10-20 ms at 1080p) disappear.
    texrender_ = gs_texrender_create(format, GS_ZS_NONE);
    stagesurf_ = gs_stagesurface_create(tw, th, format);
    if (texrender_ && stagesurf_) {
      capW_ = tw;
      capH_ = th;
      needsCpuDownscale_ = false;
    } else {
      // Fallback: creating a scaled surface failed — capture the full canvas
      // and let the worker CPU-downscale, i.e. the original pipeline. Rebuild
      // the surfaces at full canvas size.
      if (stagesurf_) {
        gs_stagesurface_destroy(stagesurf_);
        stagesurf_ = nullptr;
      }
      if (texrender_) {
        gs_texrender_destroy(texrender_);
        texrender_ = nullptr;
      }
      texrender_ = gs_texrender_create(format, GS_ZS_NONE);
      stagesurf_ = gs_stagesurface_create(cx_, cy_, format);
      if (!texrender_ || !stagesurf_) return;
      capW_ = cx_;
      capH_ = cy_;
      needsCpuDownscale_ = true;
    }

    if (gs_texrender_begin_with_color_space(texrender_, capW_, capH_, space)) {
      vec4 zero;
      vec4_zero(&zero);
      gs_clear(GS_CLEAR_COLOR, &zero, 0.0f, 0);
      // Ortho spans the FULL canvas coord space while the texrender target is
      // capW_×capH_, so the viewport scales the composited frame down to the
      // target size during obs_render_main_texture() (no-op when equal).
      gs_ortho(0.0f, static_cast<float>(cx_), 0.0f, static_cast<float>(cy_),
               -100.0f, 100.0f);

      gs_blend_state_push();
      gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
      obs_render_main_texture();
      gs_blend_state_pop();

      gs_texrender_end(texrender_);
    }
  }

  void doDownload() {
    if (!texrender_ || !stagesurf_) return;
    gs_stage_texture(stagesurf_, gs_texrender_get_texture(texrender_));
  }

  void doCopy() {
    if (!stagesurf_ || !capW_ || !capH_) return;
    uint8_t* data = nullptr;
    uint32_t srcStride = 0;
    if (!gs_stagesurface_map(stagesurf_, &data, &srcStride)) return;

    image_ = QImage(static_cast<int>(capW_), static_cast<int>(capH_),
                    QImage::Format_RGBX8888);
    const int dstStride = image_.bytesPerLine();
    const int copyStride = std::min<int>(dstStride, static_cast<int>(srcStride));
    for (uint32_t y = 0; y < capH_; ++y) {
      std::memcpy(image_.scanLine(static_cast<int>(y)),
                  data + (y * srcStride), copyStride);
    }
    gs_stagesurface_unmap(stagesurf_);
  }

  void finish() {
    // Graphics resource cleanup MUST happen on the render thread because
    // texrender / stagesurface live in libobs's GPU context. Anything below
    // is plain CPU work and should leave the render thread immediately —
    // at 1080p / quality 85 the JPG encode alone is 50-150 ms, which would
    // stall every OBS render tick that follows it and visibly hitch the
    // program output.
    obs_enter_graphics();
    if (stagesurf_) gs_stagesurface_destroy(stagesurf_);
    if (texrender_) gs_texrender_destroy(texrender_);
    obs_leave_graphics();
    stagesurf_ = nullptr;
    texrender_ = nullptr;

    if (image_.isNull()) {
      Callback cb = std::move(callback_);
      QMetaObject::invokeMethod(
          QCoreApplication::instance(),
          [cb = std::move(cb)]() mutable { cb(QString()); },
          Qt::QueuedConnection);
      delete this;
      return;
    }

    // Move ownership into the worker so we can free the job before the
    // encoding completes — back-pressure in the dock already prevents
    // multiple jobs running concurrently per stream, but the thread pool
    // still has its own scheduling.
    QImage img = std::move(image_);
    const int scaledWidth = scaledWidth_;
    const int jpgQuality = jpgQuality_;
    const bool needsCpuDownscale = needsCpuDownscale_;
    Callback cb = std::move(callback_);
    delete this;

    QThreadPool::globalInstance()->start([img = std::move(img), scaledWidth, jpgQuality,
                                          needsCpuDownscale, cb = std::move(cb)]() mutable {
      QImage scaled = std::move(img);
      if (needsCpuDownscale && scaledWidth > 0 && scaled.width() > scaledWidth) {
        // Fallback path only: the GPU-scaled path already produced a target-size
        // image. Smooth scaling at 1080p costs ~10-20 ms; we eat it on a worker
        // rather than the render thread. FastTransformation would shave a few ms
        // but produces visibly aliased thumbnails — not worth it.
        scaled = scaled.scaledToWidth(scaledWidth, Qt::SmoothTransformation);
      }
      QByteArray ba;
      QBuffer buffer(&ba);
      buffer.open(QIODevice::WriteOnly);
      QString result;
      if (scaled.save(&buffer, "JPG", jpgQuality)) {
        result = QString::fromLatin1(ba.toBase64());
      }
      // The user callback typically touches QWebSocket (main-thread only),
      // so hop back even though we already left the render thread.
      QMetaObject::invokeMethod(
          QCoreApplication::instance(),
          [cb = std::move(cb), result]() mutable { cb(result); },
          Qt::QueuedConnection);
    });
  }

  int scaledWidth_;
  int jpgQuality_;
  Callback callback_;
  int stage_ = 0;
  uint32_t cx_ = 0;  // canvas (source) dimensions
  uint32_t cy_ = 0;
  uint32_t capW_ = 0;  // captured surface dimensions (target size, or canvas on fallback)
  uint32_t capH_ = 0;
  bool needsCpuDownscale_ = false;  // true only when the GPU-scaled path fell back
  gs_texrender_t* texrender_ = nullptr;
  gs_stagesurf_t* stagesurf_ = nullptr;
  QImage image_;
};

} // namespace

void SceneController::takeScreenshot(int scaledWidth, int jpgQuality,
                                     std::function<void(const QString&)> callback) {
  if (!callback) return;
  auto* job = new ScreenshotJob(scaledWidth, jpgQuality, std::move(callback));
  job->start();
}

} // namespace semirprod
