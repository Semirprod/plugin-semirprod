#include "WebUiWindow.hpp"

#include "mac/StudioDockPolicy.hpp"
#ifdef __APPLE__
#include "mac/CefHostView.hpp"
#endif

// Vendored from obs-studio/plugins/obs-browser/panel/browser-panel.hpp. It
// declares the abstract QCef / QCefWidget interface and inline helpers that
// dlsym obs-browser's exported factory. ABI-coupled to the obs-browser build;
// if OBS ships an incompatible QCef, obs_browser_init_panel() returns null and
// we degrade gracefully (isAvailable() == false). Included ONLY here so its
// Q_OBJECT (QCefWidget) never reaches this target's AUTOMOC.
#include "browser-panel.hpp"

#include <QEventLoop>
#include <QTimer>
#include <QVBoxLayout>

#include <obs-module.h>

// Sonde des fenetres natives du navigateur (refitBrowserView, plus bas). Meme
// precaution que TokenStore.cpp et UpdateChecker.cpp : sans NOMINMAX, windows.h
// definit des macros `min`/`max` qui cassent la STL et Qt inclus au-dessus.
#ifdef Q_OS_WIN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <cwchar>
#endif

#include <string>

namespace semirprod {

namespace {

// Au-dela, on renonce a la fenetre integree et l'appelant ouvre le navigateur
// systeme. Genereux : sur une machine chargee, CEF met plusieurs secondes a
// demarrer ses process.
constexpr int kCefInitTimeoutMs = 15000;

// Attend que CEF soit pret SANS bloquer le fil d'interface.
//
// `wait_for_browser_init()` est un `os_event_wait()` sans delai maximal
// (obs-browser, panel/browser-panel.cpp). L'appeler depuis le fil Qt principal
// — ce que faisait cette fenetre — gele OBS tant que CEF n'a pas fini de
// demarrer : la fenetre ne pompe plus ses messages et Windows la repeint en
// blanc. OBS lui-meme ne fait jamais ca : `OBSBasic::InitBrowserPanelSafeBlock()`
// sort immediatement si `init_browser()` renvoie vrai, sinon delegue l'attente a
// un fil de travail en gardant sa boucle d'evenements vivante.
//
// On applique le meme principe sans creer de fil : `initialized()` est un
// `os_event_try()`, donc non bloquant. On le sonde depuis une boucle
// d'evenements locale, ce qui laisse l'interface d'OBS se peindre pendant
// l'attente, et on abandonne au bout de kCefInitTimeoutMs.
bool waitForBrowserWithoutFreezing(QCef* cef) {
  if (cef->init_browser()) return true;  // deja pret : rien a attendre

  QEventLoop loop;
  QTimer poll;
  QTimer deadline;

  QObject::connect(&poll, &QTimer::timeout, &loop, [cef, &loop]() {
    if (cef->initialized()) loop.quit();
  });
  QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);

  deadline.setSingleShot(true);
  poll.start(25);
  deadline.start(kCefInitTimeoutMs);
  loop.exec();

  return cef->initialized();
}

}  // namespace

WebUiWindow::WebUiWindow(QWidget* parent) : QWidget(parent) {
  setWindowTitle(QStringLiteral("Semirprod"));
  // Taille SUGGEREE, jamais imposee.
  //
  // Ce widget etait une fenetre de premier niveau ; il vit maintenant DANS un
  // QDockWidget, et un `setMinimumSize(960, 540)` y devient une contrainte sur
  // le dock lui-meme. Quand la taille du dock restauree par OBS ne la
  // satisfaisait pas, Qt redimensionnait la fenetre flottante et la recreait —
  // donc un masquage suivi d'un reaffichage, juste apres le montage. Le cycle
  // de vie branche sur visibilityChanged repondait en detruisant puis
  // reconstruisant le navigateur, en boucle, jusqu'a ce que libcef tombe
  // (STATUS_BREAKPOINT sur CrBrowserMain, 15 aout, en boucle au demarrage).
  //
  // Une taille minimale n'a de toute facon aucun sens ici : c'est l'operateur
  // qui dimensionne son dock. On garde seulement un plancher symbolique, qui
  // ne contraindra jamais rien.
  resize(1152, 648);
  setMinimumSize(160, 90);

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);

  QCef* cef = obs_browser_init_panel();
  if (!cef) {
    blog(LOG_WARNING,
         "[semirprod-webui] OBS embedded browser (obs-browser/CEF) unavailable "
         "— falling back to the native dock");
    return;
  }
  if (!waitForBrowserWithoutFreezing(cef)) {
    blog(LOG_WARNING,
         "[semirprod-webui] CEF toujours pas initialise apres %d ms — fenetre "
         "integree abandonnee, repli sur le navigateur du systeme",
         kCefInitTimeoutMs);
    return;  // isAvailable() reste faux : l'appelant prend le chemin de repli
  }

  // Empty URL now; load() sets the startup script (token) and the real URL so
  // the script is registered before the first navigation that matters.
  cefWidget_ = cef->create_widget(this, std::string(), nullptr);
  if (cefWidget_) {
    layout->addWidget(cefWidget_);
  } else {
    blog(LOG_WARNING, "[semirprod-webui] CEF create_widget returned null");
  }
}

