// SemirprodTheme — single source of truth for every visual decision in the
// plugin's UI. Pulled into a dedicated module so the dock, the status dock
// and the web mirror all read from the same color/space/type tokens, which
// keeps the product feeling like one app rather than three styled widgets.
//
// Public surface (kept deliberately small so adding a new screen doesn't
// fork the design system):
//   * Style::token constants — colors, spacing, radii, durations.
//   * Style::applyDockTheme(QWidget*) — installs the unified QSS scoped to one
//       dock (never qApp-wide, so OBS's own theme is untouched).
//   * Style::makeGhostButton — action button factory qui porte deja les bons
//       espacements et les transitions de survol.
//   * Style::makeCard / makeSectionHeader / makeStatusDot — briques de base.
//
// La surface a retreci avec le retrait de l'arbre de widgets de SemirprodDock :
// les fabriques que lui seul appelait (boutons primaire/secondaire/icone,
// pastilles, vLayout, ombre douce, degrade de marque) ont ete retirees plutot
// que gardees « au cas ou ». Les deux docks vivants — Diffusion et Journal —
// n'utilisent plus que ce qui reste. Git garde les anciennes si besoin.
//
// Naming convention: `token key` (kebab) in CSS, `kKey` (camel) in C++.
// The web mirror (resources/ui/index.html) duplicates the same tokens
// under the `--*` custom properties — keep both sides in sync when
// introducing a new color.

#pragma once

#include <QString>

// Qt classes forward-declared at GLOBAL scope. These MUST live outside the
// namespace below — declaring e.g. `class QLabel;` inside `semirprod::style`
// creates a phantom `semirprod::style::QLabel` type that shadows Qt's real
// ::QLabel and breaks every caller that mixes the two.
class QWidget;
class QPushButton;
class QLabel;
class QFrame;
class QHBoxLayout;

