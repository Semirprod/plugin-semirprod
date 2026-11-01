#pragma once

#include <QJsonArray>
#include <QString>
#include <QStringList>

#include <atomic>
#include <map>

typedef struct obs_volmeter obs_volmeter_t;

namespace semirprod {

/** Vu-mètres des pistes audio, pour la régie.

    OBS mesure déjà chaque piste (c'est ce que montrent les barres de son
    mélangeur) : `obs_volmeter` donne magnitude et crête en dBFS. Rien de tout
    cela ne sortait du poste, donc le mixer du dashboard affichait des faders
    sans niveaux — impossible d'y voir une piste qui sature ou un micro resté
    muet.

    Un volmètre par source, créé à la demande et détruit avec le bail : mesurer
    en continu ferait tourner une analyse sur chaque piste pour personne, et
    c'est le genre de coût qu'on ne rend jamais.

    ⚠️ Le callback d'OBS arrive sur le THREAD AUDIO. Il n'écrit donc que des
    atomiques ; la lecture (snapshot) se fait depuis le thread Qt. Aucun objet
    Qt n'est touché depuis le callback. */
class AudioLevels {
public:
  AudioLevels() = default;
  ~AudioLevels();

  AudioLevels(const AudioLevels&) = delete;
  AudioLevels& operator=(const AudioLevels&) = delete;

  /** Aligne les volmètres sur la liste de pistes donnée : crée ceux qui
      manquent, détruit ceux dont la source a disparu. Idempotent — appelé à
      chaque rafraîchissement du mixer. */
  void sync(const QStringList& sourceNames);

  /** Dernières mesures : [{ name, channels: [{ magnitudeDb, peakDb }] }].
      Les valeurs sont en dBFS (0 = pleine échelle, négatif en dessous) ;
      -Infinity est ramené à -60, plancher d'affichage du mixer.

      Une entrée PAR CANAL, comme les deux barres du mélangeur d'OBS : un micro
      branché d'un seul côté ou un canal droit muet ne se voient pas sur une
      barre unique, or c'est exactement le genre de panne qu'on cherche. */
  QJsonArray snapshot() const;

  /** Détruit tous les volmètres. Appelé à l'expiration du bail et à la
      fermeture d'OBS. */
  void clear();

  bool active() const { return !meters_.empty(); }

private:
  // Nombre de canaux remontés. Deux suffisent : le mixer en affiche deux, et
  // au delà les barres deviendraient des traits.
  static constexpr int kChannels = 2;

  struct Meter {
    obs_volmeter_t* volmeter = nullptr;
    // Écrits par le thread audio, lus par le thread Qt.
    std::atomic<float> magnitudeDb[kChannels] = {{-60.0f}, {-60.0f}};
    std::atomic<float> peakDb[kChannels] = {{-60.0f}, {-60.0f}};
  };

  static void onLevels(void* param, const float magnitude[], const float peak[],
                       const float inputPeak[]);

  // `std::map` et non un vecteur : les pointeurs passés à OBS en paramètre de
  // callback doivent rester valides tant que le volmètre vit, or un vecteur
  // réalloue. Les nœuds d'un map, eux, ne bougent pas.
  std::map<QString, Meter> meters_;
};

} // namespace semirprod