void WebUiWindow::load(const QString& url, const QString& wsToken) {
  if (!cefWidget_) return;

  // DEUX chemins pour le meme jeton, et ce n'est pas de la ceinture-bretelles
  // gratuite : sur macOS le premier ne marche pas.
  //
  // 1. localStorage, via le script de demarrage. C'est le chemin historique ;
  //    il a l'avantage de survivre aux rechargements internes de la page.
  // 2. Le fragment d'URL (#jeton). ws.ts resolveToken() le lit EN PREMIER,
  //    avant localStorage — l'interface a toujours su le faire (« URL hash >
  //    localStorage »), le plugin ne s'en servait simplement pas.
  //
  // Pourquoi le 2 est devenu obligatoire : dans le panneau CEF de macOS,
  // l'ecriture localStorage du script de demarrage n'aboutit pas, et comme le
  // script l'entoure d'un try/catch, l'echec est parfaitement muet. Resultat
  // observe : le Studio se monte normalement dans le dock, puis affiche
  // l'ecran « colle ton code d'acces » — celui du repli navigateur — sans que
  // rien, ni dans le journal d'OBS ni a l'ecran, ne relie ca au stockage.
  //
  // Le fragment ne quitte jamais le navigateur (il n'est pas envoye au serveur
  // HTTP), l'URL est 127.0.0.1, et le jeton tourne a chaque demarrage d'OBS.
  // Le dock CEF n'a pas de barre d'adresse, donc rien ne s'affiche a l'ecran —
  // c'est la raison pour laquelle le repli navigateur systeme, lui, continue
  // de passer par le presse-papiers (voir openWebUiInSystemBrowser).
  QString target = url;
  if (!wsToken.isEmpty()) {
    // Le fragment d'URL : macOS uniquement (mac/StudioDockPolicy.hpp).
    // Windows ecrit le jeton dans localStorage par le script de demarrage et
    // ca fonctionne ; on ne touche pas a un chemin qui marche.
    // The token is base64url (A-Za-z0-9-_), so it is safe inside a single-quoted
    // JS string with no escaping. Set it in localStorage: ws.ts resolveToken()
    // reads it and useWsBridge auto-connects on mount.
    const std::string script =
        "try{localStorage.setItem('semirprod_token','" + wsToken.toStdString() +
        "');}catch(e){}";
    cefWidget_->setStartupScript(script);

    // Meme alphabet base64url : rien a encoder pour en faire un fragment.
    if constexpr (kTokenInUrlFragment) {
      target += QStringLiteral("#") + wsToken;
    }
  }
  cefWidget_->setURL(target.toStdString());

#ifdef __APPLE__
  // Trace de reference : l'adresse de la vue hote AU MONTAGE. C'est a elle
  // qu'on comparera celle du reaffichage.
  logCefHostView(cefWidget_->winId(), "montage");
#endif
}

