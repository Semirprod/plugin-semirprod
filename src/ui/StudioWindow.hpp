#pragma once

#include <QDialog>
#include <QString>

namespace semirprod {

class WebUiWindow;

// Le Studio, en FENETRE MODALE.
//
// C'est le format d'OBS lui-meme pour ce genre de surface : la fenetre
// Parametres, et surtout `frontend/dialogs/OAuthLogin.cpp`, qui heberge deja un
// QCefWidget dans un QDialog de premier niveau et sert a la connexion
// Twitch/YouTube. Le precedent compte : il tourne chez tout le monde, y compris
// sur le Qt 6.8.3 d'OBS 32.0, notre plancher.
//
// POURQUOI PAS UN DOCK — un dock se MASQUE, et un QCefWidget masque perd sa
// fenetre native. Releve le 18/09 sur un Studio blanc : sous le dock il ne
// restait que trois fenetres Qt imbriquees et plus aucune `Chrome_WidgetWin_*`.
// Toute la mecanique qui entourait ce masquage (signaux de visibilite,
// demontage differe, drapeaux de reentrance, sonde de vue perdue) n'existait que
// pour rattraper ca, et deux plantages de la meme journee en sont sortis.
//
// Une fenetre modale EXISTE ou N'EXISTE PAS. Construite a l'ouverture, detruite
// a la fermeture, jamais masquee — le navigateur suit la meme vie. Il n'y a plus
// d'etat intermediaire a rattraper.
//
// macOS reste sur le dock : voir mac/StudioDockPolicy.hpp, kStudioInModalWindow.
class StudioWindow : public QDialog {
  Q_OBJECT
 public:
  explicit StudioWindow(QWidget* parent = nullptr);

  // Faux quand le navigateur integre d'OBS (obs-browser/CEF) est indisponible.
  // L'appelant bascule alors sur le navigateur du systeme.
  bool isAvailable() const;

  // Pointe le navigateur sur l'interface servie en local. Meme contrat que
  // WebUiWindow::load : le jeton ne passe ni par l'ecran ni par le journal.
  void load(const QString& url, const QString& wsToken);

  // Le seul entonnoir de fermeture : la croix, Echap, accept(), reject() et
  // close() y passent tous. C'est la qu'on ferme le navigateur, pas ailleurs.
  //
  // Publique comme dans QDialog, et pas protegee : l'arret d'OBS appelle
  // done() de l'exterieur pour faire sortir la fenetre de sa boucle modale.
  void done(int result) override;

 private:
  void restoreSavedGeometry();
  void saveCurrentGeometry();

  WebUiWindow* webUi_ = nullptr;
  // done() peut etre appele deux fois (Echap puis destruction, par exemple) :
  // la fermeture du navigateur, elle, ne doit avoir lieu qu'une fois.
  bool closed_ = false;
};

}  // namespace semirprod
