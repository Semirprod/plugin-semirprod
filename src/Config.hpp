#pragma once

#include <QString>
#include <QStringList>

namespace semirprod {

// ============================================================================
// Endpoints — production values are HARDCODED at compile time.
//
// The plugin is meant to be 100% dependent on Semirprod infrastructure. To
// prevent trivial attacks (env var pointing at evil.com that re-issues fake
// JWTs), the production URLs are baked into the binary and the env-var
// override path is only compiled in when SEMIRPROD_DEV_BUILD is defined.
//
// In a dev build (set by CMakeLists.txt when CMAKE_BUILD_TYPE=Debug or when
// the maintainer explicitly opts in), the env vars win so the plugin can be
// pointed at staging without a rebuild. In a release build (the default and
// the only thing we ship), env vars are ignored.
//
// The resolved host must also match the allowlist below — even a dev build
// cannot accidentally (or maliciously) be re-pointed at a non-Semirprod host.
// ============================================================================

struct Config {
  // Production endpoints — change here, rebuild, ship. No runtime override.
  static constexpr const char* kProdApiBase     = "https://app.semirprod.fr";
  static constexpr const char* kProdAuthBase    = "https://auth.semirprod.fr";
  static constexpr const char* kProdLiveWs      = "wss://live-ws.semirprod.fr/v2";
  static constexpr const char* kProdUpdateUrl   = "https://app.semirprod.fr/api/plugin/latest";
  // Site vitrine : c'est là que se souscrit l'abonnement Studio. Distinct du
  // tableau de bord (app.) — un compte sans abonnement n'a rien à y faire, on
  // l'envoie sur l'offre.
  static constexpr const char* kProdSiteBase    = "https://semirprod.fr";

  // Client identity — registered OAuth application on auth.semirprod.fr.
  // Changing this without re-registering on the auth server will brick login.
  static constexpr auto clientId = "obs-plugin";
  static constexpr auto scope    = "openid profile email offline_access obs:streams obs:scenes";

  // Threshold: refresh the JWT when less than this remains on its lifetime.
  // Pro-active refresh (driven by a timer in the dock) uses this — the 401
  // retry path stays as a safety net for clock skew / unexpected early exp.
  static constexpr int refreshThresholdSeconds = 300;
  // Poste v2 : avance avec laquelle la session d'appareil (JWT de 15 min) est
  // renouvelee sur la liaison ouverte. Meme valeur que `renewBeforeS` du pont.
  static constexpr int sessionRenewBeforeSeconds = 120;

  static constexpr auto keychainServiceName = "fr.semirprod.obs-plugin";
  static constexpr auto configFileName      = "semirprod-studio.json";

  // AUCUN EPINGLAGE TLS n'est actif. Toutes les connexions (HTTPS et wss://)
  // reposent sur la validation TLS STANDARD de Qt : chaine de confiance du
  // systeme + nom d'hote. Toute erreur TLS fait echouer la connexion ; aucune
  // n'est ignoree (LiveClient::evaluateSslErrors rend toujours une liste vide,
  // kPinningArmed = false).
  //
  // Les deux constantes ci-dessous sont des EMPLACEMENTS RESERVES, jamais
  // compares a quoi que ce soit en pratique : la comparaison n'aurait lieu que
  // dans le gestionnaire sslErrors, c'est-a-dire apres un echec de validation,
  // et le hachage calcule (publicKey().toDer()) n'est de toute facon pas celui
  // d'un SPKI au format attendu. Ne pas les presenter comme une protection.
  static constexpr const char* kLiveWsSpkiPinPrimary   = "REPLACE_WITH_SHA256_B64_OF_PROD_LEAF_SPKI";
  static constexpr const char* kLiveWsSpkiPinSecondary = "REPLACE_WITH_SHA256_B64_OF_NEXT_SPKI";

