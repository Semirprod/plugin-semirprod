#include "VerticalBridge.hpp"

#include "../Edition.hpp"

#include <obs.h>
#include <callback/calldata.h>
#include <callback/proc.h>
#include <callback/signal.h>

#include <QMetaObject>

namespace semirprod {

namespace {

constexpr const char* kStateSignal = "semirprod_vertical_stream_state";

QString readString(calldata_t* cd, const char* name) {
  const char* value = nullptr;
  calldata_get_string(cd, name, &value);
  return QString::fromUtf8(value ? value : "");
}

// Le vertical ne se pilote que sur l'OBS cloud. Sur le poste d'un client,
// Vertical peut etre installe pour son propre usage : le Studio fait alors
// comme s'il etait absent, sinon le site et l'app proposent un cadre et des
// commandes verticales a un poste qui n'en a pas l'offre.
bool callVertical(const char* name, calldata_t* cd) {
  if (!edition::bridgesEnabled()) return false;
  return proc_handler_call(obs_get_proc_handler(), name, cd);
}

}  // namespace

VerticalBridge::VerticalBridge(QObject* parent) : QObject(parent) {
  // signal_handler_connect sur un signal non declare ne connecte rien. Les
  // modules se chargent par ordre alphabetique, « semirprod-studio » avant
  // « semirprod-vertical » : on declare donc le signal nous-memes, avec la
  // meme signature. Le second a le declarer recoit false, sans consequence.
  signal_handler_add(obs_get_signal_handler(),
                     "void semirprod_vertical_stream_state(bool active, "
                     "string error)");
  signal_handler_connect(obs_get_signal_handler(), kStateSignal,
                         &VerticalBridge::onVerticalState, this);
  connected_ = true;
}

VerticalBridge::~VerticalBridge() {
  if (connected_)
    signal_handler_disconnect(obs_get_signal_handler(), kStateSignal,
                              &VerticalBridge::onVerticalState, this);
}

void VerticalBridge::onVerticalState(void* data, calldata_t* cd) {
  auto* self = static_cast<VerticalBridge*>(data);
  const bool active = calldata_bool(cd, "active");
  const QString error = readString(cd, "error");
  // Le signal arrive sur le fil de Vertical (UI en pratique, mais rien ne le
  // garantit) : on repasse par la file d'evenements du Studio.
  QMetaObject::invokeMethod(
      self,
      [self, active, error] {
        if (active)
          self->lastError_.clear();
        else if (!error.isEmpty())
          self->lastError_ = error;
        emit self->stateChanged();
      },
      Qt::QueuedConnection);
}

VerticalBridge::State VerticalBridge::state() const {
  State s;
  calldata_t cd = {};
  if (callVertical("semirprod_vertical_get_state",
                        &cd)) {
    s.available = true;
    s.api = int(calldata_int(&cd, "api"));
    s.streaming = calldata_bool(&cd, "streaming");
    s.configured = calldata_bool(&cd, "configured");
    s.followStream = calldata_bool(&cd, "follow_stream");
    s.server = readString(&cd, "server");
    s.lastError = s.streaming ? QString() : lastError_;
  }
  calldata_free(&cd);
  return s;
}

bool VerticalBridge::setDestination(const QString& server, const QString& key,
                                    int bitrateKbps, bool followStream,
                                    QString* error) {
  calldata_t cd = {};
  calldata_set_string(&cd, "server", server.toUtf8().constData());
  calldata_set_string(&cd, "key", key.toUtf8().constData());
  calldata_set_int(&cd, "bitrate", bitrateKbps);
  calldata_set_bool(&cd, "follow_stream", followStream);
  const bool called = callVertical("semirprod_vertical_set_destination", &cd);
  const bool ok = called && calldata_bool(&cd, "success");
  if (error)
    *error = !called ? QStringLiteral("vertical_unavailable")
                     : readString(&cd, "error");
  calldata_free(&cd);
  if (ok) lastError_.clear();
  return ok;
}

bool VerticalBridge::startStream(QString* error) {
  calldata_t cd = {};
  const bool called = callVertical("semirprod_vertical_start_stream", &cd);
  const bool ok = called && calldata_bool(&cd, "success");
  if (error)
    *error = !called ? QStringLiteral("vertical_unavailable")
                     : readString(&cd, "error");
  calldata_free(&cd);
  return ok;
}

bool VerticalBridge::startPreview(const QString& whipUrl,
                                  const QString& bearerToken, int width,
                                  int height, int fps, int bitrateKbps,
                                  QString* error) {
  calldata_t cd = {};
  calldata_set_string(&cd, "url", whipUrl.toUtf8().constData());
  calldata_set_string(&cd, "token", bearerToken.toUtf8().constData());
  calldata_set_int(&cd, "width", width);
  calldata_set_int(&cd, "height", height);
  calldata_set_int(&cd, "fps", fps);
  calldata_set_int(&cd, "bitrate", bitrateKbps);
  const bool called = callVertical("semirprod_vertical_start_preview", &cd);
  const bool ok = called && calldata_bool(&cd, "success");
  if (error)
    *error = !called ? QStringLiteral("vertical_unavailable")
                     : readString(&cd, "error");
  calldata_free(&cd);
  return ok;
}

void VerticalBridge::stopPreview() {
  calldata_t cd = {};
  callVertical("semirprod_vertical_stop_preview",
                    &cd);
  calldata_free(&cd);
}

bool VerticalBridge::previewSupported() {
  calldata_t cd = {};
  const bool called = callVertical("semirprod_vertical_get_state", &cd);
  const long long api = called ? calldata_int(&cd, "api") : 0;
  calldata_free(&cd);
  return api >= 2;
}

void VerticalBridge::stopStream() {
  calldata_t cd = {};
  callVertical("semirprod_vertical_stop_stream",
                    &cd);
  calldata_free(&cd);
}

}  // namespace semirprod
