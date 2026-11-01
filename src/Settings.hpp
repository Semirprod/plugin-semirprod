#pragma once

#include <QMap>
#include <QObject>
#include <QString>

namespace semirprod {

/**
 * Non-secret user preferences. Secrets (tokens) live in TokenStore which
 * uses QtKeychain when available.
 *
 * Persisted as JSON next to OBS global.ini, loaded on module load,
 * saved on every setter. Thread-unsafe; mutate from the Qt main thread only.
 */
class Settings : public QObject {
  Q_OBJECT

public:
  explicit Settings(QObject* parent = nullptr);

  // Paths
  static QString configFilePath();

  // Selected stream that OBS is currently configured to ingest.
  QString selectedStreamId() const { return selectedStreamId_; }
  void setSelectedStreamId(const QString& id);

  // Scene mapping for the freeze-detect / switch feature, scoped to the OBS
  // scene collection that is active RIGHT NOW. Scene names are only unique
  // inside a collection, so a single global pair pointed at scenes that don't
  // exist anymore as soon as the operator switched collection: auto-switch
  // died silently (its guard bails when the current scene matches neither
  // slot), register_stream advertised a phantom scene name to the dashboard,
  // and the selectors displayed a scene that Settings didn't actually hold.
  // Resolving the collection on every access means no caller has to remember
  // to re-scope the mapping — there is no stale copy to desync.
  QString sceneLive() const;
  QString sceneBug() const;
  void setSceneLive(const QString& name);
  void setSceneBug(const QString& name);

  /** Name of the OBS scene collection the getters above are keyed on. Empty
      when the frontend can't report one yet (very early init) — the mapping
      then simply lives under one unnamed bucket. */
  static QString activeSceneCollection();

  bool autoSwitchEnabled() const { return autoSwitchEnabled_; }
  void setAutoSwitchEnabled(bool enabled);

  // Aperçus (screenshot + WebRTC) pilotés depuis le dashboard/l'app. Off =
  // le plugin refuse les start_preview / start_program_preview — pour les
  // machines où l'encodage d'aperçu coûte trop cher.
  bool previewEnabled() const { return previewEnabled_; }
  void setPreviewEnabled(bool enabled);


  // Stable identifier for this OBS plugin instance — generated as a UUID on
  // first run and persisted forever. Used by the dashboard to route control
  // commands when several OBS hosts share the same org/stream.
  QString instanceId() const { return instanceId_; }

  // « Ne plus me rappeler » de la popup de mise a jour : la version ignoree.
  // Une version plus recente la refait apparaitre.
  QString skippedUpdateVersion() const { return skippedUpdateVersion_; }
  void setSkippedUpdateVersion(const QString& version);

  // « Me rappeler plus tard » : la version reportee et l'heure (secondes epoch)
  // avant laquelle elle ne se represente pas. Persiste, pour qu'un OBS relance
  // trois fois dans la soiree ne repose pas la question trois fois.
  bool isUpdateSnoozed(const QString& version, qint64 nowSecs) const {
    return snoozedUpdateVersion_ == version && nowSecs < snoozedUpdateUntil_;
  }
  void snoozeUpdate(const QString& version, qint64 untilSecs);

  // Nom LOCAL du poste (nom de la machine par defaut, modifiable dans le
  // panneau). Annonce au serveur comme `reported_label` : ce n'est plus la
  // verite, seulement le repli quand le site n'a pas nomme le poste.
  QString instanceLabel() const { return instanceLabel_; }
  void setInstanceLabel(const QString& label);

  // Nom donne au poste sur le site (`instance_renamed`), garde entre deux
  // lancements d'OBS. Vide tant que le site ne l'a pas nomme.
  QString serverInstanceLabel() const { return serverInstanceLabel_; }
  void setServerInstanceLabel(const QString& label);
  // Nom a AFFICHER : celui du site s'il existe, sinon le nom local.
  QString displayInstanceLabel() const {
    return serverInstanceLabel_.isEmpty() ? instanceLabel_ : serverInstanceLabel_;
  }

signals:
  void changed();

private:
  // Live/Bug pair for one scene collection. Both slots default to empty so the
  // dock auto-picks from the user's actual scene list instead of pinning
  // "Live"/"Bug", which only matched a tiny minority of OBS setups and
  // silently broke auto-switch for everyone else. Empty means "not
  // configured" and is always respected as such.
  struct SceneMapping {
    QString live;
    QString bug;
  };

  void load();
  void save();
  static QString defaultInstanceLabel();
  /** Mapping bucket of the active collection, created on first write and
      seeded from the pre-per-collection global pair. */
  SceneMapping& mappingForActiveCollection();

  QString selectedStreamId_;
  // Keyed by scene collection name. QMap rather than QHash so the persisted
  // JSON keeps a stable key order and the config file stays diff-friendly.
  QMap<QString, SceneMapping> sceneMappings_;
  // Older configs held ONE global pair. Read on load and used as the fallback
  // for a collection that has no bucket yet, so an upgrade keeps the mapping
  // the operator already had; never written back, so it fades out on the
  // first save once real per-collection buckets exist.
  QString legacySceneLive_;
  QString legacySceneBug_;
  bool autoSwitchEnabled_ = true;
  bool previewEnabled_ = true;
  QString instanceId_;
  QString skippedUpdateVersion_;
  QString snoozedUpdateVersion_;
  qint64 snoozedUpdateUntil_ = 0;
  QString instanceLabel_;
  QString serverInstanceLabel_;
};

} // namespace semirprod
