#include "SemirprodTheme.hpp"

// QGraphicsDropShadowEffect / QLinearGradient / QPainter / QPixmap sont partis
// avec addSoftShadow() et brandGradientDataUri(). QStyle reste indispensable :
// style()->unpolish() exige le type complet.
#include <QFrame>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>
#include <QWidget>

namespace semirprod::style {

namespace {

// Single source of truth for the global QSS. Kept as a long literal so the
// applied stylesheet can be diffed visually — we intentionally do NOT build
// it via QString concatenation so a search for `background-color` lands
// every site at once.
constexpr const char* kGlobalSheet = R"QSS(
/* ---------- Reset & defaults ---------- */
QWidget {
  background-color: transparent;
  color: #FAFAFA;
  font-family: 'Figtree', 'Segoe UI', 'Inter', 'Helvetica Neue', Roboto, Arial, sans-serif;
  font-size: 14px;
}

/* The root frame under our docks. We restate the background here so any
   widget created without an explicit parent (tooltips, dialogs) still
   pulls the right palette. */
QWidget#semirprodRoot {
  background-color: #000000;
}

/* ---------- Cards ----------
   Border-only by default: on a pure-black canvas a filled grey box reads as
   decoration, not structure. The fill is reserved for the tones that carry
   meaning (elevated / info / warn / danger / success). */
QFrame#semirprodCard {
  background-color: transparent;
  border: 1px solid #262626;
  border-radius: 8px;
}
QFrame#semirprodCard[semirprodTone="elevated"] {
  background-color: transparent;
  border-color: #262626;
}
QFrame#semirprodCard[semirprodTone="info"] {
  background-color: rgba(38, 98, 217, 0.10);
  border-color: rgba(38, 98, 217, 0.40);
}
QFrame#semirprodCard[semirprodTone="warn"] {
  background-color: rgba(232, 140, 48, 0.10);
  border-color: rgba(232, 140, 48, 0.40);
}
QFrame#semirprodCard[semirprodTone="danger"] {
  background-color: rgba(226, 54, 112, 0.10);
  border-color: rgba(226, 54, 112, 0.40);
}
QFrame#semirprodCard[semirprodTone="success"] {
  background-color: rgba(46, 184, 138, 0.10);
  border-color: rgba(46, 184, 138, 0.40);
}

/* ---------- Header bar ---------- */
QFrame#semirprodHeader {
  background-color: #000000;
  border-bottom: 1px solid #262626;
}
QFrame#semirprodHeaderDevBuild {
  background-color: rgba(232, 140, 48, 0.10);
  border-bottom: 1px solid rgba(232, 140, 48, 0.40);
}

QLabel#semirprodBrand {
  color: #FAFAFA;
  font-size: 16px;
  font-weight: 700;
  letter-spacing: -0.2px;
}
/* The site has no accent hue, so the second half of the wordmark is muted
   rather than colored — same trick the dashboard sidebar uses. */
QLabel#semirprodBrandSub {
  color: #8C8C8C;
  font-size: 16px;
  font-weight: 700;
  letter-spacing: -0.2px;
}
QLabel#semirprodVersion {
  color: #6B6B6B;
  font-size: 12px;
  font-family: 'Geist Mono', 'JetBrains Mono', 'Consolas', monospace;
}
QLabel#semirprodDevBadge {
  color: #E88C30;
  background-color: rgba(232, 140, 48, 0.12);
  border: 1px solid rgba(232, 140, 48, 0.40);
  border-radius: 999px;
  padding: 2px 10px;
  font-size: 12px;
  font-weight: 600;
}

/* ---------- Sidebar nav ---------- */
QFrame#semirprodSidebar {
  background-color: #000000;
  border-right: 1px solid #262626;
}
QPushButton#semirprodNavItem {
  background-color: transparent;
  color: #8C8C8C;
  border: none;
  border-radius: 6px;
  padding: 9px 12px;
  text-align: left;
  font-size: 14px;
  font-weight: 500;
}
QPushButton#semirprodNavItem:hover {
  background-color: rgba(255, 255, 255, 0.06);
  color: #FAFAFA;
}
QPushButton#semirprodNavItem:checked,
QPushButton#semirprodNavItem[semirprodActive="true"] {
  background-color: rgba(255, 255, 255, 0.08);
  color: #FAFAFA;
  font-weight: 600;
}

