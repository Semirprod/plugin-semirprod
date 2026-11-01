#include "StreamStatusDock.hpp"
#include "SemirprodDock.hpp"
#include "SemirprodTheme.hpp"

#include "../StreamRegistry.hpp"
#include "../obs/SemirprodLiveSource.hpp"
#include "../ws/LiveClient.hpp"

#include <QBoxLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>
#include "../Locale.hpp"

namespace semirprod {

namespace {
namespace ss = ::semirprod::style;

// Un debit se lit mieux en Mb/s des qu'il depasse le millier — c'est l'unite
// dans laquelle les operateurs raisonnent.
QString formatBitrate(int kbps) {
  if (kbps <= 0) return QStringLiteral("0 kb/s");
  if (kbps >= 1000) {
    return QStringLiteral("%1 Mb/s").arg(kbps / 1000.0, 0, 'f', 1);
  }
  return QStringLiteral("%1 kb/s").arg(kbps);
}

QString formatUptime(const QDateTime& start) {
  if (!start.isValid()) return QString();
  const qint64 sec = start.secsTo(QDateTime::currentDateTimeUtc());
  if (sec < 0) return QString();
  const qint64 h = sec / 3600;
  const qint64 m = (sec % 3600) / 60;
  if (h > 0) return QStringLiteral("%1 h %2").arg(h).arg(m, 2, 10, QChar('0'));
  return QStringLiteral("%1 min %2").arg(m).arg(sec % 60, 2, 10, QChar('0'));
}

} // namespace

StreamStatusDock::StreamStatusDock(SemirprodDock* dock, LiveClient* live,
                                   QWidget* parent)
    : QWidget(parent),
      live_(live),
      timer_(new QTimer(this)) {
  // Scope the Semirprod styling to this dock only (this dock is a sibling of
  // the main dock, so it needs its own sheet — the theme is never applied
  // qApp-wide, which would restyle all of OBS).
  ss::applyDockTheme(this);

  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(12, 12, 12, 12);
  root->setSpacing(10);

  // --- Bandeau d'alerte ------------------------------------------------------
  // En tete, au-dessus de tout : quand la diffusion se degrade, c'est la seule
  // chose que l'operateur doit voir. Masque le reste du temps.
  healthBanner_ = ss::makeCard(this, 10);
  healthBanner_->setProperty("semirprodTone", QStringLiteral("danger"));
  // La feuille QSS est deja resolue a ce stade : sans repolissage, le
  // selecteur [semirprodTone="danger"] ne prendrait pas.
  healthBanner_->style()->unpolish(healthBanner_);
  healthBanner_->style()->polish(healthBanner_);
  {
    auto* bl = qobject_cast<QBoxLayout*>(healthBanner_->layout());
    bl->setSpacing(2);
    healthTitle_ = new QLabel(loc("Health.Title"), healthBanner_);
    healthTitle_->setObjectName(QStringLiteral("semirprodError"));
    healthDetail_ = new QLabel(healthBanner_);
    healthDetail_->setObjectName(QStringLiteral("semirprodMuted"));
    healthDetail_->setWordWrap(true);
    bl->addWidget(healthTitle_);
    bl->addWidget(healthDetail_);
  }
  healthBanner_->hide();
  root->addWidget(healthBanner_);

  // --- Etat des liaisons -----------------------------------------------------
  auto* statusCard = ss::makeCard(this, 12);
  {
    auto* cl = qobject_cast<QBoxLayout*>(statusCard->layout());
    cl->addWidget(ss::makeSectionHeader(loc("Status.Header"), statusCard));

    auto addRow = [&](QLabel* dot, QLabel* value, QLabel* muted) {
      // Sans parent : ce layout est imbrique dans celui de la carte (voir
      // EventLogDock pour le detail). Avec `statusCard`, Qt refusait le second
      // layout et le signalait a chaque appel — trois fois ici.
      auto* row = ss::hLayout(10);
      row->addWidget(dot, 0, Qt::AlignVCenter);
      auto* v = new QVBoxLayout();
      v->setContentsMargins(0, 0, 0, 0);
      v->setSpacing(1);
      v->addWidget(value);
      v->addWidget(muted);
      row->addLayout(v, 1);
      cl->addLayout(row);
    };

    // Backend WS
    connDot_ = ss::makeStatusDot(statusCard);
    connText_ = new QLabel(statusCard);
    connText_->setObjectName(QStringLiteral("semirprodBodyEmph"));
    auto* connSub = new QLabel(statusCard);
    connSub->setObjectName(QStringLiteral("semirprodMuted"));
    connSub->setText(loc("Status.Link"));
    addRow(connDot_, connText_, connSub);

    // Publisher (MediaMTX)
    publisherDot_ = ss::makeStatusDot(statusCard);
    publisherText_ = new QLabel(statusCard);
    publisherText_->setObjectName(QStringLiteral("semirprodBodyEmph"));
    auto* pubSub = new QLabel(statusCard);
    pubSub->setObjectName(QStringLiteral("semirprodMuted"));
    pubSub->setText(loc("Status.Remote"));
    addRow(publisherDot_, publisherText_, pubSub);

    // SRT reception — la ligne « muted » porte les dimensions quand le flux
    // arrive, sinon le libelle de la liaison.
    sourceDot_ = ss::makeStatusDot(statusCard);
    sourceText_ = new QLabel(statusCard);
    sourceText_->setObjectName(QStringLiteral("semirprodBodyEmph"));
    dimensionsLabel_ = new QLabel(statusCard);
    dimensionsLabel_->setObjectName(QStringLiteral("semirprodMuted"));
    dimensionsLabel_->setText(loc("Status.Receiving"));
    addRow(sourceDot_, sourceText_, dimensionsLabel_);
  }
  root->addWidget(statusCard);

  // --- Chiffres du direct ----------------------------------------------------
  auto* statsCard = ss::makeCard(this, 12);
  {
    auto* cl = qobject_cast<QBoxLayout*>(statsCard->layout());
    cl->addWidget(ss::makeSectionHeader(loc("Live.Header"), statsCard));

    statsIdleLabel_ =
        new QLabel(loc("Live.Idle"), statsCard);
    statsIdleLabel_->setObjectName(QStringLiteral("semirprodMuted"));
    statsIdleLabel_->setWordWrap(true);
    cl->addWidget(statsIdleLabel_);

    // Grille 2 colonnes : tient dans une colonne OBS etroite sans rogner les
    // valeurs, et reste lisible si l'operateur elargit le dock.
    statsGrid_ = new QWidget(statsCard);
    auto* grid = new QGridLayout(statsGrid_);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(12);
    grid->setVerticalSpacing(10);

    auto addTile = [&](int row, int col, const QString& caption,
                       QLabel** valueOut) {
      auto* box = new QVBoxLayout();
      box->setContentsMargins(0, 0, 0, 0);
      box->setSpacing(1);
      auto* cap = new QLabel(caption, statsGrid_);
      cap->setObjectName(QStringLiteral("semirprodSectionHeader"));
      auto* val = new QLabel(QString(), statsGrid_);
      val->setObjectName(QStringLiteral("semirprodBodyEmph"));
      box->addWidget(cap);
      box->addWidget(val);
      grid->addLayout(box, row, col);
      *valueOut = val;
    };

    addTile(0, 0, loc("Live.Bitrate"), &bitrateValue_);
    addTile(0, 1, loc("Live.Duration"), &uptimeValue_);
    addTile(1, 0, loc("Live.Dropped"), &droppedValue_);
    addTile(1, 1, loc("Live.Congestion"), &congestionValue_);
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 1);

    statsGrid_->hide();
    cl->addWidget(statsGrid_);
  }
  root->addWidget(statsCard);

