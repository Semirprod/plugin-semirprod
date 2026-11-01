#include "PreviewSceneMix.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <obs.h>

namespace semirprod {

namespace {

/** Scène à composer dans la vue : celle en préparation quand le mode studio
    est actif. Hors mode studio il n'y a pas de préparation — on retombe sur la
    scène courante pour que l'aperçu montre quelque chose de cohérent plutôt
    qu'un écran noir pendant le battement où le mode vient d'être coupé.
    Référence NEUVE : à relâcher par l'appelant. */
obs_source_t* previewSceneRef() {
  obs_source_t* source = obs_frontend_preview_program_mode_active()
                             ? obs_frontend_get_current_preview_scene()
                             : obs_frontend_get_current_scene();
  return source;
}

} // namespace

PreviewSceneMix::~PreviewSceneMix() {
  release();
}

video_t* PreviewSceneMix::ensure() {
  if (view_ && video_) {
    syncToPreviewScene();
    return video_;
  }

  if (!obs_get_video()) {
    blog(LOG_ERROR,
         "[semirprod-preview] pas de pipeline video — mix de preparation "
         "impossible");
    return nullptr;
  }

  obs_source_t* scene = previewSceneRef();
  if (!scene) {
    blog(LOG_WARNING,
         "[semirprod-preview] aucune scene de preparation — mix non cree");
    return nullptr;
  }

  view_ = obs_view_create();
  if (!view_) {
    obs_source_release(scene);
    blog(LOG_ERROR, "[semirprod-preview] obs_view_create a echoue");
    return nullptr;
  }
  obs_view_set_source(view_, 0, scene);
  obs_source_release(scene);

  // Deuxième mix aux réglages vidéo courants d'OBS (même canevas, même fps).
  // L'encodeur réduit ensuite à la taille d'aperçu ; on ne touche pas au
  // pipeline global, qui appartient à la diffusion.
  video_ = obs_view_add(view_);
  if (!video_) {
    obs_view_set_source(view_, 0, nullptr);
    obs_view_destroy(view_);
    view_ = nullptr;
    blog(LOG_ERROR, "[semirprod-preview] obs_view_add a echoue");
    return nullptr;
  }

  blog(LOG_INFO, "[semirprod-preview] mix de preparation cree");
  return video_;
}

void PreviewSceneMix::syncToPreviewScene() {
  if (!view_) return;
  obs_source_t* scene = previewSceneRef();
  // Un nullptr est accepté par obs_view_set_source : la vue rend alors du noir,
  // ce qui vaut mieux que de garder à l'antenne d'aperçu une scène détruite.
  obs_view_set_source(view_, 0, scene);
  if (scene) obs_source_release(scene);
}

void PreviewSceneMix::release() {
  if (!view_) return;
  obs_view_set_source(view_, 0, nullptr);
  if (video_) {
    obs_view_remove(view_);
    video_ = nullptr;
  }
  obs_view_destroy(view_);
  view_ = nullptr;
  blog(LOG_INFO, "[semirprod-preview] mix de preparation libere");
}

} // namespace semirprod