// Fermeture EXPLICITE du navigateur, avant toute destruction.
//
// POURQUOI CE N'EST PAS REDONDANT AVEC LE DESTRUCTEUR
//
// ~QCefWidgetInternal() appelle deja closeBrowser() (obs-browser,
// panel/browser-panel.cpp:184). On pourrait donc croire qu'un simple
// deleteLater() sur cette fenetre suffit. Il ne suffit pas, et le mode d'echec
// est un plantage d'OBS, pas une fuite.
//
// closeBrowser() fait tourner une BOUCLE D'EVENEMENTS IMBRIQUEE : il detache
// la fenetre native, demande CefBrowserHost::CloseBrowser(), puis
// `browserCloseLoop.exec()` jusqu'a readyToClose ou 1 seconde. Declenche depuis
// un destructeur — c'est-a-dire pendant le traitement de l'evenement
// DeferredDelete de Qt — cela signifie pomper des evenements AU MILIEU de la
// destruction de l'arbre de widgets. Les evenements en attente sont alors
// delivres a des objets a demi detruits.
//
// Constate ici, quatre fois de suite, a chaque fermeture du Studio :
//
//   Thread 0 CrBrowserMain  EXC_BAD_ACCESS (SIGSEGV) at 0x0
//     Chromium Embedded Framework
//     CoreFoundation  ___forwarding___        <- message ObjC a un objet mort
//     obs-browser
//     QtCore          QObject::event(QEvent*)
//
// `___forwarding___` est la signature d'un message Objective-C envoye a un
// objet desalloue : la vue native du navigateur, que closeBrowser() venait de
// detacher.
//
// En appelant closeBrowser() ICI, sur une fenetre encore vivante et encore
// attachee a sa hierarchie, la boucle imbriquee tourne dans un etat COHERENT.
// La destruction qui suit n'a plus rien a fermer (cefBrowser est nul), donc
// plus de boucle imbriquee pendant le destructeur, et plus de plantage.
bool WebUiWindow::refitBrowserView() {
#ifdef __APPLE__
  if (!cefWidget_) return false;

  // winId() force la creation d'une NSView si le widget n'en a pas encore ;
  // c'est voulu, c'est aussi ce que fait obs-browser dans Init().
  const WId host = cefWidget_->winId();
  logCefHostView(host, "reaffichage");
  if (refitCefHostView(host, cefWidget_->size())) return true;

  // Aucune sous-vue : le navigateur n'est plus attache a ce widget. C'est le
  // cas normal apres un masquage sur macOS, pas une anomalie — obs-browser
  // n'y rattache jamais rien apres la creation. L'appelant recree.
  logCefHostView(host, "vue perdue");
  return false;
#elif defined(Q_OS_WIN)
  if (!cefWidget_) return false;

  // Windows : MEME QUESTION QUE SUR macOS, posee aux fenetres natives.
  //
  // Ce bras retournait `true` en dur — « vue intacte », toujours, sans rien
  // regarder. L'appelant en concluait qu'il n'y avait rien a faire, et remettait
  // a l'ecran un navigateur dont la vue n'existait plus : DOCK BLANC. Seule une
  // fermeture de plus de 1,5 s y echappait, parce qu'elle detruit et reconstruit
  // le Studio par un autre chemin.
  //
  // Constate le 18/09, OBS en cours, Studio blanc a l'ecran, arbre des fenetres
  // de la fenetre flottante « Semirprod » :
  //
  //   Qt6111QWindowIcon  927x637      <- l'hote du QCefWidget
  //     Qt6111QWindowIcon  927x637    <- le conteneur
  //       Qt6111QWindowIcon  927x637  <- la QWindow
  //   (et RIEN d'autre)
  //
  // Aucune `Chrome_WidgetWin_*` dans ce sous-arbre, alors que les deux autres
  // docks navigateur d'OBS ont la leur. Le navigateur n'est plus attache a ce
  // widget : exactement le « vue perdue » de macOS, et la meme reponse — on
  // repond non, l'appelant recree.
  //
  // Pourquoi obs-browser ne la remet pas : `QCefWidgetInternal::showEvent()`
  // (panel/browser-panel.cpp) ne reconstruit QUE si `cefBrowser` est nul. Ici il
  // ne l'est pas — le navigateur est vivant, c'est sa fenetre qui a disparu du
  // sous-arbre —, donc le reaffichage ne declenche rien du tout.
  //
  // On SONDE, sans rien replacer : le redimensionnement, lui, est deja gere par
  // obs-browser (resizeEvent -> Resize()), et bouger une fenetre qui appartient
  // au fil de CEF depuis le fil Qt n'a rien a faire ici.
  const HWND host = reinterpret_cast<HWND>(cefWidget_->winId());
  if (!host) return false;

  bool trouve = false;
  ::EnumChildWindows(
      host,
      [](HWND enfant, LPARAM sortie) -> BOOL {
        wchar_t classe[64] = {0};
        if (::GetClassNameW(enfant, classe, 64) <= 0) return TRUE;
        // Le nom de la classe est celui que Chromium donne a la fenetre de son
        // navigateur ; obs-browser n'en expose aucun autre signe depuis un
        // plugin. Prefixe et non egalite : le suffixe numerique varie.
        if (::wcsncmp(classe, L"Chrome_WidgetWin", 16) != 0) return TRUE;
        *reinterpret_cast<bool*>(sortie) = true;
        return FALSE;  // trouve, on arrete l'enumeration
      },
      reinterpret_cast<LPARAM>(&trouve));

  if (!trouve) {
    blog(LOG_INFO,
         "[semirprod-webui] aucune fenetre de navigateur sous le Studio — "
         "vue perdue au masquage");
  }
  return trouve;
#else
  return true;
#endif
}

void WebUiWindow::closeBrowser() {
  if (!cefWidget_) return;
  cefWidget_->closeBrowser();
}

}  // namespace semirprod
