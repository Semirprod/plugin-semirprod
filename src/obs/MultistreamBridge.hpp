#pragma once

#include <QJsonArray>
#include <QJsonValue>
#include <QObject>
#include <QString>

// Type C de libobs (callback/calldata.h), declare hors du namespace.
struct calldata;

namespace semirprod {

// Pilote Semirprod Multistream, s'il est installe, par le proc handler global
// de libobs (contrat : src/RemoteApi.hpp du depot semirprod-obs-multistream).
//
// L'edition « regie » de l'OBS cloud : plusieurs chaines, chacune sur son
// canvas. Les chaines viennent du profil choisi sur le dashboard ; le Studio
// ne fait que relayer. Aucune dependance de compilation entre les plugins.
//
// Actif sur l'OBS cloud seulement (edition::bridgesEnabled, Edition.hpp),
// comme VerticalBridge.
class MultistreamBridge : public QObject {
  Q_OBJECT

 public:
  explicit MultistreamBridge(QObject* parent = nullptr);
  ~MultistreamBridge() override;

  // Multistream charge et pilotable : condition de la capacite `multistream`.
  static bool available();

  // { canvases, destinations } tel que Multistream le publie (jamais de
  // cle), ou null si Multistream est absent.
  QJsonValue state() const;

  // Remplace les chaines du dashboard. Les cles ne sont ni journalisees ni
  // gardees ici. `error` = "multistream_unavailable", "streaming_active" ou
  // le message de Multistream.
  bool setDestinations(const QJsonArray& destinations, QString* error);
  // `id` vide = toutes.
  bool start(const QString& id, QString* error);
  void stop(const QString& id);

 signals:
  // Fil UI. Liste, canvas ou etat d'une sortie ont change.
  void stateChanged();

 private:
  static void onState(void* data, ::calldata* cd);
  bool connected_ = false;
};

}  // namespace semirprod
