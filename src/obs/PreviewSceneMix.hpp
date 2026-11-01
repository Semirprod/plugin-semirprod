#pragma once

typedef struct obs_view obs_view_t;
typedef struct video_output video_t;

namespace semirprod {

/** Deuxième mix vidéo, branché sur la scène de PRÉPARATION du mode studio.

    Le programme est déjà disponible tel quel (`obs_get_video()`), mais la
    scène en préparation n'a pas de sortie : elle n'est rendue que dans le
    canevas de gauche d'OBS. Pour l'envoyer au dashboard, il faut la composer
    dans une vue à part — c'est exactement le mécanisme des projecteurs et du
    multiview d'OBS (`obs_view_create` + `obs_view_add`).

    Coût : une deuxième composition à la résolution du canevas, plus l'encodeur
    qui la consomme. La vue n'est donc créée QUE quand le dashboard demande
    l'aperçu de préparation, et détruite dès que le bail expire.

    ⚠️ Ordre de destruction : tout encodeur branché sur `video()` doit être
    détruit AVANT release(). La vue est le producteur, l'encodeur le
    consommateur ; l'inverse laisse un encodeur pointer un mix mort.

    À piloter depuis le thread Qt principal, comme le reste du plugin. */
class PreviewSceneMix {
public:
  PreviewSceneMix() = default;
  ~PreviewSceneMix();

  PreviewSceneMix(const PreviewSceneMix&) = delete;
  PreviewSceneMix& operator=(const PreviewSceneMix&) = delete;

  /** Crée la vue si besoin et la pointe sur la scène de préparation courante.
      Retourne le mix vidéo à donner à l'encodeur, ou nullptr en cas d'échec
      (pas de pipeline vidéo, mode studio inactif, aucune scène). */
  video_t* ensure();

  /** Repointe la vue sur la scène de préparation courante. Appelé sur
      PREVIEW_SCENE_CHANGED : l'output et l'encodeur restent vivants, seule la
      source composée change — donc pas de coupure côté spectateur. No-op si la
      vue n'existe pas. */
  void syncToPreviewScene();

  /** Détache la source, retire le mix et détruit la vue. Idempotent. */
  void release();

  bool active() const { return view_ != nullptr; }
  video_t* video() const { return video_; }

private:
  obs_view_t* view_ = nullptr;
  video_t* video_ = nullptr;
};

} // namespace semirprod
