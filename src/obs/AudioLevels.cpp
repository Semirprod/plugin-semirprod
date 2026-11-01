#include "AudioLevels.hpp"

#include <obs-module.h>
#include <obs.h>

#include <QJsonObject>

#include <algorithm>
#include <cmath>

namespace semirprod {

namespace {

// Plancher d'affichage du mixer. OBS rend -Infinity pour une piste silencieuse
// (log10(0)) : le laisser passer donnerait un `null` en JSON, que la barre du
// dashboard interpréterait comme « pas de mesure » au lieu de « silence ».
constexpr float kFloorDb = -60.0f;

float clampDb(float value) {
  if (!std::isfinite(value)) return kFloorDb;
  return std::clamp(value, kFloorDb, 0.0f);
}

} // namespace

AudioLevels::~AudioLevels() {
  clear();
}

void AudioLevels::onLevels(void* param, const float magnitude[],
                           const float peak[], const float /*inputPeak*/[]) {
  auto* meter = static_cast<Meter*>(param);
  if (!meter) return;
  // Un canal par barre, gauche et droite, comme le mélangeur d'OBS. Une source
  // mono ne remplit que le canal 0 : on recopie alors sa valeur à droite plutôt
  // que d'afficher une barre morte à côté d'une barre vivante.
  const bool mono = !std::isfinite(magnitude[1]) && !std::isfinite(peak[1]);
  for (int channel = 0; channel < kChannels; ++channel) {
    const int source = mono ? 0 : channel;
    // `store` sans verrou : ce callback tourne sur le thread audio d'OBS, et
    // le faire attendre un mutex tenu par l'interface ferait craquer le son.
    meter->magnitudeDb[channel].store(clampDb(magnitude[source]),
                                      std::memory_order_relaxed);
    meter->peakDb[channel].store(clampDb(peak[source]),
                                 std::memory_order_relaxed);
  }
}

void AudioLevels::sync(const QStringList& sourceNames) {
  // Retrait de ce qui a disparu (source supprimée, scène changée).
  for (auto it = meters_.begin(); it != meters_.end();) {
    if (sourceNames.contains(it->first)) {
      ++it;
      continue;
    }
    if (it->second.volmeter) {
      obs_volmeter_remove_callback(it->second.volmeter, &AudioLevels::onLevels,
                                   &it->second);
      obs_volmeter_detach_source(it->second.volmeter);
      obs_volmeter_destroy(it->second.volmeter);
    }
    it = meters_.erase(it);
  }

  // Ajout de ce qui est nouveau.
  for (const QString& name : sourceNames) {
    if (meters_.count(name) > 0) continue;
    obs_source_t* source = obs_get_source_by_name(name.toUtf8().constData());
    if (!source) continue;

    // OBS_FADER_LOG : la même courbe que le fader du mélangeur d'OBS, donc les
    // barres du dashboard bougent exactement comme celles du poste.
    obs_volmeter_t* volmeter = obs_volmeter_create(OBS_FADER_LOG);
    if (!volmeter) {
      obs_source_release(source);
      continue;
    }
    // `operator[]` construit le nœud AVANT l'abonnement : l'adresse passée à
    // OBS doit être définitive, et un nœud de map ne bouge plus une fois créé.
    Meter& meter = meters_[name];
    meter.volmeter = volmeter;
    obs_volmeter_add_callback(volmeter, &AudioLevels::onLevels, &meter);
    if (!obs_volmeter_attach_source(volmeter, source)) {
      blog(LOG_WARNING, "[semirprod-audio] volmetre non attache a \"%s\"",
           name.toUtf8().constData());
    }
    obs_source_release(source);
  }
}

QJsonArray AudioLevels::snapshot() const {
  QJsonArray items;
  for (const auto& [name, meter] : meters_) {
    QJsonArray channels;
    for (int channel = 0; channel < kChannels; ++channel) {
      QJsonObject values;
      values[QStringLiteral("magnitudeDb")] = static_cast<double>(
          meter.magnitudeDb[channel].load(std::memory_order_relaxed));
      values[QStringLiteral("peakDb")] = static_cast<double>(
          meter.peakDb[channel].load(std::memory_order_relaxed));
      channels.append(values);
    }
    QJsonObject entry;
    entry[QStringLiteral("name")] = name;
    entry[QStringLiteral("channels")] = channels;
    items.append(entry);
  }
  return items;
}

void AudioLevels::clear() {
  for (auto& [name, meter] : meters_) {
    if (!meter.volmeter) continue;
    obs_volmeter_remove_callback(meter.volmeter, &AudioLevels::onLevels, &meter);
    obs_volmeter_detach_source(meter.volmeter);
    obs_volmeter_destroy(meter.volmeter);
    meter.volmeter = nullptr;
  }
  meters_.clear();
}

} // namespace semirprod