  root->addStretch(1);

  if (live_) {
    wsConnected_ = live_->isConnected();
    connect(live_, &LiveClient::connectionChanged,
            this, &StreamStatusDock::onConnectionChanged);
    connect(live_, &LiveClient::publishStateChanged,
            this, &StreamStatusDock::onPublishStateChanged);
  }
  if (dock) {
    connect(dock, &SemirprodDock::stateChanged,
            this, &StreamStatusDock::onStateChanged);
  }

  connect(timer_, &QTimer::timeout, this, &StreamStatusDock::refresh);
  timer_->start(500);
  refresh();
}

StreamStatusDock::~StreamStatusDock() = default;

void StreamStatusDock::onConnectionChanged(bool connected) {
  wsConnected_ = connected;
  if (!connected) publishing_ = false;
  refresh();
}

void StreamStatusDock::onPublishStateChanged(const QString& streamId,
                                             bool publishing) {
  Q_UNUSED(streamId);
  publishing_ = publishing;
  refresh();
}

void StreamStatusDock::onStateChanged(const QJsonObject& state) {
  healthAlert_ = state.value(QStringLiteral("healthAlert")).toBool();
  healthReason_ = state.value(QStringLiteral("healthReason")).toString();
  streaming_ = state.value(QStringLiteral("streaming")).toBool();
  statsActive_ = state.value(QStringLiteral("statsActive")).toBool();
  statsKbps_ = state.value(QStringLiteral("statsKbps")).toInt();
  droppedFrames_ = state.value(QStringLiteral("statsDroppedFrames")).toInt();
  totalFrames_ = state.value(QStringLiteral("statsTotalFrames")).toInt();
  congestion_ = state.value(QStringLiteral("statsCongestion")).toDouble();
  // Horodatage emis en UTC (currentDateTimeUtc + Qt::ISODateWithMs, donc
  // suffixe « Z »). On garantit ce suffixe avant de parser : sans zone dans la
  // chaine, Qt tagge le QDateTime en heure locale et l'uptime derape de
  // l'offset du fuseau. Le faire ici plutot qu'apres coup evite setTimeSpec()
  // (deprecie) et setTimeZone(QTimeZone::UTC) (Qt >= 6.5 seulement, alors que
  // le plugin doit compiler contre le Qt embarque par OBS 30).
  QString startedAt = state.value(QStringLiteral("streamingStartedAt")).toString();
  const int tPos = startedAt.indexOf(QLatin1Char('T'));
  if (tPos >= 0) {
    const QString timePart = startedAt.mid(tPos + 1);
    const bool hasZone = timePart.contains(QLatin1Char('Z'))
                      || timePart.contains(QLatin1Char('+'))
                      || timePart.contains(QLatin1Char('-'));
    if (!hasZone) startedAt.append(QLatin1Char('Z'));
  }
  streamingStartedAt_ = QDateTime::fromString(startedAt, Qt::ISODateWithMs);

  applyHealthBanner();
  applyStats();
}

