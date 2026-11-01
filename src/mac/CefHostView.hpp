#pragma once

// Inspection et remise en place de la vue native de CEF — macOS uniquement.
//
// Pourquoi ce fichier existe : sur macOS, obs-browser attache la vue du
// navigateur UNE SEULE FOIS, a la creation (Init() -> SetAsChild(winId())), et
// ne la retouche plus jamais. Son resizeEvent est litteralement vide sur cette
// plateforme :
//
//   void QCefWidgetInternal::resizeEvent(QResizeEvent *event) {
//       QWidget::resizeEvent(event);
//   #ifndef __APPLE__          // tout le corps est exclu sur mac
//       Resize();
//   #endif
//   }
//
// Consequence : si Qt recree la NSView du widget (ce qu'il fait en masquant
// puis reaffichant une fenetre flottante), ou si la taille change entre deux
// affichages, la vue du navigateur reste a l'ancienne place, a l'ancienne
// taille, voire orpheline. A l'ecran : un rectangle gris.
//
// Ces fonctions travaillent sur la hierarchie AppKit du widget, sans toucher a
// CEF : on ne cree ni ne detruit aucun navigateur, on replace une vue.

#include <QSize>
#include <qwindowdefs.h>  // WId — declare la, pas dans QtGlobal

namespace semirprod {

// Ecrit dans le journal d'OBS l'etat de la vue hote et de ses sous-vues :
// nombre, classes, cadres, masquage. C'est la mesure qui dit si un ecran gris
// vient d'une vue absente (rien a replacer) ou mal dimensionnee (replacable).
void logCefHostView(WId hostView, const char* context);

// Redonne a chaque sous-vue le cadre complet du widget hote et un
// autoresizing mask, puis demande un reaffichage. Rend false si la vue hote
// n'a aucune sous-vue — c'est-a-dire si le navigateur n'est plus attache, cas
// qu'aucun redimensionnement ne peut rattraper.
bool refitCefHostView(WId hostView, QSize size);

}  // namespace semirprod