  // Clé PUBLIQUE du signeur de manifestes de mise à jour (format v2).
  //
  // Pourquoi une clé publique et plus un secret partagé : le plugin est
  // distribué, donc tout ce qu'il contient appartient à celui qui l'achète. Le
  // HMAC-SHA256 de la v1 utilisait une clé SYMÉTRIQUE compilée dans le binaire
  // — autrement dit chaque client détenait la clé qui authentifie les mises à
  // jour, extractible de `.rdata` en trois minutes, et donc capable de forger
  // un manifeste. En asymétrique, le binaire ne porte que de quoi VÉRIFIER :
  // l'extraire ne donne rien.
  //
  // Point EC non compressé, 65 octets (0x04 || X[32] || Y[32]), encodé en
  // base64. Courbe P-256, signature ECDSA/SHA-256.
  //
  // Générer la paire, côté backend :
  //   openssl ecparam -name prime256v1 -genkey -noout -out manifest-signer.pem
  //   openssl ec -in manifest-signer.pem -pubout -outform DER | tail -c 65 | base64 -w0
  //
  // (une seule ligne : une continuation « \ » en fin de commentaire // fait
  //  avaler la ligne suivante par le commentaire, et gcc previent -Wcomment
  //  dans CHAQUE unite de compilation qui inclut ce fichier)
  //
  // Tant que la constante vaut le placeholder, TOUT manifeste est refusé — le
  // fail-closed est vérifié dans UpdateChecker::verifySignature.
  // Paire générée le 2026-08-15. La clé privée reste côté serveur, hors de
  // ce dépôt, et ne quitte pas l'infrastructure de signature. Celle-ci est
  // publique par construction :
  // la commiter est sans effet sur la sécurité, c'est tout l'intérêt de
  // l'asymétrique.
  //
  // La remplacer invalide les manifestes signés par l'ancienne clé privée :
  // les deux côtés se changent ensemble, sinon plus aucune mise à jour ne
  // passe — et les binaires déjà distribués, eux, gardent l'ancienne.
  static constexpr const char* kUpdateSignerPubKeyB64 =
      "BF498KyvSlUfzU52JGOI8UK+v0jMRKHltTVkQ6sfM9Lyxb5x+UXuvRCcp5mo2kkx25UHhuWZxaIiYX3j2fmDmyo=";

  // Hosts the plugin is willing to talk to. EXACT host match against a fixed
  // list (edition::isAllowedRemoteHost : src/public/PublicConfig.cpp ou
  // src/cloud/CloudConfig.cpp) — no wildcard, no suffix match. Anything
  // outside this set is rejected before a single byte is sent — even in dev
  // builds. Pour un usage precis (WHIP, telechargement de mise a jour,
  // redirection porteuse du bearer), utiliser les gardes dediees ci-dessous.
  static bool isAllowedHost(const QString& url);

  /** Usage d'une URL qui arrive du serveur et part avec quelque chose de
      precieux : le programme (WHIP) ou un executable (mise a jour). */
  enum class HostUse {
    Whip,            // apercus WHIP : hotes live (live-ws., live.) uniquement
    UpdateDownload,  // downloadUrl du manifeste : hote(s) de l'app uniquement
  };

  /**
   * isAllowedHost() ET, si l'hote est un hote de PRODUCTION, qu'il serve bien
   * cet usage. Avant, n'importe quel hote autorise valait pour tout : le site
   * vitrine (semirprod.fr) ou l'API etaient des destinations WHIP valides, et
   * le site vitrine une source valide d'installeur. Les hotes acceptes hors
   * production (build de dev, configuration de l'edition cloud) restent soumis
   * a la seule allowlist, deja propre a leur configuration.
   */
  static bool isAllowedHostFor(const QString& url, HostUse use);

  /**
   * Vrai si `host` (deja en minuscules) est, ou est un sous-domaine de, un
   * fournisseur d'overlays connu. Sert a l'IMPORT d'une collection : seule une
   * source navigateur pointant chez l'un d'eux garde son URL (sans aucun
   * controle sur OBS). Liste VOLONTAIREMENT courte, dans Config.cpp
   * (kOverlayProviders) : pour l'etendre, y ajouter le domaine enregistrable
   * du service (pas un sous-domaine), apres avoir verifie qu'il sert des
   * overlays et pas du contenu deposable par n'importe qui. Les hotes
   * Semirprod sont une liste EXACTE (kSemirprodOverlayHosts) : les
   * sous-domaines de semirprod.fr hebergent aussi des sites clients.
   */
  static bool isKnownOverlayHost(const QString& host);

