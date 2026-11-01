#include "EventLogDock.hpp"
#include "SemirprodDock.hpp"
#include "SemirprodTheme.hpp"

#include <QAbstractItemView>
#include <QBoxLayout>
#include <QCheckBox>
#include <QColor>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QScrollBar>
#include <QVBoxLayout>
#include "../Locale.hpp"

namespace semirprod {

namespace {
namespace ss = ::semirprod::style;

// Au-dela, le journal n'apporte plus rien a un operateur (et une session de
// 8 h ne doit pas faire gonfler la memoire d'OBS).
constexpr int kMaxEntries = 300;

// Role de donnees portant le niveau, pour re-filtrer sans re-parser le texte.
constexpr int kLevelRole = Qt::UserRole + 1;

QColor colorForLevel(const QString& level) {
  // Memes teintes que le theme QSS (--success / --warning / --destructive) :
  // les listes Qt ne suivent pas les selecteurs QSS par item, on pose donc la
  // couleur a la main.
  if (level == QStringLiteral("success")) return QColor(0x2E, 0xB8, 0x8A);
  if (level == QStringLiteral("warn")) return QColor(0xE8, 0x8C, 0x30);
  if (level == QStringLiteral("error")) return QColor(0xE2, 0x36, 0x70);
  return QColor(0xFA, 0xFA, 0xFA);
}

bool isIncident(const QString& level) {
  return level == QStringLiteral("warn") || level == QStringLiteral("error");
}

} // namespace

EventLogDock::EventLogDock(SemirprodDock* dock, QWidget* parent)
    : QWidget(parent) {
  ss::applyDockTheme(this);

  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(12, 12, 12, 12);
  root->setSpacing(10);

  auto* card = ss::makeCard(this, 12);
  auto* cl = qobject_cast<QBoxLayout*>(card->layout());

  // Sans parent : ce layout est destine a etre IMBRIQUE dans celui de la carte
  // (cl->addLayout plus bas). Lui passer `card` tentait d'installer un second
  // layout sur un widget qui en a deja un — Qt le refuse et le signale
  // (« QLayout: Attempting to add QLayout ... which already has a layout »).
  auto* headerRow = ss::hLayout(8);
  headerRow->addWidget(ss::makeSectionHeader(loc("Log.Header"), card));
  headerRow->addStretch(1);
  auto* clearButton = ss::makeGhostButton(loc("Log.Clear"), card);
  headerRow->addWidget(clearButton);
  cl->addLayout(headerRow);

  // Filtre : pendant un direct, un operateur ne veut souvent que ce qui a mal
  // tourne. Coche persistee nulle part — c'est un reglage de coup d'oeil.
  onlyIncidents_ =
      new QCheckBox(loc("Log.OnlyIncidents"), card);
  cl->addWidget(onlyIncidents_);

  emptyLabel_ = new QLabel(
      loc("Log.Empty"), card);
  emptyLabel_->setObjectName(QStringLiteral("semirprodMuted"));
  emptyLabel_->setWordWrap(true);
  cl->addWidget(emptyLabel_);

  list_ = new QListWidget(card);
  list_->setFrameShape(QFrame::NoFrame);
  list_->setSelectionMode(QAbstractItemView::NoSelection);
  list_->setFocusPolicy(Qt::NoFocus);
  list_->setWordWrap(true);
  list_->setUniformItemSizes(false);
  list_->setStyleSheet(QStringLiteral("background: transparent;"));
  list_->hide();
  cl->addWidget(list_, 1);

  root->addWidget(card, 1);

  connect(clearButton, &QPushButton::clicked, this, [this]() {
    list_->clear();
    applyFilter();
  });
  connect(onlyIncidents_, &QCheckBox::toggled, this,
          [this](bool) { applyFilter(); });

  if (dock) {
    connect(dock, &SemirprodDock::eventLogged, this, &EventLogDock::appendEvent);
  }
}

EventLogDock::~EventLogDock() = default;

void EventLogDock::appendEvent(const QDateTime& at, const QString& level,
                               const QString& message) {
  // Le scroll ne suit que si l'operateur etait deja en bas : sinon on ne veut
  // pas lui arracher sa lecture d'un evenement passe.
  QScrollBar* bar = list_->verticalScrollBar();
  const bool wasAtBottom = !bar || bar->value() >= bar->maximum() - 4;

  auto* item = new QListWidgetItem(
      QStringLiteral("%1  %2")
          .arg(at.toString(QStringLiteral("HH:mm:ss")))
          .arg(message));
  item->setForeground(colorForLevel(level));
  item->setData(kLevelRole, level);
  list_->addItem(item);

  while (list_->count() > kMaxEntries) {
    delete list_->takeItem(0);
  }

  const bool hidden = onlyIncidents_->isChecked() && !isIncident(level);
  item->setHidden(hidden);

  applyFilter();
  if (wasAtBottom && !hidden) list_->scrollToBottom();
}

void EventLogDock::applyFilter() {
  const bool onlyIncidents = onlyIncidents_->isChecked();
  int visible = 0;
  for (int i = 0; i < list_->count(); ++i) {
    QListWidgetItem* item = list_->item(i);
    const QString level = item->data(kLevelRole).toString();
    const bool hidden = onlyIncidents && !isIncident(level);
    item->setHidden(hidden);
    if (!hidden) ++visible;
  }

  list_->setVisible(visible > 0);
  emptyLabel_->setVisible(visible == 0);
  emptyLabel_->setText(
      list_->count() > 0 && visible == 0
          ? loc("Log.NoIncidents")
          : loc("Log.Empty"));
}

} // namespace semirprod
