#pragma once

#include <QString>
#include <QWidget>

// QCefWidget lives in the global namespace (obs-browser's browser-panel.hpp).
// Forward-declared here so this header never pulls that Q_OBJECT type into
// AUTOMOC — the include stays confined to WebUiWindow.cpp.
class QCefWidget;

namespace semirprod {

// A top-level window that hosts OBS's embedded browser (CEF, provided by the
// obs-browser plugin) and shows the Semirprod web UI served locally by
// WebUiServer on http://127.0.0.1:47615. This is what the "Open Semirprod"
// menu opens — the React UI, identical to the site — instead of the legacy
// native Qt dock. The plugin's SemirprodDock stays alive as the backend (WS
// server + logic); this window is just the view.
class WebUiWindow : public QWidget {
  Q_OBJECT
 public:
  explicit WebUiWindow(QWidget* parent = nullptr);

  // False when OBS's embedded browser is unavailable (obs-browser not loaded,
  // or Wayland). The caller should then fall back to the native dock.
  bool isAvailable() const { return cefWidget_ != nullptr; }

  // Point the embedded browser at `url`, injecting `wsToken` into the page's
  // localStorage before load so the UI auto-connects to the WS without a manual
  // paste. The token never touches the URL, the OBS log, or any on-screen text.
  void load(const QString& url, const QString& wsToken);

  // Ferme le navigateur CEF, a appeler AVANT de detruire cette fenetre.
  //
  // Ce n'est pas une commodite : sans cet appel, OBS PLANTE a chaque fermeture
  // du Studio. Detail dans WebUiWindow.cpp.
  //
  // Idempotent : obs-browser sort immediatement si le navigateur est deja
  // ferme, donc un second appel (par le destructeur de QCefWidget) ne coute
  // rien.
  void closeBrowser();

  // macOS : replace la vue native du navigateur dans le widget, a la taille
  // courante. No-op ailleurs.
  //
  // A appeler a chaque REAFFICHAGE. obs-browser attache cette vue une seule
  // fois, a la creation, et son resizeEvent est vide sur macOS : apres un
  // masquage, la vue peut rester a l'ancienne taille ou hors champ, ce qui se
  // voit comme un rectangle gris a la place de l'interface.
  // Rend FAUX si la vue native du navigateur n'est plus attachee au widget :
  // l'appelant doit alors recreer le Studio, aucun replacement ne peut sauver
  // un navigateur dont la vue a disparu.
  bool refitBrowserView();

 private:
  QCefWidget* cefWidget_ = nullptr;
};

}  // namespace semirprod
