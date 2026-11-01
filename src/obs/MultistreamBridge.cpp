#include "MultistreamBridge.hpp"

#include "../Edition.hpp"

#include <obs.h>
#include <callback/calldata.h>
#include <callback/proc.h>
#include <callback/signal.h>

#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>

namespace semirprod {

namespace {

constexpr const char* kStateSignal = "semirprod_multistream_state";

QString readString(calldata_t* cd, const char* name) {
  const char* value = nullptr;
  calldata_get_string(cd, name, &value);
  return QString::fromUtf8(value ? value : "");
}

// La regie n'existe que sur l'OBS cloud : sur le poste d'un client,
// Multistream reste son outil local et le Studio fait comme s'il etait absent.
bool callMultistream(const char* name, calldata_t* cd) {
  if (!edition::bridgesEnabled()) return false;
  return proc_handler_call(obs_get_proc_handler(), name, cd);
}

}  // namespace

MultistreamBridge::MultistreamBridge(QObject* parent) : QObject(parent) {
  // Meme precaution que VerticalBridge : declarer le signal nous-memes, le
  // second a le faire recoit false sans consequence.
  signal_handler_add(obs_get_signal_handler(), "void semirprod_multistream_state()");
  signal_handler_connect(obs_get_signal_handler(), kStateSignal,
                         &MultistreamBridge::onState, this);
  connected_ = true;
}

MultistreamBridge::~MultistreamBridge() {
  if (connected_)
    signal_handler_disconnect(obs_get_signal_handler(), kStateSignal,
                              &MultistreamBridge::onState, this);
}

void MultistreamBridge::onState(void* data, calldata_t*) {
  auto* self = static_cast<MultistreamBridge*>(data);
  QMetaObject::invokeMethod(self, [self] { emit self->stateChanged(); },
                            Qt::QueuedConnection);
}

bool MultistreamBridge::available() {
  calldata_t cd = {};
  const bool ok = callMultistream("semirprod_multistream_get_state", &cd) &&
                  calldata_int(&cd, "api") >= 1;
  calldata_free(&cd);
  return ok;
}

QJsonValue MultistreamBridge::state() const {
  calldata_t cd = {};
  QJsonValue out(QJsonValue::Null);
  if (callMultistream("semirprod_multistream_get_state", &cd)) {
    const QJsonDocument doc = QJsonDocument::fromJson(readString(&cd, "json").toUtf8());
    if (doc.isObject()) out = doc.object();
  }
  calldata_free(&cd);
  return out;
}

bool MultistreamBridge::setDestinations(const QJsonArray& destinations, QString* error) {
  calldata_t cd = {};
  const QByteArray json = QJsonDocument(destinations).toJson(QJsonDocument::Compact);
  calldata_set_string(&cd, "json", json.constData());
  const bool called = callMultistream("semirprod_multistream_set_destinations", &cd);
  const bool ok = called && calldata_bool(&cd, "success");
  if (error)
    *error = !called ? QStringLiteral("multistream_unavailable") : readString(&cd, "error");
  calldata_free(&cd);
  return ok;
}

bool MultistreamBridge::start(const QString& id, QString* error) {
  calldata_t cd = {};
  calldata_set_string(&cd, "id", id.toUtf8().constData());
  const bool called = callMultistream("semirprod_multistream_start", &cd);
  const bool ok = called && calldata_bool(&cd, "success");
  if (error)
    *error = !called ? QStringLiteral("multistream_unavailable") : readString(&cd, "error");
  calldata_free(&cd);
  return ok;
}

void MultistreamBridge::stop(const QString& id) {
  calldata_t cd = {};
  calldata_set_string(&cd, "id", id.toUtf8().constData());
  callMultistream("semirprod_multistream_stop", &cd);
  calldata_free(&cd);
}

}  // namespace semirprod
