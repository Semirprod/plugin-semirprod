#include "Config.hpp"

#include "Edition.hpp"

#include <QByteArray>
#include <QHostAddress>
#include <QStringList>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <cstring>

namespace semirprod {

namespace {

// Parse an X.509-style hostname allowlist once at program init. We don't
// need a public suffix list — semirprod.fr and its subdomains are the only
// public surface, and dev machines use 127.0.0.1 / ::1 / .local. Anything
// else (including 10.x.y.z in production) is a misconfiguration and gets
// rejected.
bool hostMatchesAllowlist(const QString& host) {
  if (host.isEmpty()) return false;

  // Boucle locale : BUILDS DE DEV SEULEMENT.
  //
  // Elle etait acceptee dans tous les builds, release compris. Or cette liste
  // garde aussi des URLs qui arrivent du serveur (whipUrl des apercus, qui
  // emporte tout le programme ; URL d'avatar ; downloadUrl d'une mise a jour) :
  // un serveur detourne pouvait donc viser n'importe quel service qui ecoute
  // sur le poste du client. Le pont de l'interface (127.0.0.1:47615) ne passe
  // pas par ici. L'edition cloud n'en a pas besoin non plus : un hote local
  // pose dans sa configuration figure deja dans sa liste (CloudConfig).
  if (Config::isDevBuild()) {
    if (host == QStringLiteral("localhost")) return true;
    const QHostAddress addr(host);
    if (!addr.isNull() && addr.isLoopback()) return true;
  }

  // Au-dela de la boucle locale, la liste depend de l'edition : hotes de
  // production compiles (public, src/public/PublicConfig.cpp) ou hotes deduits
  // de la configuration du Pod (cloud, src/cloud/CloudConfig.cpp).
  const QString h = host.toLower();
  return edition::isAllowedRemoteHost(h);
}

// Hotes de PRODUCTION et les usages sensibles que chacun sert. Doit rester
// d'accord avec la liste de src/public/PublicConfig.cpp (isAllowedRemoteHost) :
// un hote absent d'ici n'est pas restreint par usage, seulement par l'allowlist.
struct ProductionHost {
  const char* host;
  bool whip;
  bool updateDownload;
};
constexpr ProductionHost kProductionHosts[] = {
    {"semirprod.fr", false, false},          // site vitrine
    {"app.semirprod.fr", false, true},       // API + telechargements
    {"auth.semirprod.fr", false, false},     // OAuth
    // live-ws porte AUSSI la signalisation WHIP/WHEP des apercus
    // (LIVE_WEBRTC_BASE_URL cote serveur) : il doit rester autorise pour WHIP.
    {"live-ws.semirprod.fr", true, false},   // WebSocket du poste + WHIP
    {"live.semirprod.fr", true, false},      // ingest / WHIP
};

// Adresse effective : politique de l'edition (voir Edition.hpp). Public :
// constante de production, surcharge par l'environnement en build de dev
// seulement. Cloud : la variable d'environnement seule, sans repli.
QString resolveUrl(const char* prodUrl, const char* envVar) {
  return edition::resolveUrl(prodUrl, envVar);
}

// Caracteres de controle, espaces et antislash : rien de tout ca n'a sa place
// dans une URL de flux, et chacun peut faire lire a ffmpeg ou a QUrl deux
// choses differentes.
bool hasUnsafeUrlCharacters(const QString& url) {
  for (const QChar c : url) {
    if (c.unicode() <= 0x20 || c.unicode() == 0x7F || c == QLatin1Char('\\')) return true;
  }
  return false;
}

// Le texte commence-t-il par `<schema>://` pour l'un des schemas donnes ?
bool startsWithScheme(const QString& url, const QStringList& schemes) {
  for (const QString& scheme : schemes) {
    if (url.startsWith(scheme + QStringLiteral("://"), Qt::CaseInsensitive)) return true;
  }
  return false;
}

// Adresse IP qui ne designe pas Internet : boucle locale, reseaux prives, lien
// local, CGNAT, multidiffusion, non routable. Une IPv4 encapsulee dans une IPv6
// (`::ffff:127.0.0.1`) est ramenee a sa forme IPv4 avant la comparaison.
bool isNonPublicAddress(const QHostAddress& address) {
  QHostAddress a = address;
  bool estIpv4 = false;
  const quint32 v4 = address.toIPv4Address(&estIpv4);
  if (estIpv4) a = QHostAddress(v4);

  static const QList<QPair<QHostAddress, int>> blocs = {
      QHostAddress::parseSubnet(QStringLiteral("0.0.0.0/8")),
      QHostAddress::parseSubnet(QStringLiteral("10.0.0.0/8")),
      QHostAddress::parseSubnet(QStringLiteral("100.64.0.0/10")),
      QHostAddress::parseSubnet(QStringLiteral("127.0.0.0/8")),
      QHostAddress::parseSubnet(QStringLiteral("169.254.0.0/16")),
      QHostAddress::parseSubnet(QStringLiteral("172.16.0.0/12")),
      QHostAddress::parseSubnet(QStringLiteral("192.168.0.0/16")),
      QHostAddress::parseSubnet(QStringLiteral("224.0.0.0/3")),
      QHostAddress::parseSubnet(QStringLiteral("::/127")),
      QHostAddress::parseSubnet(QStringLiteral("fc00::/7")),
      QHostAddress::parseSubnet(QStringLiteral("fe80::/10")),
      QHostAddress::parseSubnet(QStringLiteral("ff00::/8")),
  };
  for (const auto& bloc : blocs) {
    if (a.isInSubnet(bloc)) return true;
  }
  return a.isLoopback();
}

// Hote acceptable pour une URL publique. Les noms ne sont pas resolus.
bool isPublicHost(const QString& rawHost) {
  const QString host = rawHost.toLower();
  if (host.isEmpty()) return false;
  if (host == QStringLiteral("localhost") || host.endsWith(QStringLiteral(".localhost"))) {
    return false;
  }

  const QHostAddress address(host);
  if (!address.isNull()) return !isNonPublicAddress(address);

  // Formes numeriques que QHostAddress refuse mais qu'un resolveur systeme
  // (inet_aton) accepte : `2130706433`, `127.1`, `0x7f.0.0.1`. Un hote fait
  // seulement de chiffres et de points, ou dont un segment commence par `0x`,
  // n'est pas un nom de domaine : refuse.
  bool queChiffresEtPoints = true;
  for (const QChar c : host) {
    if (!c.isDigit() && c != QLatin1Char('.')) {
      queChiffresEtPoints = false;
      break;
    }
  }
  if (queChiffresEtPoints) return false;
  for (const QString& segment : host.split(QLatin1Char('.'))) {
    if (segment.startsWith(QStringLiteral("0x"))) return false;
  }
  return true;
}

} // namespace

QString Config::allowedHostsForLog() {
  return edition::allowedHostsForLog();
}

bool Config::isAllowedHost(const QString& url) {
  const QUrl parsed(url);
  const QString host = parsed.host(); // lowercased, no port
  return hostMatchesAllowlist(host);
}

bool Config::isAllowedHostFor(const QString& url, HostUse use) {
  if (!isAllowedHost(url)) return false;
  const QString host = QUrl(url).host().toLower();
  for (const ProductionHost& prod : kProductionHosts) {
    if (host != QLatin1String(prod.host)) continue;
    bool allowed = false;
    switch (use) {
      case HostUse::Whip:
        allowed = prod.whip;
        break;
      case HostUse::UpdateDownload:
        allowed = prod.updateDownload;
        break;
    }
    return allowed;
  }
  // Hote hors production deja admis par l'allowlist (build de dev, cloud).
  return true;
}

bool Config::isKnownOverlayHost(const QString& host) {
  // Fournisseurs d'overlays et d'alertes pour le direct. Domaines
  // enregistrables : un sous-domaine est accepte (`overlays.x.com`).
  static const QStringList kOverlayProviders = {
      QStringLiteral("streamelements.com"), QStringLiteral("streamlabs.com"),
      QStringLiteral("twitch.tv"),          QStringLiteral("twitchalerts.com"),
      QStringLiteral("kick.com"),           QStringLiteral("youtube.com"),
      QStringLiteral("own3d.tv"),           QStringLiteral("nightbot.tv"),
      QStringLiteral("lumiastream.com"),
  };
  // Semirprod : hotes EXACTS seulement. Les overlays sont servis sur
  // semirprod.fr/overlay/<jeton> (NEXT_PUBLIC_WEB_URL de la plateforme), mais
  // un sous-domaine de semirprod.fr peut etre un site CLIENT heberge : il n'a
  // aucune raison d'etre accepte comme fournisseur d'overlays.
  static const QStringList kSemirprodOverlayHosts = {
      QStringLiteral("semirprod.fr"), QStringLiteral("www.semirprod.fr"),
      QStringLiteral("app.semirprod.fr"),
  };
  if (host.isEmpty()) return false;
  if (kSemirprodOverlayHosts.contains(host)) return true;
  for (const QString& domaine : kOverlayProviders) {
    if (host == domaine || host.endsWith(QLatin1Char('.') + domaine)) return true;
  }
  return false;
}

bool Config::isPublicNetworkUrl(const QString& url, const QStringList& schemes) {
  if (url.isEmpty() || url.size() > 2048) return false;
  if (hasUnsafeUrlCharacters(url)) return false;
  if (!startsWithScheme(url, schemes)) return false;
  const QUrl parsed(url);
  if (!parsed.isValid()) return false;
  return isPublicHost(parsed.host());
}

bool Config::isAllowedIngestUrl(const QString& url) {
  if (url.isEmpty() || url.size() > 2048) return false;
  if (hasUnsafeUrlCharacters(url)) return false;
  static const QStringList kSchemes = {QStringLiteral("srt"), QStringLiteral("rtmp"),
                                       QStringLiteral("rtmps")};
  if (!startsWithScheme(url, kSchemes)) return false;
  const QUrl parsed(url);
  if (!parsed.isValid() || parsed.host().isEmpty()) return false;
  return isAllowedHost(url);
}

QString Config::apiBaseUrl() {
  return resolveUrl(kProdApiBase, kEnvApiBase);
}

QString Config::authBaseUrl() {
  return resolveUrl(kProdAuthBase, kEnvAuthBase);
}

QString Config::liveWsUrl() {
  // Protocole v2 (poste = appareil, jeton renouvele sur le socket). Une
  // surcharge de developpement ecrite pour l'ancien chemin `/ws` est ramenee
  // sur `/v2` : l'ancien chemin sera ferme a la bascule.
  QString url = resolveUrl(kProdLiveWs, kEnvLiveWs);
  if (url.endsWith(QStringLiteral("/ws"))) url.chop(3), url += QStringLiteral("/v2");
  return url;
}

// Plateforme annoncee au manifeste. Le serveur publie un build par plateforme
// et, sans parametre, repond Windows : c'est le contrat
// garde pour le parc 0.1.5 et anterieur, exclusivement Windows. Un build macOS
// ou Linux qui n'enverrait rien se verrait donc proposer l'installeur `.exe`.
#if defined(Q_OS_WIN)
static constexpr const char* kPluginPlatform = "windows";
#elif defined(Q_OS_MACOS)
static constexpr const char* kPluginPlatform = "macos";
#else
static constexpr const char* kPluginPlatform = "linux";
#endif

QString Config::updateManifestUrl() {
  const QString base = resolveUrl(kProdUpdateUrl, kEnvUpdateUrl);
  // Cloud sans variable : chaine vide, refusee plus loin avec son journal.
  if (base.isEmpty()) return base;

  // Ajoute a l'URL resolue, donc aussi a SEMIRPROD_UPDATE_MANIFEST_URL : les
  // lanceurs de staging posent l'URL nue. Une valeur deja presente n'est pas
  // ecrasee, pour pouvoir interroger le manifeste d'une autre plateforme.
  QUrl url(base);
  QUrlQuery query(url);
  if (!query.hasQueryItem(QStringLiteral("platform"))) {
    query.addQueryItem(QStringLiteral("platform"), QString::fromLatin1(kPluginPlatform));
  }
  // Version installee : le serveur rend la severite de la pire version
  // manquee, pas seulement celle de la derniere (une « info » publiee apres
  // une « critique » ne doit pas la masquer).
  if (!query.hasQueryItem(QStringLiteral("current"))) {
    query.addQueryItem(QStringLiteral("current"), QStringLiteral(SEMIRPROD_PLUGIN_VERSION));
  }
  url.setQuery(query);
  return url.toString();
}

QString Config::siteBaseUrl() {
  return resolveUrl(kProdSiteBase, kEnvSiteBase);
}

} // namespace semirprod
