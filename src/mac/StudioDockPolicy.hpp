#pragma once

// ============================================================================
// Cycle de vie du dock Studio : ce qui change sur macOS, et RIEN ailleurs.
//
// Ce fichier existe pour une raison precise : le comportement du panneau CEF
// d'OBS n'est pas le meme sur les trois plateformes, et Windows FONCTIONNE. Il
// ne doit donc pas payer les contournements dont macOS a besoin. Tout ce qui
// suit se resume a des constantes de compilation, evaluees a la compilation :
// dans un binaire Windows, le code macOS n'existe pas, et reciproquement.
//
// ----------------------------------------------------------------------------
// CE QUI DIFFERE, ET POURQUOI (mesure sur macOS 26.5, OBS 32.2.2, CEF du
// paquet officiel — six rapports .ips concordants)
//
// 1. DETRUIRE LE NAVIGATEUR A LA FERMETURE DU DOCK
//    Windows : le fait, avec un delai de 1,5 s, et s'en porte bien.
//    macOS   : plantage systematique d'OBS ~3 s apres la fermeture.
//
//      Thread 0 CrBrowserMain  doesNotRecognizeSelector: -> objc_exception_throw
//        CoreFoundation ___forwarding___
//        obs-browser
//        QtCore QObject::event(QEvent*)
//
//    Cause, dans obs-browser (panel/browser-panel.cpp) :
//
//      void QCefWidgetInternal::showEvent(QShowEvent *) {
//          if (!cefBrowser) {
//              connect(&timer, &QTimer::timeout, this, &Init);
//              timer.start(500);          // relance Init() toutes les 500 ms
//              Init();
//          }
//      }
//
//    Fermer le navigateur met `cefBrowser` a nul ; tout evenement d'affichage
//    qui suit relance donc la creation d'un navigateur, et Init() y passe
//    `winId()` — sur macOS une NSView deja morte — a CreateBrowserSync. Le
//    timer appartient a obs-browser et n'est pas expose : la course est
//    imperdable depuis le plugin. On ne detruit donc plus, on masque, comme
//    OBS le fait pour ses propres docks navigateur.
//
// 2. RECHARGER LA PAGE A LA REOUVERTURE
//    Interdit sur les deux plateformes, en realite, mais seul macOS l'a fait
//    tomber ici : setURL() d'obs-browser fait
//    `cefBrowser->GetMainFrame()->LoadURL(url)` sans verifier GetMainFrame(),
//    qui est nul apres un masquage (obs-browser#375).
//
// 3. REFERMER LE STUDIO AU DEMARRAGE D'OBS
//    OBS restaure son `DockState` APRES le chargement des modules
//    (frontend/widgets/OBSBasic.cpp : restoreState ligne 1254, puis
//    FINISHED_LOADING ligne 1397), donc apres le hide() du plugin. Le Studio
//    se rouvrait seul, parfois ancre a droite. Corrige sur macOS uniquement :
//    sur Windows le comportement actuel convient et n'a pas a bouger.
//
// 4. LE JETON DE SESSION DANS LE FRAGMENT D'URL
//    Le script de demarrage CEF ecrit le jeton dans localStorage ; sur macOS
//    cette ecriture n'aboutit pas, et le try/catch du script rend l'echec
//    muet — la page affichait alors l'ecran « colle ton code d'acces ». Le
//    fragment est lu en priorite par ws.ts et ne depend d'aucun stockage.
// ============================================================================

namespace semirprod {


// UN SEUL COMPORTEMENT, SUR LES TROIS PLATEFORMES.
//
// Ces constantes ont d'abord eu deux jeux de valeurs, un pour macOS et un pour
// le reste, le temps de porter le plugin. Elles n'en ont plus qu'un : un plugin
// OBS tape sur les memes API partout, et maintenir deux comportements revenait
// a devoir reporter a la main chaque correction Windows. Desormais une
// modification faite pour Windows se recompile telle quelle sur macOS.
//
// Ce qui reste specifique a macOS est d'une autre nature : ce ne sont pas des
// choix de comportement mais des prerequis techniques sans equivalent ailleurs
// — le backend TLS embarque (src/mac/QtTlsBackend), le bundle `.plugin`, le
// RPATH, Security.framework. Ceux-la n'ont pas vocation a disparaitre.
inline constexpr bool kDestroyBrowserOnClose = true;
inline constexpr bool kSafeMacTeardown = false;
inline constexpr int kBrowserDestroyDelayMs = 0;  // inutilise hors macOS
inline constexpr bool kForceStudioClosedAtStartup = false;
inline constexpr bool kTokenInUrlFragment = false;

// LE STUDIO EST UNE FENETRE MODALE, PAS UN DOCK.
//
// C'est le format d'OBS lui-meme pour ce genre de surface : Parametres, et
// surtout `frontend/dialogs/OAuthLogin.cpp`, qui heberge un QCefWidget dans un
// QDialog de premier niveau et s'en sert pour la connexion Twitch/YouTube.
//
// Ce qu'on y gagne n'est pas cosmetique. Un dock se MASQUE, et un QCefWidget
// masque perd sa fenetre native : releve le 18/09 sur un Studio blanc, il ne
// restait sous le dock que trois fenetres Qt imbriquees et plus aucune
// `Chrome_WidgetWin_*`. Toute la mecanique qui entourait ce masquage — signaux
// de visibilite, demontage differe, drapeaux de reentrance, sonde de vue
// perdue — n'existait que pour rattraper ca, et deux plantages de la meme
// journee en sont sortis.
//
// Une fenetre modale, elle, EXISTE ou N'EXISTE PAS. Elle est construite a
// l'ouverture, detruite a la fermeture, jamais masquee. Le navigateur suit la
// meme vie. Il n'y a plus d'etat intermediaire a rattraper.
//
// Le prix, mesure : 42 ms pour reconstruire le Studio (journal du 18/09,
// ouverture 15:33:40.074 -> monte 15:33:40.116). CEF tourne deja depuis le
// lancement d'OBS et la page vient de 127.0.0.1.
inline constexpr bool kStudioInModalWindow = true;

// RETIREE. Le menu est desormais identique sur les trois plateformes (un menu
// « Semirprod » a une entree) : macOS n'affiche ni une action de premier niveau
// sans sous-menu, ni un menu vide, donc le clic unique de Windows n'y est pas
// portable. Voir le commentaire dans plugin.cpp.
//
// Ancien texte, conserve pour la trace :
//
// Il ne contenait qu'une entree, « Semirprod Studio », sous un menu deja nomme
// « Semirprod » : deux clics et deux fois le meme mot pour une action unique.
// Une QAction posee directement sur la barre de menus se declenche comme un
// bouton — c'est ce que Qt fait d'une action de barre de menus sans sous-menu.
//
// Constante DISTINCTE de kStudioInModalWindow, et il faut qu'elle le reste :
// macOS prend la fenetre mais garde le sous-menu, parce que sa barre de menus
// est celle du systeme et n'affiche pas une action sans menu. Les confondre
// ferait disparaitre l'entree Semirprod de la barre macOS.



}  // namespace semirprod
