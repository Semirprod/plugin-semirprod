#pragma once

#include <QList>
#include <QMutex>
#include <QString>

namespace semirprod {

struct StreamEntry {
  QString id;
  QString label;
  QString rtmpUrl;
  QString srtReadUrl;
  QString streamKey;
};

// Thread-safe singleton bridging the SemirprodDock (which owns auth + HTTP) and
// the OBS source-type registered in SemirprodLiveSource. The dock pushes the
// latest stream list after each API refresh; the source-type reads a snapshot
// when its properties dialog is opened.
class StreamRegistry {
public:
  static StreamRegistry& instance();

  void set(const QList<StreamEntry>& entries);
  QList<StreamEntry> snapshot() const;

  // The stream id the dock is currently pointing at. Custom source instances
  // read this on create/update to stay in sync with the user's dock choice
  // instead of keeping their own per-instance selector.
  void setActiveStreamId(const QString& id);
  QString activeStreamId() const;

  // Returns the preferred playback URL for `id`. SRT (UDP, <1 s latency, ARQ
  // loss recovery) is used when the backend advertises an srtReadUrl — it
  // handles flaky networks (4G/5G IRL) much better than RTMP. Falls back to
  // "<rtmpUrl>/<streamKey>" when the SRT URL is missing (e.g. older backends).
  QString playbackUrlForId(const QString& id) const;

private:
  StreamRegistry() = default;
  mutable QMutex mutex_;
  QList<StreamEntry> entries_;
  QString activeStreamId_;
};

} // namespace semirprod
