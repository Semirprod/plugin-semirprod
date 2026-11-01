#include "StudioWindow.hpp"

#include "WebUiWindow.hpp"

#include "../Edition.hpp"
#include "../Settings.hpp"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPoint>
#include <QRect>
#include <QVBoxLayout>

#include <obs-module.h>

namespace semirprod {

namespace {

// Geometrie de la fenetre, dans un JSON a cote des autres fichiers du plugin.
//
// C'est OBS qui gardait position et taille du Studio, via le `DockState` de sa
// fenetre principale. Une fenetre de premier niveau n'y figure pas : si on ne
// l'enregistre pas nous-memes, le Studio rouvre au centre a chaque fois, et
// l'operateur qui l'avait mis sur son deuxieme ecran l'y remet a chaque
// ouverture.
QString geometryFilePath() {
  const QFileInfo info(Settings::configFilePath());
  return info.absoluteDir().filePath(
      QStringLiteral("semirprod-studio-window.json"));
}

}  // namespace

StudioWindow::StudioWindow(QWidget* parent) : QDialog(parent) {
  setWindowTitle(QStringLiteral("Semirprod"));
  setObjectName(QStringLiteral("semirprod_studio_window"));

  // Le point d'interrogation de la barre de titre ne mene nulle part : meme
  // geste que OAuthLogin, qui le retire aussi.
  setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
  setSizeGripEnabled(true);

  // Plancher symbolique, comme dans WebUiWindow : c'est l'operateur qui
  // dimensionne sa fenetre, pas nous.
  setMinimumSize(640, 360);

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);


  // La fenetre ne touche JAMAIS au QCefWidget directement : `browser-panel.hpp`
  // n'est inclus que dans WebUiWindow.cpp, et son Q_OBJECT ne doit atteindre
  // l'AUTOMOC d'aucune autre cible. On passe donc par l'API de WebUiWindow.
  webUi_ = new WebUiWindow(this);
  layout->addWidget(webUi_);

  // Cloud : geometrie imposee, centree sur OBS (pas de barre de titre pour la
  // deplacer, voir cloud/Edition.cpp). Public : celle que l'operateur a
  // laissee.
  if (!edition::imposeStudioWindowGeometry(this, parent)) restoreSavedGeometry();
}

bool StudioWindow::isAvailable() const {
  return webUi_ != nullptr && webUi_->isAvailable();
}

void StudioWindow::load(const QString& url, const QString& wsToken) {
  if (webUi_) webUi_->load(url, wsToken);
}

void StudioWindow::done(int result) {
  if (!closed_) {
    closed_ = true;
    saveCurrentGeometry();

    if (webUi_) {
      // L'ORDRE, et il n'est pas negociable — c'est la lecon du 18/09.
      //
      //   1. closeBrowser() sur une fenetre ENCORE VIVANTE et encore dans sa
      //      hierarchie. obs-browser y fait tourner une boucle d'evenements
      //      imbriquee (browser-panel.cpp) : elle doit tourner sur un objet
      //      coherent, jamais au milieu d'une destruction.
      //   2. hide() + setParent(nullptr) : le widget quitte l'arbre de cette
      //      fenetre. Qt ne retire pas un enfant de la liste de son parent
      //      avant ~QObject, et tout ce qui parcourrait cet arbre pendant la
      //      destruction tomberait sur un objet a demi detruit.
      //   3. deleteLater() plutot que `delete` : done() peut etre appele depuis
      //      un gestionnaire d'evenement du navigateur lui-meme (la page qui se
      //      ferme, une touche Echap partie de CEF). Detruire sur-le-champ
      //      reviendrait alors a liberer la pile sous nos pieds.
      //
      // OBS fait un simple `delete cefWidget` dans OAuthLogin::reject(). On est
      // plus prudent d'un cran, pour la seule raison que notre fermeture peut
      // venir de la page.
      webUi_->closeBrowser();
      webUi_->hide();
#ifdef _WIN32
      webUi_->setParent(nullptr);
#endif
      // PAS de setParent(nullptr) hors Windows (crash du 04/10, instance
      // cloud) : sous X11 le navigateur est embarque par un QWindowContainer.
      // Detache, il devient une fenetre de premier niveau dont CEF a deja
      // ferme la fenetre native ; sa destruction differee la detruit une
      // seconde fois (QWidgetPrivate::deleteTLSysExtra). Il reste donc enfant
      // de cette fenetre et part avec elle, comme le fait OAuthLogin d'OBS.
      webUi_->deleteLater();
      webUi_ = nullptr;
    }
  }

  QDialog::done(result);
}

void StudioWindow::restoreSavedGeometry() {
  QFile file(geometryFilePath());
  if (!file.exists() || !file.open(QIODevice::ReadOnly)) {
    // Premiere ouverture : la taille par defaut du Studio, centree sur OBS par
    // Qt puisque la fenetre a un parent.
    resize(1152, 648);
    return;
  }

  const QJsonObject obj =
      QJsonDocument::fromJson(file.readAll()).object();
  const QByteArray blob = QByteArray::fromBase64(
      obj.value(QStringLiteral("geometry")).toString().toLatin1());

  // `restoreGeometry` rend faux sur un contenu illisible ou une geometrie qui
  // ne tombe sur aucun ecran actuel — un Studio enregistre sur un deuxieme
  // ecran debranche depuis. On retombe alors sur la taille par defaut plutot
  // que d'ouvrir une fenetre invisible.
  if (blob.isEmpty() || !restoreGeometry(blob)) resize(1152, 648);
}

void StudioWindow::saveCurrentGeometry() {
  QJsonObject obj;
  obj[QStringLiteral("geometry")] =
      QString::fromLatin1(saveGeometry().toBase64());

  QFile file(geometryFilePath());
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    blog(LOG_WARNING,
         "[semirprod-webui] geometrie du Studio non enregistree (%s)",
         file.errorString().toUtf8().constData());
    return;
  }
  file.write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

}  // namespace semirprod