/* ---------- Section / labels ---------- */
QLabel#semirprodSectionHeader {
  color: #8C8C8C;
  font-size: 12px;
  font-weight: 600;
  letter-spacing: 0.4px;
  text-transform: uppercase;
}
QLabel#semirprodFieldLabel {
  color: #8C8C8C;
  font-size: 14px;
  font-weight: 500;
}
QLabel#semirprodHero {
  color: #FAFAFA;
  font-size: 24px;
  font-weight: 600;
  letter-spacing: -0.4px;
}
QLabel#semirprodHeroMuted {
  color: #8C8C8C;
  font-size: 14px;
  font-weight: 400;
}
QLabel#semirprodMuted {
  color: #8C8C8C;
  font-size: 12px;
}
QLabel#semirprodBody {
  color: #FAFAFA;
  font-size: 14px;
}
QLabel#semirprodBodyEmph {
  color: #FAFAFA;
  font-size: 14px;
  font-weight: 600;
}
QLabel#semirprodPairingCode {
  color: #FAFAFA;
  font-family: 'Geist Mono', 'JetBrains Mono', 'Consolas', 'SFMono-Regular', monospace;
  font-size: 28px;
  font-weight: 600;
  letter-spacing: 6px;
}
QLabel#semirprodPairingHint {
  color: #8C8C8C;
  font-size: 12px;
  font-weight: 500;
  letter-spacing: 0.4px;
  text-transform: uppercase;
}
QLabel#semirprodError {
  color: #E23670;
  font-size: 14px;
  font-weight: 500;
}
QLabel#semirprodSuccess {
  color: #2EB88A;
  font-size: 14px;
  font-weight: 500;
}
QLabel#semirprodWarning {
  color: #E88C30;
  font-size: 14px;
  font-weight: 500;
}

/* ---------- Status dots ---------- */
QLabel#semirprodStatusDot {
  border-radius: 5px;
}
QLabel#semirprodStatusDot[semirprodState="online"]   { background-color: #2EB88A; }
QLabel#semirprodStatusDot[semirprodState="busy"]     { background-color: #E88C30; }
QLabel#semirprodStatusDot[semirprodState="offline"]  { background-color: #E23670; }
QLabel#semirprodStatusDot[semirprodState="unknown"]  { background-color: #3A3A3A; }
QLabel#semirprodStatusDot[semirprodState="warn"]     { background-color: #E88C30; }
QLabel#semirprodStatusDot[semirprodState="accent"]   { background-color: #FAFAFA; }
/* Le battement n'est PAS pilote par la feuille de style : QSS ne sait pas
   animer. C'est un QGraphicsOpacityEffect anime cote C++ (voir
   pulseAnimationFor). Une regle vide s'appuyant sur une propriete
   `semirprodPulse` vivait ici et ne servait a rien — elle laissait croire que
   le selecteur faisait quelque chose. */

/* ---------- Pills (site Badge: pill, 12px, 600) ---------- */
QLabel#semirprodPill {
  border-radius: 999px;
  padding: 2px 10px;
  font-size: 12px;
  font-weight: 600;
}
QLabel#semirprodPill[semirprodTone="neutral"] {
  color: #FAFAFA;
  background-color: transparent;
  border: 1px solid #262626;
}
QLabel#semirprodPill[semirprodTone="info"] {
  color: #7CA5F0;
  background-color: rgba(38, 98, 217, 0.16);
}
QLabel#semirprodPill[semirprodTone="success"] {
  color: #2EB88A;
  background-color: rgba(46, 184, 138, 0.16);
}
QLabel#semirprodPill[semirprodTone="warn"] {
  color: #E88C30;
  background-color: rgba(232, 140, 48, 0.16);
}
QLabel#semirprodPill[semirprodTone="danger"] {
  color: #E23670;
  background-color: rgba(226, 54, 112, 0.16);
}

