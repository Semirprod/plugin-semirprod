#include "Settings.hpp"
#include "Config.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

#include <obs-frontend-api.h>
#include <obs-module.h>

namespace semirprod {

namespace {
constexpr auto kCollectionsKey = "sceneCollections";
constexpr auto kLiveKey = "live";
constexpr auto kBugKey = "bug";
} // namespace

Settings::Settings(QObject* parent) : QObject(parent) {
  load();

  // First run (or upgrade from a build that didn't persist these): mint a
  // UUID and pick a friendly default label. Save immediately so the
  // dashboard sees the same instanceId across restarts.
  bool needsSave = false;
  if (instanceId_.isEmpty()) {
    instanceId_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    needsSave = true;
  }
  if (instanceLabel_.isEmpty()) {
    instanceLabel_ = defaultInstanceLabel();
    needsSave = true;
  }
  if (needsSave) save();
}

QString Settings::defaultInstanceLabel() {
  const QString host = QHostInfo::localHostName();
  if (host.isEmpty()) return QStringLiteral("OBS");
  return QStringLiteral("OBS-%1").arg(host);
}

QString Settings::configFilePath() {
  // Prefer OBS module config path so the file lives next to the plugin's
  // other config (auto-created per profile). Falls back to the user's
  // AppConfigLocation if OBS doesn't return a path.
  char* modulePath = obs_module_get_config_path(obs_current_module(), Config::configFileName);
  QString path;
  if (modulePath) {
    path = QString::fromUtf8(modulePath);
    bfree(modulePath);
  } else {
    path = QDir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation))
               .filePath(Config::configFileName);
  }
  QFileInfo(path).absoluteDir().mkpath(QStringLiteral("."));
  return path;
}

void Settings::load() {
  QFile file(configFilePath());
  if (!file.exists() || !file.open(QIODevice::ReadOnly)) return;

  const auto doc = QJsonDocument::fromJson(file.readAll());
  if (!doc.isObject()) return;
  const auto obj = doc.object();

  selectedStreamId_ = obj.value(QStringLiteral("selectedStreamId")).toString(selectedStreamId_);

  // Pre-per-collection layout: two top-level keys for the whole install.
  legacySceneLive_ = obj.value(QStringLiteral("sceneLive")).toString();
  legacySceneBug_ = obj.value(QStringLiteral("sceneBug")).toString();

  sceneMappings_.clear();
  const auto collections = obj.value(QString::fromUtf8(kCollectionsKey)).toObject();
  for (auto it = collections.constBegin(); it != collections.constEnd(); ++it) {
    const auto entry = it.value().toObject();
    sceneMappings_.insert(it.key(),
                          SceneMapping{entry.value(QString::fromUtf8(kLiveKey)).toString(),
                                       entry.value(QString::fromUtf8(kBugKey)).toString()});
  }

  autoSwitchEnabled_ = obj.value(QStringLiteral("autoSwitchEnabled")).toBool(autoSwitchEnabled_);
  previewEnabled_ = obj.value(QStringLiteral("previewEnabled")).toBool(previewEnabled_);
  instanceId_ = obj.value(QStringLiteral("instanceId")).toString(instanceId_);
  instanceLabel_ = obj.value(QStringLiteral("instanceLabel")).toString(instanceLabel_);
  skippedUpdateVersion_ =
      obj.value(QStringLiteral("skippedUpdateVersion")).toString(skippedUpdateVersion_);
  serverInstanceLabel_ =
      obj.value(QStringLiteral("serverInstanceLabel")).toString(serverInstanceLabel_);
  snoozedUpdateVersion_ =
      obj.value(QStringLiteral("snoozedUpdateVersion")).toString(snoozedUpdateVersion_);
  snoozedUpdateUntil_ = static_cast<qint64>(
      obj.value(QStringLiteral("snoozedUpdateUntil")).toDouble(snoozedUpdateUntil_));
}