  /**
   * Vrai si `url` commence par l'un des `schemes` (« rtmp », « srt »…, suivi de
   * `://`) et vise un hote PUBLIC : ni vide, ni `localhost`, ni adresse IP de
   * boucle locale, privee, lien-local, CGNAT ou non routable, ni forme numerique
   * exotique (`2130706433`, `0x7f.1`) qu'un resolveur ramenerait sur la machine.
   *
   * Pour les URLs qui ne viennent PAS de notre infrastructure mais ne doivent
   * pas pour autant viser le poste ou son reseau local : destination de
   * diffusion poussee par le site (Twitch, YouTube…), entree reseau d'une
   * source media importee. Les noms de domaine ne sont pas resolus : la garde
   * porte sur ce qui est ecrit, pas sur ce que le DNS rendra.
   */
  static bool isPublicNetworkUrl(const QString& url, const QStringList& schemes);

  /**
   * Vrai si `url` est une URL de lecture de flux (`srt://`, `rtmp://`,
   * `rtmps://`) dont l'hote passe isAllowedHost().
   *
   * Le prefixe est verifie sur le TEXTE, avant toute analyse : l'URL finit
   * telle quelle dans ffmpeg, qui comprend aussi `file:`, `concat:`,
   * `subfile:` ou un chemin nu, et lirait alors un fichier du poste.
   */
  static bool isAllowedIngestUrl(const QString& url);

  /**
   * Les hotes autorises, sous forme lisible, POUR LE JOURNAL UNIQUEMENT.
   *
   * Un refus d'allowlist qui ne dit ni l'hote refuse ni ce qui etait attendu se
   * diagnostique a l'aveugle : un whipUrl envoye sur un hote de production a
   * un binaire configure pour un autre hote ne demarrait jamais l'apercu, sans
   * rien afficher, et le journal n'avait qu'une ligne sans aucun nom.
   *
   * Ne PAS s'en servir pour decider : la decision reste isAllowedHost().
   */
  static QString allowedHostsForLog();

  // Resolved endpoints, through edition::resolveUrl (Edition.hpp). In a
  // public release build these ALWAYS return the prod constant. In a public
  // dev build (SEMIRPROD_DEV_BUILD defined) the env var wins IF the resulting
  // host passes isAllowedHost().
  // Noms des variables d'environnement qui portent ces adresses. Un seul
  // endroit ou ils sont ecrits : le chemin de surcharge des builds de dev et
  // l'edition cloud (src/cloud/CloudConfig.cpp) lisent les memes.
  static constexpr const char* kEnvApiBase    = "SEMIRPROD_API_BASE";
  static constexpr const char* kEnvAuthBase   = "SEMIRPROD_AUTH_BASE";
  static constexpr const char* kEnvLiveWs     = "SEMIRPROD_LIVE_WS";
  static constexpr const char* kEnvSiteBase   = "SEMIRPROD_SITE_BASE";
  static constexpr const char* kEnvUpdateUrl  = "SEMIRPROD_UPDATE_MANIFEST_URL";

  //
  // EXCEPTION EDITION CLOUD (instance infogeree) : voir src/cloud/CloudConfig.hpp.
  // Dans cette edition, ces fonctions rendent une chaine VIDE quand la
  // variable manque.
  // Aucun appelant n'a besoin d'etre adapte pour autant : une URL vide est
  // invalide, donc refusee par sanitizeUrlOrLog dans HttpClient et par les
  // gardes de LiveClient, avec une trace dans le journal d'OBS.
  static QString apiBaseUrl();
  static QString authBaseUrl();
  static QString liveWsUrl();
  static QString updateManifestUrl();
  static QString siteBaseUrl();

  // True if this binary was compiled with SEMIRPROD_DEV_BUILD. Useful for
  // surfacing a "DEVELOPMENT BUILD" badge in the UI so a leaked dev binary
  // is obviously not production.
  static constexpr bool isDevBuild() {
#ifdef SEMIRPROD_DEV_BUILD
    return true;
#else
    return false;
#endif
  }
};

} // namespace semirprod
