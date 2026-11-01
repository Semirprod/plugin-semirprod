#include "mac/QtTlsBackend.hpp"

#ifdef __APPLE__

#include <dlfcn.h>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSslSocket>
#include <QString>
#include <QStringList>

#include <obs-module.h>

namespace semirprod {
namespace {

// Chemin du dossier `PlugIns` de NOTRE bundle, deduit du binaire courant.
//
// dladdr et non obs_module_file() : ce dernier vise `Contents/Resources`, et
// surtout il repond relativement au module tel qu'OBS l'a enregistre. dladdr
// part de l'adresse d'une fonction reellement chargee, donc du binaire qui
// s'execute — c'est vrai aussi bien pour le bundle installe que pour un
// .plugin lance depuis l'arbre de build, ce qui evite d'avoir deux codes
// selon la provenance.
QString bundlePlugInsDir() {
  Dl_info info{};
  if (dladdr(reinterpret_cast<const void*>(&bundlePlugInsDir), &info) == 0 ||
      info.dli_fname == nullptr) {
    return {};
  }
  // .../semirprod-studio.plugin/Contents/MacOS/semirprod-studio
  //   -> .../semirprod-studio.plugin/Contents/PlugIns
  const QFileInfo binary(QString::fromUtf8(info.dli_fname));
  const QDir macOsDir = binary.absoluteDir();          // Contents/MacOS
  QDir contents(macOsDir);
  if (!contents.cdUp()) return {};                     // Contents
  const QString plugIns = contents.absoluteFilePath("PlugIns");
  return QDir(plugIns).exists() ? plugIns : QString{};
}

} // namespace

void ensureQtTlsBackend() {
  // Ne RIEN interroger de QSslSocket avant d'avoir ajoute le chemin.
  //
  // QSslSocket::supportsSsl() declenche l'initialisation du sous-systeme TLS
  // et fige le backend actif. Un appel « juste pour verifier si c'est deja
  // bon » place ici verrouillerait donc l'absence de backend avant meme qu'on
  // ait eu l'occasion de la corriger. addLibraryPath() est de toute facon
  // idempotent (Qt ignore un chemin deja present), donc il n'y a rien a
  // economiser.
  const QString plugIns = bundlePlugInsDir();
  if (plugIns.isEmpty()) {
    blog(LOG_WARNING,
         "[semirprod-tls] dossier PlugIns du bundle introuvable — le backend "
         "TLS embarque ne sera pas charge, toutes les requetes HTTPS vont "
         "echouer en « TLS initialization failed »");
    return;
  }

  // addLibraryPath rafraichit les QFactoryLoader existants (refreshAll), donc
  // l'ordre « ajouter le chemin puis utiliser le reseau » suffit : il n'y a pas
  // besoin de toucher QT_PLUGIN_PATH ni de le faire avant la construction de
  // QCoreApplication (impossible ici : c'est OBS qui la cree, bien avant nous).
  QCoreApplication::addLibraryPath(plugIns);

  const bool ok = QSslSocket::supportsSsl();
  if (ok) {
    blog(LOG_INFO, "[semirprod-tls] backend %s actif (%s)",
         qUtf8Printable(QSslSocket::activeBackend()),
         qUtf8Printable(QSslSocket::sslLibraryVersionString()));
  } else {
    // Message explicite plutot qu'un echec reseau muet trente secondes plus
    // tard. Les deux causes realistes : le fichier n'a pas ete empaquete, ou
    // il l'a ete mais vient d'un Qt plus recent que celui de l'OBS hote, que
    // le chargeur de plugins de Qt refuse alors.
    blog(LOG_ERROR,
         "[semirprod-tls] AUCUN backend TLS — HTTPS et wss:// vont echouer. "
         "Cherche %s/tls/ : soit le backend n'y est pas, soit il a ete compile "
         "avec un Qt plus recent que celui d'OBS (%s).",
         qUtf8Printable(plugIns), qVersion());
  }
}

} // namespace semirprod

#else

namespace semirprod {
void ensureQtTlsBackend() {}
} // namespace semirprod

#endif // __APPLE__
