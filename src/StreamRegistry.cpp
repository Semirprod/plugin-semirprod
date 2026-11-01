#include "StreamRegistry.hpp"

#include "Config.hpp"

#include <QMutexLocker>

#include <obs-module.h>

namespace semirprod {

StreamRegistry& StreamRegistry::instance() {
  static StreamRegistry r;
  return r;
}

void StreamRegistry::set(const QList<StreamEntry>& entries) {
  QMutexLocker lock(&mutex_);
  entries_ = entries;
  blog(LOG_INFO, "[semirprod-registry] set %d entries", static_cast<int>(entries.size()));
  // Les logs OBS sont régulièrement partagés en public par les utilisateurs
  // (support, forums) : jamais d'URL d'infra ni de clé de stream en clair —
  // uniquement des indicateurs de présence.
  for (const auto& e : entries) {
    blog(LOG_INFO,
         "[semirprod-registry]   id=%s label=%s playback=%s ingest=%s key=%s",
         e.id.toUtf8().constData(),
         e.label.toUtf8().constData(),
         e.srtReadUrl.isEmpty() ? "absent" : "present",
         e.rtmpUrl.isEmpty() ? "absent" : "present",
         e.streamKey.isEmpty() ? "absent" : "present");
  }
}

QList<StreamEntry> StreamRegistry::snapshot() const {
  QMutexLocker lock(&mutex_);
  return entries_;
}

void StreamRegistry::setActiveStreamId(const QString& id) {
  QMutexLocker lock(&mutex_);
  activeStreamId_ = id;
}

QString StreamRegistry::activeStreamId() const {
  QMutexLocker lock(&mutex_);
  return activeStreamId_;
}

QString StreamRegistry::playbackUrlForId(const QString& id) const {
  QMutexLocker lock(&mutex_);
  blog(LOG_INFO,
       "[semirprod-registry] playbackUrlForId(%s) — %d entries available",
       id.toUtf8().constData(), static_cast<int>(entries_.size()));
  for (const auto& e : entries_) {
    if (e.id != id) continue;
    // Même règle que set() : présence seulement, jamais les valeurs.
    blog(LOG_INFO,
         "[semirprod-registry]   matched entry playback=%s ingest=%s key=%s",
         e.srtReadUrl.isEmpty() ? "absent" : "present",
         e.rtmpUrl.isEmpty() ? "absent" : "present",
         e.streamKey.isEmpty() ? "absent" : "present");
    // L'URL rendue finit TELLE QUELLE dans l'`input` d'une source media, donc
    // dans ffmpeg, qui lit aussi `file:`, `concat:`, `subfile:`, un chemin nu
    // ou un partage reseau. Elle vient de l'API, mais une reponse alteree ne
    // doit pas suffire a faire lire un fichier du poste : seuls srt://,
    // rtmp:// et rtmps:// vers un hote de la liste blanche passent.
    QString url;
    if (!e.srtReadUrl.isEmpty()) {
      url = e.srtReadUrl;
    } else {
      if (e.rtmpUrl.isEmpty() || e.streamKey.isEmpty()) return {};
      url = e.rtmpUrl.endsWith('/') ? e.rtmpUrl + e.streamKey
                                    : e.rtmpUrl + QStringLiteral("/") + e.streamKey;
    }
    if (!Config::isAllowedIngestUrl(url)) {
      blog(LOG_WARNING,
           "[semirprod-registry]   URL de lecture refusee (schema ou hote hors liste "
           "blanche) pour id=%s",
           id.toUtf8().constData());
      return {};
    }
    return url;
  }
  blog(LOG_INFO, "[semirprod-registry]   no entry matched id=%s",
       id.toUtf8().constData());
  return {};
}

} // namespace semirprod