/* ---------- Buttons ----------
   Default = the site's `outline` variant (transparent on black, hairline
   border, muted-grey hover fill). */
QPushButton {
  background-color: transparent;
  color: #FAFAFA;
  border: 1px solid #262626;
  border-radius: 6px;
  padding: 8px 16px;
  font-size: 14px;
  font-weight: 500;
  min-height: 22px;
  text-align: center;
}
QPushButton:hover    { background-color: rgba(255, 255, 255, 0.06); }
QPushButton:pressed  { background-color: rgba(255, 255, 255, 0.10); }
QPushButton:disabled { color: #6B6B6B; background-color: transparent; border-color: #1A1A1A; }
QPushButton:focus    { outline: none; border-color: #CCCCCC; }

/* Primary = the site's default Button: INVERTED — near-white fill, near-black
   label. This is the whole emphasis system; there is no brand hue to lean on. */
QPushButton[semirprodVariant="primary"] {
  background-color: #FAFAFA;
  color: #171717;
  border: 1px solid #FAFAFA;
  font-weight: 500;
}
QPushButton[semirprodVariant="primary"]:hover    { background-color: #E1E1E1; border-color: #E1E1E1; }
QPushButton[semirprodVariant="primary"]:pressed  { background-color: #C9C9C9; border-color: #C9C9C9; }
QPushButton[semirprodVariant="primary"]:disabled { background-color: transparent; color: #6B6B6B; border-color: #262626; }

QPushButton[semirprodVariant="secondary"] {
  background-color: transparent;
  color: #FAFAFA;
  border: 1px solid #262626;
}
QPushButton[semirprodVariant="secondary"]:hover { background-color: rgba(255, 255, 255, 0.06); }
QPushButton[semirprodVariant="secondary"]:disabled { color: #6B6B6B; background-color: transparent; }

QPushButton[semirprodVariant="ghost"] {
  background-color: transparent;
  color: #FAFAFA;
  border: none;
  padding: 8px 12px;
}
QPushButton[semirprodVariant="ghost"]:hover { background-color: rgba(255, 255, 255, 0.06); }
QPushButton[semirprodVariant="ghost"]:disabled { color: #6B6B6B; }

QPushButton[semirprodVariant="icon"] {
  background-color: transparent;
  color: #8C8C8C;
  border: 1px solid transparent;
  border-radius: 6px;
  padding: 6px;
  min-width: 28px;
  min-height: 28px;
  font-size: 14px;
}
QPushButton[semirprodVariant="icon"]:hover {
  background-color: rgba(255, 255, 255, 0.06);
  color: #FAFAFA;
}

/* ---------- Combo box ---------- */
QComboBox {
  background-color: transparent;
  color: #FAFAFA;
  border: 1px solid #262626;
  border-radius: 6px;
  padding: 8px 30px 8px 12px;
  font-size: 14px;
  min-height: 22px;
}
QComboBox:hover   { border-color: #3A3A3A; }
QComboBox:focus   { border-color: #CCCCCC; }
QComboBox:disabled {
  color: #6B6B6B;
  background-color: transparent;
  border-color: #1A1A1A;
}
QComboBox::drop-down {
  subcontrol-origin: padding;
  subcontrol-position: top right;
  width: 28px;
  border: none;
  background: transparent;
}
QComboBox::down-arrow {
  image: none;
  width: 0; height: 0;
  border-left: 4px solid transparent;
  border-right: 4px solid transparent;
  border-top: 5px solid #8C8C8C;
  margin-right: 8px;
}
QComboBox QAbstractItemView {
  background-color: #0A0A0A;
  border: 1px solid #262626;
  border-radius: 6px;
  padding: 4px;
  selection-background-color: rgba(255, 255, 255, 0.08);
  selection-color: #FAFAFA;
  outline: none;
}
QComboBox QAbstractItemView::item {
  padding: 7px 10px;
  border-radius: 4px;
  min-height: 20px;
}
QComboBox QAbstractItemView::item:hover { background-color: rgba(255, 255, 255, 0.06); }

/* ---------- Checkbox ---------- */
QCheckBox { color: #FAFAFA; font-size: 14px; spacing: 10px; padding: 4px 0; }
QCheckBox::indicator {
  width: 16px;
  height: 16px;
  border-radius: 4px;
  border: 1px solid #262626;
  background-color: transparent;
}
QCheckBox::indicator:hover { border-color: #CCCCCC; }
QCheckBox::indicator:checked {
  background-color: #FAFAFA;
  border-color: #FAFAFA;
  image: none;
}
QCheckBox:disabled { color: #6B6B6B; }
QCheckBox::indicator:disabled { background-color: transparent; border-color: #1A1A1A; }

/* ---------- Text inputs / logs ---------- */
QPlainTextEdit, QTextEdit, QLineEdit {
  background-color: transparent;
  color: #FAFAFA;
  border: 1px solid #262626;
  border-radius: 6px;
  padding: 8px 12px;
  selection-background-color: rgba(255, 255, 255, 0.15);
  selection-color: #FAFAFA;
}
QPlainTextEdit#semirprodLog {
  background-color: transparent;
  border-color: #262626;
  color: #8C8C8C;
  font-family: 'Geist Mono', 'JetBrains Mono', 'Consolas', 'SFMono-Regular', monospace;
  font-size: 12px;
}
QLineEdit:focus, QPlainTextEdit:focus { border-color: #CCCCCC; }

/* ---------- Scroll bars (site: 4px, translucent white handle) ---------- */
QScrollBar:vertical {
  background: transparent;
  width: 6px;
  margin: 4px 2px 4px 0;
}
QScrollBar::handle:vertical {
  background: rgba(255, 255, 255, 0.15);
  min-height: 32px;
  border-radius: 3px;
}
QScrollBar::handle:vertical:hover { background: rgba(255, 255, 255, 0.25); }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }

QScrollBar:horizontal {
  background: transparent;
  height: 6px;
  margin: 0 4px 2px 4px;
}
QScrollBar::handle:horizontal {
  background: rgba(255, 255, 255, 0.15);
  min-width: 32px;
  border-radius: 3px;
}
QScrollBar::handle:horizontal:hover { background: rgba(255, 255, 255, 0.25); }
QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }
QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: transparent; }

/* ---------- Tooltip ---------- */
QToolTip {
  background-color: #0A0A0A;
  color: #FAFAFA;
  border: 1px solid #262626;
  border-radius: 6px;
  padding: 6px 10px;
}

/* ---------- Tab widget (legacy fallback; we ship a sidebar nav now) ---------- */
QTabWidget::pane { border: none; background: transparent; }
QTabBar { background: transparent; }
QTabBar::tab {
  background: transparent;
  color: #8C8C8C;
  padding: 8px 14px;
  border: none;
  margin-right: 4px;
  border-radius: 6px 6px 0 0;
}
QTabBar::tab:hover   { color: #FAFAFA; background-color: rgba(255, 255, 255, 0.06); }
QTabBar::tab:selected { color: #FAFAFA; background-color: rgba(255, 255, 255, 0.08); font-weight: 600; }

/* ---------- Menu / menu bar ---------- */
QMenu {
  background-color: #0A0A0A;
  border: 1px solid #262626;
  border-radius: 6px;
  padding: 4px;
}
QMenu::item {
  padding: 6px 22px;
  border-radius: 4px;
}
QMenu::item:selected { background-color: rgba(255, 255, 255, 0.08); color: #FAFAFA; }
QMenuBar { background-color: transparent; }
QMenuBar::item:selected { background-color: rgba(255, 255, 255, 0.06); border-radius: 4px; }

/* ---------- Message boxes (update / Studio paywall) ---------- */
QMessageBox { background-color: #000000; }
QMessageBox QLabel { color: #FAFAFA; }
QMessageBox QPushButton { min-width: 90px; }
)QSS";

}  // namespace

void applyDockTheme(QWidget* root) {
  if (!root) return;
  // Scope the stylesheet to this dock's widget subtree ONLY. QSS set on a
  // widget cascades to its descendants (and to dialogs parented to it), but
  // NOT to sibling widgets like OBS's own UI. We intentionally do NOT call
  // qApp->setStyleSheet() here — that used to restyle the whole OBS window.
  root->setStyleSheet(QString::fromUtf8(kGlobalSheet));
}

// ---------- Factories ----------

namespace {

// Centralised name registry so QSS selectors stay in sync with the actual
// QObject::objectName values. Changing one and forgetting the other would
// leave widgets unstyled silently.
constexpr auto kObjectCard           = "semirprodCard";
constexpr auto kObjectSidebar        = "semirprodSidebar";
constexpr auto kObjectHeader         = "semirprodHeader";
constexpr auto kObjectHeaderDevBuild = "semirprodHeaderDevBuild";
constexpr auto kObjectNavItem        = "semirprodNavItem";
constexpr auto kObjectSectionHeader  = "semirprodSectionHeader";
constexpr auto kObjectFieldLabel     = "semirprodFieldLabel";
constexpr auto kObjectStatusDot      = "semirprodStatusDot";
constexpr auto kObjectHero           = "semirprodHero";
constexpr auto kObjectHeroMuted      = "semirprodHeroMuted";
constexpr auto kObjectBrand          = "semirprodBrand";
constexpr auto kObjectBrandSub       = "semirprodBrandSub";
constexpr auto kObjectVersion        = "semirprodVersion";
constexpr auto kObjectDevBadge       = "semirprodDevBadge";
constexpr auto kObjectBody           = "semirprodBody";
constexpr auto kObjectBodyEmph       = "semirprodBodyEmph";
constexpr auto kObjectMuted          = "semirprodMuted";
constexpr auto kObjectPairingCode    = "semirprodPairingCode";
constexpr auto kObjectPairingHint    = "semirprodPairingHint";
constexpr auto kObjectError          = "semirprodError";
constexpr auto kObjectSuccess        = "semirprodSuccess";
constexpr auto kObjectWarning        = "semirprodWarning";

constexpr auto kPropVariant    = "semirprodVariant";
constexpr auto kPropActive     = "semirprodActive";
constexpr auto kPropState      = "semirprodState";

}  // namespace

QPushButton* makeGhostButton(const QString& text, QWidget* parent) {
  auto* b = new QPushButton(text, parent);
  b->setProperty(kPropVariant, QStringLiteral("ghost"));
  b->setCursor(Qt::PointingHandCursor);
  b->style()->unpolish(b);
  b->style()->polish(b);
  return b;
}

QFrame* makeCard(QWidget* parent, int padding) {
  auto* f = new QFrame(parent);
  f->setObjectName(QString::fromLatin1(kObjectCard));
  // 0..3 strength is caller-overridable via addSoftShadow; we default
  // to no shadow here so nested cards don't accumulate stacked outlines.
  auto* layout = new QVBoxLayout(f);
  layout->setContentsMargins(padding, padding, padding, padding);
  layout->setSpacing(spacing::kMd);
  return f;
}

QHBoxLayout* hLayout(int gap, QWidget* parent) {
  auto* l = new QHBoxLayout(parent);
  l->setContentsMargins(0, 0, 0, 0);
  l->setSpacing(gap);
  return l;
}

QLabel* makeSectionHeader(const QString& text, QWidget* parent) {
  auto* l = new QLabel(text, parent);
  l->setObjectName(QString::fromLatin1(kObjectSectionHeader));
  return l;
}

namespace {

// Nom de l'unique animation portee par une pastille. C'est par lui qu'on la
// retrouve au lieu d'en creer une nouvelle.
constexpr auto kObjectPulseAnim = "semirprodPulseAnim";

// L'animation de battement d'une pastille — UNE SEULE, creee a la demande et
// possedee par la pastille.
//
// Ce qui existait avant faisait tomber OBS (violation d'acces du 15 aout,
// 18:17:49, pile : StreamStatusDock::refresh -> setStatusState -> qt6widgets).
// Trois defauts qui se composaient :
//
//   1. `new QPropertyAnimation(dot, ...)` SANS parent, en boucle infinie. Le
//      drapeau DeleteWhenStopped ne se declenche donc jamais : l'animation
//      n'etait ni detruite, ni possedee par personne, et gardait un pointeur
//      nu sur le QLabel.
//   2. Sortir d'un etat pulse ne l'ARRETAIT pas — la branche « off » se
//      contentait de remettre l'opacite. Chaque nouvelle entree dans un etat
//      pulse en empilait donc une de plus, indefiniment. refresh() tourne deux
//      fois par seconde et la source oscille : sur une session d'une heure, on
//      accumulait des dizaines d'animations infinies ecrivant toutes sur le
//      meme label a chaque trame.
//   3. Elle animait `windowOpacity`, qui ne s'applique qu'aux fenetres de
//      premier niveau. Sur une pastille enfant, ca n'a JAMAIS rien anime. Le
//      mecanisme fuyait sans produire l'effet pour lequel il existait.
//
// D'ou : un effet graphique (qui, lui, marche sur un widget enfant), une seule
// animation, parentee a la pastille, retrouvee au lieu d'etre recreee.
QPropertyAnimation* pulseAnimationFor(QLabel* dot) {
  if (auto* found = dot->findChild<QPropertyAnimation*>(
          QString::fromLatin1(kObjectPulseAnim), Qt::FindDirectChildrenOnly)) {
    return found;
  }

  // setGraphicsEffect prend la propriete de l'effet : il mourra avec la
  // pastille, comme l'animation.
  auto* effect = new QGraphicsOpacityEffect;
  effect->setOpacity(1.0);
  dot->setGraphicsEffect(effect);

  auto* anim = new QPropertyAnimation(effect, QByteArrayLiteral("opacity"), dot);
  anim->setObjectName(QString::fromLatin1(kObjectPulseAnim));
  anim->setDuration(1400);
  anim->setStartValue(0.55);
  anim->setKeyValueAt(0.5, 1.0);
  anim->setEndValue(0.55);
  anim->setLoopCount(-1);
  return anim;
}

}  // namespace

QLabel* makeStatusDot(QWidget* parent, bool pulse) {
  auto* l = new QLabel(parent);
  l->setObjectName(QString::fromLatin1(kObjectStatusDot));
  l->setFixedSize(QSize(10, 10));
  l->setProperty(kPropState, QStringLiteral("unknown"));
  if (pulse) pulseAnimationFor(l)->start();
  return l;
}

void setStatusState(QLabel* dot, const QString& state, bool animated) {
  if (!dot) return;

  const QString curState = dot->property(kPropState).toString();
  const bool stateChanged = (curState != state);
  if (stateChanged) dot->setProperty(kPropState, state);

  // Les etats transitoires battent ; online/offline sont des couleurs fixes.
  const bool wantPulse = animated || state == QStringLiteral("busy")
                       || state == QStringLiteral("warn");

  QPropertyAnimation* anim = pulseAnimationFor(dot);
  if (wantPulse) {
    // Idempotent : relancer une animation deja en cours la ferait repartir du
    // debut a chaque tick, soit un clignotement au lieu d'un battement.
    if (anim->state() != QAbstractAnimation::Running) anim->start();
  } else {
    anim->stop();
    if (auto* effect =
            qobject_cast<QGraphicsOpacityEffect*>(dot->graphicsEffect())) {
      effect->setOpacity(1.0);
    }
  }

  // Repolir SEULEMENT quand l'etat a change. C'etait fait a chaque appel, donc
  // six recalculs de feuille de style par seconde pendant toute la session,
  // pour un selecteur qui n'avait pas bouge.
  if (stateChanged) {
    dot->style()->unpolish(dot);
    dot->style()->polish(dot);
  }
}

}  // namespace semirprod::style