void Settings::save() {
  QJsonObject obj;
  obj[QStringLiteral("selectedStreamId")] = selectedStreamId_;

  // The legacy top-level sceneLive/sceneBug keys are deliberately NOT written
  // back: once a real bucket exists they would be a second source of truth for
  // the same setting, and the whole point of the rewrite is that there is only
  // one — the bucket of the collection you are on.
  QJsonObject collections;
  for (auto it = sceneMappings_.constBegin(); it != sceneMappings_.constEnd(); ++it) {
    QJsonObject entry;
    entry[QString::fromUtf8(kLiveKey)] = it->live;
    entry[QString::fromUtf8(kBugKey)] = it->bug;
    collections[it.key()] = entry;
  }
  obj[QString::fromUtf8(kCollectionsKey)] = collections;

  obj[QStringLiteral("autoSwitchEnabled")] = autoSwitchEnabled_;
  obj[QStringLiteral("previewEnabled")] = previewEnabled_;
  obj[QStringLiteral("instanceId")] = instanceId_;
  obj[QStringLiteral("instanceLabel")] = instanceLabel_;
  obj[QStringLiteral("skippedUpdateVersion")] = skippedUpdateVersion_;
  obj[QStringLiteral("snoozedUpdateVersion")] = snoozedUpdateVersion_;
  obj[QStringLiteral("snoozedUpdateUntil")] = static_cast<double>(snoozedUpdateUntil_);
  obj[QStringLiteral("serverInstanceLabel")] = serverInstanceLabel_;

  // Ecriture ATOMIQUE (fichier temporaire puis renommage). Ce fichier porte
  // instanceId, l'identite que le plugin annonce a l'appairage : tronque par un
  // plantage d'OBS en pleine ecriture (Truncate puis write), il se relisait
  // vide au demarrage suivant, le constructeur tirait un NOUVEL UUID et
  // l'ecrivait par-dessus. L'instance changeait alors d'identite sans que
  // personne l'ait demande.
  QSaveFile file(configFilePath());
  if (!file.open(QIODevice::WriteOnly)) return;
  file.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
  if (!file.commit()) return;
  emit changed();
}

void Settings::setSkippedUpdateVersion(const QString& version) {
  if (skippedUpdateVersion_ == version) return;
  skippedUpdateVersion_ = version;
  save();
}

void Settings::snoozeUpdate(const QString& version, qint64 untilSecs) {
  snoozedUpdateVersion_ = version;
  snoozedUpdateUntil_ = untilSecs;
  save();
}

void Settings::setSelectedStreamId(const QString& id) {
  if (selectedStreamId_ == id) return;
  selectedStreamId_ = id;
  save();
}

QString Settings::activeSceneCollection() {
  char* name = obs_frontend_get_current_scene_collection();
  if (!name) return {};
  const QString collection = QString::fromUtf8(name);
  bfree(name);
  return collection;
}

Settings::SceneMapping& Settings::mappingForActiveCollection() {
  const QString collection = activeSceneCollection();
  auto it = sceneMappings_.find(collection);
  if (it == sceneMappings_.end()) {
    // First write for this collection: inherit the legacy global pair so an
    // upgrade doesn't lose the mapping the operator already had. If those names
    // belonged to a different collection they simply don't match any scene
    // here, and the dock drops any slot that has no matching scene.
    it = sceneMappings_.insert(collection, SceneMapping{legacySceneLive_, legacySceneBug_});
  }
  return *it;
}

QString Settings::sceneLive() const {
  const auto it = sceneMappings_.constFind(activeSceneCollection());
  return it != sceneMappings_.constEnd() ? it->live : legacySceneLive_;
}

QString Settings::sceneBug() const {
  const auto it = sceneMappings_.constFind(activeSceneCollection());
  return it != sceneMappings_.constEnd() ? it->bug : legacySceneBug_;
}

void Settings::setSceneLive(const QString& name) {
  SceneMapping& mapping = mappingForActiveCollection();
  if (mapping.live == name) return;
  mapping.live = name;
  save();
}

void Settings::setSceneBug(const QString& name) {
  SceneMapping& mapping = mappingForActiveCollection();
  if (mapping.bug == name) return;
  mapping.bug = name;
  save();
}

void Settings::setAutoSwitchEnabled(bool enabled) {
  if (autoSwitchEnabled_ == enabled) return;
  autoSwitchEnabled_ = enabled;
  save();
}

void Settings::setPreviewEnabled(bool enabled) {
  if (previewEnabled_ == enabled) return;
  previewEnabled_ = enabled;
  save();
}


void Settings::setInstanceLabel(const QString& label) {
  if (instanceLabel_ == label) return;
  instanceLabel_ = label;
  save();
}

void Settings::setServerInstanceLabel(const QString& label) {
  if (serverInstanceLabel_ == label) return;
  serverInstanceLabel_ = label;
  save();
}

} // namespace semirprod