namespace semirprod::style {

// ---------- Colors ----------
// Mirrors the Semirprod web design system 1:1 (shadcn "New York", base color
// Neutral, dark theme). Every value below is the hex of the corresponding CSS
// custom property in Semirprod-site apps/*/app/globals.css `.dark` — keep the
// two in sync. The palette is a pure neutral greyscale: there is no brand hue,
// emphasis comes from an INVERTED primary (near-white surface, near-black text)
// exactly like the site's default Button.
namespace colors {
inline constexpr auto kBg          = "#000000";   // --background
inline constexpr auto kSurface     = "#141414";   // --card / --popover
inline constexpr auto kSurfaceAlt  = "#0A0A0A";   // faint raise off pure black
inline constexpr auto kElevated    = "#212121";   // --secondary / --muted / --accent
inline constexpr auto kBorder      = "#262626";   // --border / --input
inline constexpr auto kBorderHi    = "#3A3A3A";   // hovered border

inline constexpr auto kTextPrimary = "#FAFAFA";   // --foreground
inline constexpr auto kTextMuted   = "#8C8C8C";   // --muted-foreground
inline constexpr auto kTextFaint   = "#6B6B6B";   // de-emphasised labels

// "Accent" on the site is not a hue — the primary action is an inverted
// surface, and focus is a neutral ring. Keeping the names so call sites
// don't churn.
inline constexpr auto kAccent      = "#FAFAFA";   // --primary
inline constexpr auto kAccentOn    = "#171717";   // --primary-foreground
inline constexpr auto kRing        = "#CCCCCC";   // --ring (focus outline)

// Semantic status. These stay chromatic on purpose — a red/green dot has to
// read as red/green. Sourced from the site's chart tokens so they belong to
// the same family as the dashboard graphs.
inline constexpr auto kSuccess     = "#2EB88A";   // --chart-2
inline constexpr auto kSuccessBg   = "rgba(46, 184, 138, 0.12)";
inline constexpr auto kWarning     = "#E88C30";   // --chart-3
inline constexpr auto kWarningBg   = "rgba(232, 140, 48, 0.12)";
inline constexpr auto kDanger      = "#E23670";   // --chart-5
inline constexpr auto kDangerBg    = "rgba(226, 54, 112, 0.12)";
inline constexpr auto kInfo        = "#2662D9";   // --chart-1
inline constexpr auto kInfoBg      = "rgba(38, 98, 217, 0.12)";
}  // namespace colors

// ---------- Spacing & geometry ----------
namespace spacing {
inline constexpr int kXs    = 4;
inline constexpr int kSm    = 8;
inline constexpr int kMd    = 12;
inline constexpr int kLg    = 16;
inline constexpr int kXl    = 24;
inline constexpr int kXxl   = 32;
}  // namespace spacing

// Matches the site's --radius: 0.5rem, i.e. Tailwind rounded-sm/md/lg =
// 4 / 6 / 8px. Cards are `rounded-lg`, buttons and inputs are `rounded-md`.
namespace radius {
inline constexpr int kSm    = 4;    // chips, small inputs
inline constexpr int kMd    = 6;    // buttons, inputs, combo
inline constexpr int kLg    = 8;    // cards
inline constexpr int kPill  = 999;  // rounded pills (status)
}  // namespace radius

namespace duration {
inline constexpr int kFast  = 120;  // hover/state transitions
inline constexpr int kMed   = 220;  // panel changes
inline constexpr int kSlow  = 360;  // hero pulses
}  // namespace duration

// Figtree (sans) + Geist Mono, same as the site. Neither ships with the
// plugin, so the fallback chain has to hold up on a machine that has never
// visited semirprod.fr — Segoe UI first on Windows, then the usual suspects.
namespace typography {
inline constexpr auto kFamily   = "'Figtree', 'Segoe UI', 'Inter', "
                                   "'Helvetica Neue', "
                                   "Roboto, Arial, sans-serif";
inline constexpr auto kFamilyMo = "'Geist Mono', 'JetBrains Mono', 'Consolas', "
                                  "'SFMono-Regular', monospace";
inline constexpr int kTitle   = 18;  // top header title
inline constexpr int kH1      = 20;  // hero stats
inline constexpr int kH2      = 14;  // card titles
inline constexpr int kBody    = 13;
inline constexpr int kSmall   = 12;
inline constexpr int kCaption = 11;
}  // namespace typography

// ---------- Public API ----------

// Installs the unified QSS on `root` (a plugin dock) so it and every
// descendant — including dialogs/QMessageBoxes parented to it — pick up the
// Semirprod styling. IMPORTANT: this is scoped to `root` ONLY. It deliberately
// does NOT touch qApp's application-wide stylesheet: doing that restyled the
// entire OBS UI (menus, tabs, other plugins), which is not wanted. Call it once
// per dock. Idempotent.
void applyDockTheme(QWidget* root);

// ---------- Widget factories ----------
// All factories return heap-allocated, parent-less widgets. Ownership
// stays with the caller (typically the dock).

QPushButton* makeGhostButton(const QString& text, QWidget* parent = nullptr);

// Rounded card surface. `padding` lets callers tighten the chrome when
// nesting cards inside cards.
QFrame*     makeCard(QWidget* parent = nullptr, int padding = spacing::kLg);

// A horizontal stack helper. `gap` defaults to 12 — keeps spacing
// predictable without every caller having to re-set it.
QHBoxLayout* hLayout(int gap = spacing::kMd, QWidget* parent = nullptr);

// Section header — small uppercase caption + optional right-side widget.
QLabel* makeSectionHeader(const QString& text, QWidget* parent = nullptr);

// Status dot — a tiny circular indicator with a subtle glow. Color is
// updated via setStatusState(). `pulse=true` adds a soft breathing
// animation appropriate for "waiting / connecting" states.
QLabel* makeStatusDot(QWidget* parent = nullptr, bool pulse = false);

// Updates an existing status dot color & pulse animation state.
// `state` ∈ {"online","busy","offline","unknown","warn"}.
void setStatusState(QLabel* dot, const QString& state, bool animated = false);

}  // namespace semirprod::style