void StreamStatusDock::applyHealthBanner() {
  healthBanner_->setVisible(healthAlert_);
  if (!healthAlert_) return;
  healthDetail_->setText(
      healthReason_.isEmpty()
          ? loc("Health.Fallback")
          : loc("Health.Detail")
                .arg(healthReason_));
}

void StreamStatusDock::applyStats() {
  const bool live = streaming_ || statsActive_;
  statsGrid_->setVisible(live);
  statsIdleLabel_->setVisible(!live);
  if (!live) return;

  bitrateValue_->setText(formatBitrate(statsKbps_));
  uptimeValue_->setText(formatUptime(streamingStartedAt_));

  if (totalFrames_ > 0) {
    const double pct =
        (static_cast<double>(droppedFrames_) / totalFrames_) * 100.0;
    droppedValue_->setText(QStringLiteral("%1 %").arg(pct, 0, 'f', 1));
    // Les seuils reprennent ceux de l'alerte sante : meme vocabulaire visuel
    // entre le chiffre et le bandeau.
    droppedValue_->setObjectName(pct >= 5.0
                                     ? QStringLiteral("semirprodError")
                                     : QStringLiteral("semirprodBodyEmph"));
  } else {
    droppedValue_->setText(QStringLiteral("0 %"));
    droppedValue_->setObjectName(QStringLiteral("semirprodBodyEmph"));
  }

  const int congestionPct = static_cast<int>(congestion_ * 100);
  congestionValue_->setText(QStringLiteral("%1 %").arg(congestionPct));
  congestionValue_->setObjectName(congestionPct >= 50
                                      ? QStringLiteral("semirprodError")
                                      : QStringLiteral("semirprodBodyEmph"));

  // Un objectName change ne re-declenche pas le selecteur QSS tout seul.
  for (QLabel* l : {droppedValue_, congestionValue_}) {
    l->style()->unpolish(l);
    l->style()->polish(l);
  }
}

void StreamStatusDock::refresh() {
  // Backend WS
  ss::setStatusState(connDot_, wsConnected_ ? "online" : "offline",
                     /*animated=*/!wsConnected_);
  connText_->setText(wsConnected_ ? loc("Status.Connected")
                                  : loc("Status.Disconnected"));

  // Publisher — "unknown" state uses the muted dot color and pulses while
  // we're awaiting the first publish-state event.
  if (!wsConnected_) {
    ss::setStatusState(publisherDot_, "unknown", /*animated=*/true);
    publisherText_->setText(loc("Status.Unknown"));
  } else {
    ss::setStatusState(publisherDot_, publishing_ ? "online" : "offline",
                       /*animated=*/false);
    publisherText_->setText(publishing_ ? loc("Status.Publishing")
                                        : loc("Status.Waiting"));
  }

  // SRT receive
  InstanceState s = queryInstanceState();
  if (!s.anyExists) {
    ss::setStatusState(sourceDot_, "unknown", /*animated=*/true);
    sourceText_->setText(loc("Source.NotAdded"));
    dimensionsLabel_->setText(loc("Status.Receiving"));
  } else if (s.anyReady) {
    ss::setStatusState(sourceDot_, "online");
    sourceText_->setText(loc("Source.LiveState"));
    dimensionsLabel_->setText(
        QStringLiteral("%1 × %2 px").arg(s.width).arg(s.height));
  } else {
    ss::setStatusState(sourceDot_, "busy", /*animated=*/true);
    sourceText_->setText(loc("Source.WaitingStream"));
    dimensionsLabel_->setText(loc("Status.Receiving"));
  }

  // La duree avance seule entre deux instantanes (pousses toutes les 2 s).
  if (streaming_ || statsActive_) {
    uptimeValue_->setText(formatUptime(streamingStartedAt_));
  }
}

} // namespace semirprod
