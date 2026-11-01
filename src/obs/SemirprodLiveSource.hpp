#pragma once

#include <string>

struct obs_source;
typedef struct obs_source obs_source_t;

namespace semirprod {

// Nom AFFICHE du type de source : entree du menu « + » d'OBS, et nom donne aux
// instances que le plugin cree lui-meme.
//
// Rien ne s'y accroche. Les recherches de source se font par IDENTIFIANT DE
// TYPE (voir plus bas), jamais par ce nom : l'operateur peut renommer ses
// sources, et il le fait. Ce libelle est donc librement modifiable, a une
// nuance pres — les instances DEJA posees chez les clients gardent le nom
// qu'elles portent, OBS ne renomme rien retroactivement.
constexpr const char* kSemirprodLiveSourceName = "Flux Semirprod";

// Registers the OBS source type "semirprod_live_source".
//
// CET IDENTIFIANT NE CHANGE JAMAIS. Il est ecrit dans le fichier de collection
// de chaque client : le renommer rendrait inconnues toutes les sources deja
// posees dans le parc, qui disparaitraient de leurs scenes. Il n'est jamais
// affiche, donc rien n'oblige a y toucher.
//
// After this runs, OBS shows "Flux Semirprod" in the add-source (+) menu and users can
// create instances that wrap an internal ffmpeg_source playing an HLS stream
// picked from the dock's stream list (via StreamRegistry).
void registerSemirprodLiveSource();

// Returns true when `source` is a live instance of our source type AND its
// inner ffmpeg_source is currently delivering frames (non-zero dimensions).
// Returns false for unknown sources or when the inner hasn't connected yet.
// Thread-safe; the scene watch timer on the dock polls this every 200 ms.
bool isSourceInstanceReady(obs_source_t* source);

// Returns the stream id currently selected on this source instance, or an
// empty string if the source isn't a live semirprod instance.
// Renvoie une COPIE. L ancienne signature (const char*) renvoyait le buffer
// interne d un std::string protege par un mutex deja relache au return :
// `applyStream` pouvait le reallouer sous les pieds de l appelant. Aucun
// appelant n existait, la mine n a donc jamais explose.
std::string sourceInstanceStreamId(obs_source_t* source);

// Re-resout son flux pour chaque instance vivante, apres un changement du
// registre ou du flux du poste.
//
// Une instance PUNAISEE (reglage `stream_id` autre que « auto ») garde le sien :
// changer de flux dans le panneau ne rebascule que les instances restees en
// automatique. C'est ce qui permet le multicam — une scene par camera, chaque
// source sur la sienne.
//
// Sert aussi de rattrapage : une source punaisee sur un flux absent du registre
// n'affiche rien, et se raccroche ici des que ce flux apparait.
void reapplyActiveStreamToAllInstances();

// Le flux `streamId` vient de commencer a publier : rouvre tout de suite les
// instances qui l'affichent sans recevoir d'image. Sans elle, une source ouverte
// avant la publication attendait le delai de reconnexion d'OBS. Une instance
// deja en direct n'est pas touchee. A appeler depuis le thread Qt.
void kickStalledInstances(const std::string& streamId);

struct InstanceState {
  bool anyExists = false;
  bool anyReady = false;
  int width = 0;
  int height = 0;
  // True when a ready instance's incoming feed exceeds 1080p and is being
  // capped at ingest. nativeWidth/Height carry the pre-cap dimensions so the
  // dock can notify the operator once.
  bool anyOversize = false;
  int nativeWidth = 0;
  int nativeHeight = 0;
};

// Snapshot of instance health used by the dock status panel: whether the user
// has placed any Flux Semirprod source in their scenes and whether the first
// ready one is delivering frames.
InstanceState queryInstanceState();

// Une source punaisee sur ce flux recoit-elle des images en ce moment ?
//
// C'est une connaissance LOCALE : elle ne dit pas que le flux publie chez nous,
// elle dit que ce poste le decode. Un flux qui n'est affiche nulle part rend
// donc false sans que rien ne soit en panne — le panneau doit l'ecrire ainsi.
bool isStreamDelivering(const std::string& streamId);

} // namespace semirprod
