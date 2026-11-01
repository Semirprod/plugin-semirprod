// Edition publique uniquement : ce fichier n'est compile que si
// SEMIRPROD_EDITION=public (voir CMakeLists.txt). Pendant cloud :
// src/cloud/CloudConfig.cpp.

#include "Config.hpp"
#include "Edition.hpp"

#include <QByteArray>
#include <QStringList>
#include <QtGlobal>

namespace semirprod::edition {

// Resolve a URL with the dev/prod policy. In release builds, env vars are
// ignored entirely and we always return the production constant. In dev
// builds, the env var wins, but the resulting host must still match the
// allowlist — if not, we silently fall back to the prod constant (and log
// a warning so the dev notices they typed the wrong hostname).
QString resolveUrl(const char* prodUrl, const char* envVar) {
#ifdef SEMIRPROD_DEV_BUILD
  const QByteArray env = qgetenv(envVar);
  if (!env.isEmpty()) {
    const QString candidate = QString::fromUtf8(env);
    if (Config::isAllowedHost(candidate)) {
      return candidate;
    }
    // Fall through to prod and log so the dev knows their env var was bogus.
    // We don't blog() here — Config.cpp is included in headers that might
    // be pulled in before obs-module.h; the caller logs the fallback.
  }
#else
  // Release build: explicitly check that SEMIRPROD_DEV_BUILD is NOT set.
  // If a user/attacker set the env var, the runtime value is silently
  // discarded. Documented behavior — see README.
  Q_UNUSED(envVar);
#endif
  return QString::fromLatin1(prodUrl);
}

bool isAllowedRemoteHost(const QString& h) {
  // Production surface — LISTE EXACTE, plus de correspondance par suffixe.
  //
  // Le `endsWith('.' + suffixe)` précédent acceptait n'importe quel
  // sous-domaine de semirprod.fr. Or la plateforme y héberge des SITES
  // CLIENTS : un sous-domaine tenu par un tiers devenait donc une destination
  // valide pour le `whipUrl` de l'aperçu programme (qui pousse tout le
  // programme, vidéo et audio), pour le `downloadUrl` d'une mise à jour, et
  // pour la garde anti-redirection qui réémet le bearer.
  //
  // Ces cinq hôtes couvrent toutes les URLs que le plugin construit (API +
  // téléchargements sur app., OAuth sur auth., WebSocket et signalisation WHIP
  // sur live-ws., ingest sur live.). Ajouter un hôte ici est un changement de
  // binaire : une omission coupe la fonction correspondante chez tous les
  // clients. Les usages sensibles (WHIP, téléchargement de mise à jour) sont
  // en plus restreints par hôte : Config::isAllowedHostFor (Config.cpp).
  static const QStringList kAllowedHosts{
      QStringLiteral("semirprod.fr"),
      QStringLiteral("app.semirprod.fr"),
      QStringLiteral("auth.semirprod.fr"),
      QStringLiteral("live-ws.semirprod.fr"),
      QStringLiteral("live.semirprod.fr"),
  };
  if (kAllowedHosts.contains(h)) return true;

  // Dev builds additionally accept mDNS ".local" names so the plugin can be
  // pointed at a staging box without a rebuild. Loopback is accepted by Config
  // in dev builds only (the WebUI bridge on 127.0.0.1 does not go through the
  // allowlist).
  // RFC1918 addresses (10.x / 192.168.x) stay rejected on purpose — a prod
  // build talking to a private IP is a misconfiguration we want to fail loud.
  if (Config::isDevBuild()) {
    if (h.endsWith(QStringLiteral(".local"), Qt::CaseInsensitive)) return true;

    // Hôtes de pré-production, fournis A LA COMPILATION d'un build de dev
    // (option CMake SEMIRPROD_DEV_HOSTS, vide par défaut). Volontairement DANS
    // ce bloc et pas dans kAllowedHosts : un binaire de release doit rester
    // exactement aussi strict. Dans la liste principale, ces hôtes
    // deviendraient une destination valide chez les clients pour le
    // `downloadUrl` d'une mise à jour, pour le `whipUrl` de l'aperçu programme
    // et pour la garde anti-redirection qui réémet le bearer.
    //
    // La macro n'est définie que par un build de dev : en release, la liste
    // n'existe même pas dans le binaire livré.
#ifdef SEMIRPROD_DEV_HOSTS
    static const QStringList kDevHosts =
        QStringLiteral(SEMIRPROD_DEV_HOSTS).split(QLatin1Char(','), Qt::SkipEmptyParts);
    if (kDevHosts.contains(h, Qt::CaseInsensitive)) return true;
#endif
  }

  return false;
}

QString allowedHostsForLog() {
  return QStringLiteral("liste de production compilee");
}

}  // namespace semirprod::edition
