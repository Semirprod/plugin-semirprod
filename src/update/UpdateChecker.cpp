#include "UpdateChecker.hpp"

#include "../Config.hpp"
#include "../http/HttpClient.hpp"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QRandomGenerator>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QtGlobal>

#include <algorithm>

#ifdef Q_OS_WIN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <bcrypt.h>
#elif defined(Q_OS_MACOS)
// Sur macOS le rôle de CNG est tenu par Security.framework, PAS par libcrypto,
// et ce n'est pas une préférence de style : le bundle OBS macOS n'embarque
// aucun libcrypto (son TLS est mbedTLS). Un `find_package(OpenSSL)` n'y a donc
// qu'un seul candidat, celui de Homebrew, et le binaire produit porterait une
// dépendance absolue vers /opt/homebrew/... absente de toute machine cliente —
// module qui refuse de charger, ou pire, updater mort en silence.
//
// Security.framework est l'exact équivalent de bcrypt.dll : un composant
// SYSTÈME, toujours présent, jamais embarqué. La règle du dépôt (« aucune
// bibliothèque de crypto tierce ») est donc tenue sur les trois plateformes,
// chacune avec l'implémentation de son OS.
#  include <CoreFoundation/CoreFoundation.h>
#  include <Security/Security.h>
#elif defined(SEMIRPROD_HAVE_OPENSSL)
// Hors Windows et macOS, le rôle de CNG est tenu par libcrypto. Ce n'est PAS un
// reniement de la règle « aucune bibliothèque de crypto tierce embarquée » :
// libcrypto est ici une bibliothèque système, exactement comme bcrypt.dll
// l'est sur Windows. On n'embarque rien, on lie ce que l'OS fournit — et le
// processus OBS la charge déjà de toute façon (le backend TLS de Qt, librist
// et libsrt en dépendent).
#  include <openssl/core_names.h>
#  include <openssl/ec.h>
#  include <openssl/evp.h>
#  include <openssl/params.h>
#endif

#include <obs-module.h>

#ifndef SEMIRPROD_PLUGIN_VERSION
#define SEMIRPROD_PLUGIN_VERSION "0.0.0"
#endif

namespace semirprod {

namespace {

// Format de signature du manifeste. La v1 (HMAC-SHA256, secret partagé) est
// RETIRÉE, pas dépréciée : la laisser en repli aurait donné une attaque de
// rétrogradation triviale — il aurait suffi d'annoncer `"v": 1` pour repasser
// sur la clé symétrique extractible du binaire.
//
// v2 = ECDSA P-256 / SHA-256, clé publique dans Config::kUpdateSignerPubKeyB64.
constexpr int kManifestSigVersion = 2;
constexpr const char* kManifestSigAlg = "ECDSA-P256-SHA256";

// Longueurs imposées par le format. Fixes, donc vérifiables avant tout appel
// cryptographique — et c'est ce qui rend le parsing sûr : pas de longueur
// choisie par le serveur.
constexpr int kP256PubKeyLen = 65;  // 0x04 || X[32] || Y[32]
constexpr int kP256SigLen = 64;     // r[32] || s[32], surtout PAS du DER

// Décodage STRICT du base64.
//
// `QByteArray::fromBase64()` en options par défaut ignore silencieusement les
// caractères hors alphabet au lieu d'échouer. C'est ce qui avait neutralisé le
// fail-closed de la v1 : le placeholder "REPLACE_WITH_32_RANDOM_BYTES_B64"
// contient 4 `_`, simplement sautés, laissant 27 caractères valides soit
// 20 octets décodés — la sentinelle `size() < 16` était donc FRANCHIE et la
// vérification tournait avec une clé dérivable d'une constante publique du
// dépôt. Toute entrée base64 de ce fichier passe désormais par ici.
QByteArray strictBase64(const char* b64) {
  const auto decoded = QByteArray::fromBase64Encoding(
      QByteArray(b64),
      QByteArray::Base64Encoding | QByteArray::AbortOnBase64DecodingErrors);
  if (!decoded) return {};
  return *decoded;
}

// Clé publique du signeur, ou vide si elle n'est pas configurée. Vide =>
// l'appelant refuse le manifeste.
QByteArray signerPublicKey() {
  // Refus explicite du placeholder, même si quelqu'un le rendait décodable.
  if (QByteArray(Config::kUpdateSignerPubKeyB64).startsWith("REPLACE_WITH")) {
    return {};
  }
  const QByteArray key = strictBase64(Config::kUpdateSignerPubKeyB64);
  // Taille EXACTE et préfixe de point non compressé : une clé tronquée n'est
  // pas une clé configurée.
  if (key.size() != kP256PubKeyLen) return {};
  if (static_cast<unsigned char>(key.at(0)) != 0x04) return {};
  return key;
}

#ifdef Q_OS_MACOS
// Encodage DER d'une des deux moitiés de la signature, en ENTIER ASN.1.
//
// Deux règles, et rater l'une ou l'autre produit une signature valide rejetée
// au hasard selon la valeur de r ou s — un bug qui ne se voit qu'une fois sur
// deux et seulement en production :
//   * pas de zéros de tête (l'encodage DER est unique, donc minimal) ;
//   * un octet 0x00 en tête si le bit de poids fort est à 1, sinon l'entier
//     ASN.1, qui est SIGNÉ, se lirait comme négatif.
QByteArray derInteger(const unsigned char* be32) {
  int i = 0;
  while (i < 31 && be32[i] == 0) ++i;
  QByteArray v(reinterpret_cast<const char*>(be32 + i), 32 - i);
  if (static_cast<unsigned char>(v.at(0)) & 0x80) v.prepend('\0');
  QByteArray out;
  out += static_cast<char>(0x02);
  out += static_cast<char>(v.size());
  out += v;
  return out;
}

// r||s bruts -> SEQUENCE DER, la seule forme que Security.framework accepte
// (kSecKeyAlgorithmECDSASignatureDigestX962SHA256).
//
// Comme dans la branche OpenSSL, la conversion se fait ICI, à partir de deux
// moitiés de 32 octets DÉJÀ vérifiées : le serveur n'envoie jamais de DER,
// donc aucun DER hostile n'est jamais parsé. C'est tout le sens du « surtout
// PAS du DER » plus haut.
//
// Le corps fait au plus 2*(2+33) = 70 octets, donc la longueur tient sur un
// octet et la forme courte suffit — pas de cas long à gérer.
QByteArray rawSigToDer(const QByteArray& sig) {
  const auto* raw = reinterpret_cast<const unsigned char*>(sig.constData());
  const QByteArray body = derInteger(raw) + derInteger(raw + 32);
  QByteArray out;
  out += static_cast<char>(0x30);
  out += static_cast<char>(body.size());
  out += body;
  return out;
}
#endif  // Q_OS_MACOS

// Vérifie une signature ECDSA P-256 / SHA-256 avec l'implémentation du système.
//
// On s'appuie sur CNG (`bcrypt.dll`, DLL système toujours présente) plutôt que
// d'embarquer une bibliothèque de crypto. Deux raisons : le code qu'on
// n'écrit pas ne peut pas être mal intégré, et ce dépôt a déjà payé le prix
// d'un binaire tiers embarqué (Qt6WebSockets, retiré depuis).
bool verifyEcdsaP256(const QByteArray& message, const QByteArray& pubKey,
                     const QByteArray& sig) {
#ifdef Q_OS_WIN
  if (pubKey.size() != kP256PubKeyLen || sig.size() != kP256SigLen) return false;

  // BCRYPT_ECCKEY_BLOB : en-tête (magic + cbKey) suivi de X||Y bruts. On saute
  // donc l'octet 0x04 de préfixe du point non compressé.
  QByteArray blob(static_cast<int>(sizeof(BCRYPT_ECCKEY_BLOB)) + 64, '\0');
  auto* hdr = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob.data());
  hdr->dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
  hdr->cbKey = 32;
  memcpy(blob.data() + sizeof(BCRYPT_ECCKEY_BLOB), pubKey.constData() + 1, 64);

  BCRYPT_ALG_HANDLE alg = nullptr;
  if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(
          &alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0))) {
    return false;
  }

  BCRYPT_KEY_HANDLE key = nullptr;
  NTSTATUS st = BCryptImportKeyPair(
      alg, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key,
      reinterpret_cast<PUCHAR>(blob.data()),
      static_cast<ULONG>(blob.size()), 0);
  if (!BCRYPT_SUCCESS(st)) {
    BCryptCloseAlgorithmProvider(alg, 0);
    return false;
  }

  const QByteArray digest =
      QCryptographicHash::hash(message, QCryptographicHash::Sha256);
  st = BCryptVerifySignature(
      key, nullptr,
      reinterpret_cast<PUCHAR>(const_cast<char*>(digest.constData())),
      static_cast<ULONG>(digest.size()),
      reinterpret_cast<PUCHAR>(const_cast<char*>(sig.constData())),
      static_cast<ULONG>(sig.size()), 0);

  BCryptDestroyKey(key);
  BCryptCloseAlgorithmProvider(alg, 0);
  // Pas de comparaison à temps constant à faire ici : contrairement au HMAC de
  // la v1, la vérification asymétrique ne compare aucun secret. Le verdict de
  // CNG est la réponse.
  return BCRYPT_SUCCESS(st);
#elif defined(Q_OS_MACOS)
  // Mêmes contrôles de longueur et de préfixe que sur les deux autres
  // plateformes, et pour la même raison : ils passent AVANT tout appel
  // cryptographique, donc aucune longueur choisie par le serveur n'atteint la
  // crypto. 0x04 = point non compressé, exigé et non supposé.
  if (pubKey.size() != kP256PubKeyLen || sig.size() != kP256SigLen) return false;
  if (static_cast<unsigned char>(pubKey[0]) != 0x04) return false;

  // SecKeyCreateWithData attend, pour une clé publique EC, exactement le point
  // non compressé 0x04||X||Y — soit le format du signeur, tel quel.
  CFDataRef keyData = CFDataCreate(
      kCFAllocatorDefault,
      reinterpret_cast<const UInt8*>(pubKey.constData()),
      static_cast<CFIndex>(pubKey.size()));
  if (!keyData) return false;

  CFMutableDictionaryRef attrs = CFDictionaryCreateMutable(
      kCFAllocatorDefault, 3, &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks);
  if (!attrs) {
    CFRelease(keyData);
    return false;
  }
  const int keyBits = 256;
  CFNumberRef bits = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &keyBits);
  CFDictionarySetValue(attrs, kSecAttrKeyType, kSecAttrKeyTypeECSECPrimeRandom);
  CFDictionarySetValue(attrs, kSecAttrKeyClass, kSecAttrKeyClassPublic);
  if (bits) CFDictionarySetValue(attrs, kSecAttrKeySizeInBits, bits);

  SecKeyRef key = SecKeyCreateWithData(keyData, attrs, nullptr);
  if (bits) CFRelease(bits);
  CFRelease(attrs);
  CFRelease(keyData);
  if (!key) return false;

  const QByteArray digest =
      QCryptographicHash::hash(message, QCryptographicHash::Sha256);
  const QByteArray der = rawSigToDer(sig);

  CFDataRef digestData = CFDataCreate(
      kCFAllocatorDefault,
      reinterpret_cast<const UInt8*>(digest.constData()),
      static_cast<CFIndex>(digest.size()));
  CFDataRef sigData = CFDataCreate(
      kCFAllocatorDefault,
      reinterpret_cast<const UInt8*>(der.constData()),
      static_cast<CFIndex>(der.size()));

  bool ok = false;
  if (digestData && sigData) {
    // Variante « Digest » et non « Message » : on lui passe l'empreinte déjà
    // calculée, exactement comme à CNG et à EVP_PKEY_verify. Les trois
    // plateformes vérifient donc le même octet, sans qu'aucune ne re-hache.
    ok = SecKeyVerifySignature(key, kSecKeyAlgorithmECDSASignatureDigestX962SHA256,
                               digestData, sigData, nullptr);
  }

  if (digestData) CFRelease(digestData);
  if (sigData) CFRelease(sigData);
  CFRelease(key);
  // Comme sur les deux autres plateformes : rien à comparer en temps constant,
  // la vérification asymétrique ne manipule aucun secret.
  return ok;
#elif defined(SEMIRPROD_HAVE_OPENSSL)
  // Mêmes contrôles de longueur qu'au-dessus, et pour la même raison : ils
  // passent AVANT tout appel cryptographique, donc aucune longueur choisie par
  // le serveur n'atteint la crypto.
  if (pubKey.size() != kP256PubKeyLen || sig.size() != kP256SigLen) return false;
  // 0x04 = point non compressé. On l'exige au lieu de le supposer : les formes
  // compressées (0x02/0x03) font la même longueur qu'un préfixe invalide, et
  // c'est le format du signeur qu'on valide ici, pas ce qu'OpenSSL tolère.
  if (static_cast<unsigned char>(pubKey[0]) != 0x04) return false;

  // Clé publique reconstruite depuis le point brut. EVP_PKEY_fromdata est
  // l'API non dépréciée d'OpenSSL 3 ; EC_KEY_* ferait l'affaire mais crie.
  EVP_PKEY* key = nullptr;
  {
    char group[] = SN_X9_62_prime256v1;
    OSSL_PARAM params[] = {
        OSSL_PARAM_construct_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME, group, 0),
        OSSL_PARAM_construct_octet_string(
            OSSL_PKEY_PARAM_PUB_KEY,
            const_cast<char*>(pubKey.constData()),
            static_cast<size_t>(pubKey.size())),
        OSSL_PARAM_construct_end(),
    };
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr);
    if (!ctx) return false;
    const bool built = EVP_PKEY_fromdata_init(ctx) == 1 &&
                       EVP_PKEY_fromdata(ctx, &key, EVP_PKEY_PUBLIC_KEY, params) == 1;
    EVP_PKEY_CTX_free(ctx);
    if (!built || !key) {
      if (key) EVP_PKEY_free(key);
      return false;
    }
  }

  // r||s bruts -> DER, la seule forme que comprenne EVP_PKEY_verify. La
  // conversion est faite ICI, à partir de deux moitiés de 32 octets déjà
  // vérifiées : le serveur n'envoie jamais de DER, donc on ne parse jamais de
  // DER hostile. C'est exactement la raison du « surtout PAS du DER » plus haut.
  QByteArray der;
  {
    const auto* raw = reinterpret_cast<const unsigned char*>(sig.constData());
    BIGNUM* r = BN_bin2bn(raw, 32, nullptr);
    BIGNUM* s = BN_bin2bn(raw + 32, 32, nullptr);
    ECDSA_SIG* ecsig = ECDSA_SIG_new();
    if (!r || !s || !ecsig) {
      if (r) BN_free(r);
      if (s) BN_free(s);
      if (ecsig) ECDSA_SIG_free(ecsig);
      EVP_PKEY_free(key);
      return false;
    }
    // set0 prend possession de r et s dès qu'elle réussit : ne pas les libérer
    // après, sous peine de double free.
    if (ECDSA_SIG_set0(ecsig, r, s) != 1) {
      BN_free(r);
      BN_free(s);
      ECDSA_SIG_free(ecsig);
      EVP_PKEY_free(key);
      return false;
    }
    unsigned char* out = nullptr;
    const int len = i2d_ECDSA_SIG(ecsig, &out);
    if (len > 0 && out) der = QByteArray(reinterpret_cast<char*>(out), len);
    if (out) OPENSSL_free(out);
    ECDSA_SIG_free(ecsig);
    if (der.isEmpty()) {
      EVP_PKEY_free(key);
      return false;
    }
  }

  const QByteArray digest =
      QCryptographicHash::hash(message, QCryptographicHash::Sha256);

  bool ok = false;
  {
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(key, nullptr);
    if (ctx && EVP_PKEY_verify_init(ctx) == 1) {
      // Pas de md positionné : pour une clé EC, EVP_PKEY_verify attend
      // directement l'empreinte, pas le message.
      ok = EVP_PKEY_verify(
               ctx, reinterpret_cast<const unsigned char*>(der.constData()),
               static_cast<size_t>(der.size()),
               reinterpret_cast<const unsigned char*>(digest.constData()),
               static_cast<size_t>(digest.size())) == 1;
    }
    if (ctx) EVP_PKEY_CTX_free(ctx);
  }
  EVP_PKEY_free(key);
  // Comme côté CNG : rien à comparer en temps constant, la vérification
  // asymétrique ne manipule aucun secret. Le verdict d'OpenSSL est la réponse.
  return ok;
#else
  // Ni CNG, ni Security.framework, ni libcrypto => refus. C'est volontairement un échec fermé : sans
  // moyen de vérifier la signature, la seule réponse sûre est « non ». On ne
  // retire jamais le contrôle pour faire passer la mise à jour.
  Q_UNUSED(message);
  Q_UNUSED(pubKey);
  Q_UNUSED(sig);
  return false;
#endif
}

// Message canonique v2 : champs séparés par des sauts de ligne, ordre fixe.
//
// Le premier champ est le numéro de format. C'est une séparation de domaine :
// sans lui, une signature valide pour un format pourrait être rejouée sur un
// autre qui produirait par hasard les mêmes octets.
//
// `sha256` et `sizeBytes` sont DANS le message signé. En v1 `sizeBytes` vivait
// à côté de la signature sans être couvert par elle : il était donc réécrivable
// à volonté, et ne valait rien comme contrôle d'intégrité.
//
// Ajouter un champ impose de bumper kManifestSigVersion ET le signeur du
// backend, sinon un attaquant glisse des champs sous une ancienne signature.
QByteArray canonicalMessage(const QString& version,
                            const QString& downloadUrl,
                            const QString& notes,
                            const QString& severity,
                            const QString& sha256Hex,
                            qint64 sizeBytes) {
  QByteArray out;
  out += QByteArray::number(kManifestSigVersion);
  out += '\n';
  out += version.toUtf8();
  out += '\n';
  out += downloadUrl.toUtf8();
  out += '\n';
  out += notes.toUtf8();
  out += '\n';
  out += severity.toUtf8();
  out += '\n';
  out += sha256Hex.toUtf8();
  out += '\n';
  out += QByteArray::number(sizeBytes);
  out += '\n';
  return out;
}

bool verifySignature(const QString& version,
                     const QString& downloadUrl,
                     const QString& notes,
                     const QString& severity,
                     const QString& sha256Hex,
                     qint64 sizeBytes,
                     const QString& alg,
                     int sigVersion,
                     const QString& sigB64) {
  if (alg != QLatin1String(kManifestSigAlg)) {
    blog(LOG_WARNING, "[semirprod-studio] update signature alg=%s not supported (want %s)",
         alg.toUtf8().constData(), kManifestSigAlg);
    return false;
  }
  // Égalité STRICTE, jamais `<=` : accepter une version de format antérieure
  // rouvrirait la v1 et son secret symétrique extractible du binaire. Une
  // rétrogradation doit être aussi impossible qu'une forgerie.
  if (sigVersion != kManifestSigVersion) {
    blog(LOG_WARNING,
         "[semirprod-studio] update signature version=%d not supported (want %d)",
         sigVersion, kManifestSigVersion);
    return false;
  }

  const QByteArray pubKey = signerPublicKey();
  if (pubKey.isEmpty()) {
    blog(LOG_ERROR,
         "[semirprod-studio] update signer public key not configured — refusing manifest");
    return false;
  }

  // Longueur exacte exigée AVANT tout appel crypto : la signature vient du
  // serveur, sa taille ne doit jamais piloter quoi que ce soit.
  const QByteArray sig = strictBase64(sigB64.toLatin1().constData());
  if (sig.size() != kP256SigLen) {
    blog(LOG_WARNING,
         "[semirprod-studio] update signature malformee (%d octets, attendu %d) — manifeste rejete",
         static_cast<int>(sig.size()), kP256SigLen);
    return false;
  }

  const QByteArray msg = canonicalMessage(version, downloadUrl, notes, severity,
                                          sha256Hex, sizeBytes);
  if (!verifyEcdsaP256(msg, pubKey, sig)) {
    blog(LOG_WARNING,
         "[semirprod-studio] update signature mismatch — manifest rejected");
    return false;
  }
  return true;
}

// Retire le `v` de tete et le suffixe de pre-version (voir isNewer).
QString normalizeVersion(QString version) {
  if (version.startsWith('v') || version.startsWith('V')) version.remove(0, 1);
  const int dash = version.indexOf('-');
  if (dash >= 0) version.truncate(dash);
  return version;
}

// Tous les segments sont des nombres : isNewer() peut comparer pour de vrai.
bool isComparableVersion(const QString& version) {
  const QStringList parts = normalizeVersion(version).split('.');
  for (const QString& part : parts) {
    bool ok = false;
    part.toInt(&ok);
    if (!ok) return false;
  }
  return true;
}

// Rythme des verifications. Un cycle FIXE ferait verifier tout le parc au meme
// instant des qu'un evenement commun l'a aligne (reprise apres une panne du
// serveur, soiree de lancement) : chaque cycle tire donc son propre decalage.
constexpr int kCheckIntervalMs = 6 * 60 * 60 * 1000;
constexpr int kCheckJitterMs = 30 * 60 * 1000;

// Reprise apres echec : 5, 10, 20, 40, 80, 160, 320 min, puis 360 min. Le
// decalage (+/- 20 %) compte ici plus qu'ailleurs : une panne du serveur fait
// echouer tout le parc en meme temps, et il reviendrait d'un seul bloc.
constexpr int kRetryBaseMs = 5 * 60 * 1000;
constexpr int kRetryJitterDivisor = 5;
constexpr int kMaxCountedFailures = 8;

// `failures` commence a 1. Plafonne a la duree du cycle normal, avant decalage :
// a +/- 20 %, un essai peut donc partir des 288 min, contre 330 min au plus tot
// pour un poste en bonne sante.
int backoffDelayMs(int failures) {
  const int shift = std::clamp(failures - 1, 0, kMaxCountedFailures - 1);
  const qint64 delayMs = static_cast<qint64>(kRetryBaseMs) << shift;
  return static_cast<int>(std::min<qint64>(delayMs, kCheckIntervalMs));
}

// `baseMs` a +/- `maxDeltaMs`, `draw` etant un tirage aleatoire. Separe du
// generateur pour rester testable.
int withJitter(int baseMs, int maxDeltaMs, quint32 draw) {
  const qint64 span = 2LL * maxDeltaMs + 1;
  return static_cast<int>(baseMs - maxDeltaMs + static_cast<qint64>(draw) % span);
}

} // namespace

UpdateChecker::UpdateChecker(HttpClient* http, QObject* parent)
    : QObject(parent), http_(http), timer_(new QTimer(this)) {
  // Un seul coup a la fois : c'est la fin d'une verification qui arme la
  // suivante. Il ne peut donc jamais y avoir deux requetes en vol, ni un minuteur
  // qui tombe pendant qu'une reponse se fait attendre.
  timer_->setSingleShot(true);
  connect(timer_, &QTimer::timeout, this, &UpdateChecker::check);
}

void UpdateChecker::start() {
  check();
}

void UpdateChecker::stop() {
  stopped_ = true;
  timer_->stop();
}

void UpdateChecker::check() {
  if (!http_ || inFlight_ || stopped_) return;
  // Pose AVANT l'appel : HttpClient rappelle de facon synchrone quand il refuse
  // l'URL. Elle reste posee pendant le traitement, car un abonne de
  // updateAvailable peut redemander une verification (enterPluginBlocked) : ce
  // serait sinon une seconde requete lancee depuis le rappel de la premiere.
  inFlight_ = true;
  timer_->stop();
  const QString url = Config::updateManifestUrl();
  QPointer<UpdateChecker> self(this);
  http_->getJson(url, QString(), [self](const HttpResponse& res, const QJsonDocument& doc) {
    if (!self) return;
    if (self->stopped_) {
      self->inFlight_ = false;
      return;
    }
    const CheckOutcome outcome = self->handleManifest(res, doc);
    if (!self) return;
    self->inFlight_ = false;
    self->scheduleNext(outcome);
  });
}

void UpdateChecker::scheduleNext(CheckOutcome outcome) {
  if (stopped_) return;
  const quint32 draw = QRandomGenerator::global()->generate();
  if (outcome == CheckOutcome::Failed) {
    failures_ = std::min(failures_ + 1, kMaxCountedFailures);
    const int baseMs = backoffDelayMs(failures_);
    const int delayMs = withJitter(baseMs, baseMs / kRetryJitterDivisor, draw);
    blog(LOG_INFO, "[semirprod-studio] update check: nouvel essai dans %d s (echec %d)",
         delayMs / 1000, failures_);
    timer_->start(delayMs);
    return;
  }
  // Seul un manifeste verifie remet la reprise a zero. Un 404 ne prouve rien sur
  // le chemin de confiance : il suit le cycle normal sans effacer le compteur.
  if (outcome == CheckOutcome::Verified) failures_ = 0;
  timer_->start(withJitter(kCheckIntervalMs, kCheckJitterMs, draw));
}

UpdateChecker::CheckOutcome UpdateChecker::handleManifest(const HttpResponse& res,
                                                          const QJsonDocument& doc) {
  if (!res.ok() || !doc.isObject()) {
    blog(LOG_INFO, "[semirprod-studio] update check failed (status=%d)", res.statusCode);
    // Rien n'est parti (URL absente ou hors allowlist), ou le serveur dit qu'il
    // n'a aucun build pour cette plateforme : reessayer plus tot ne changera
    // rien. Tout le reste (reseau, 5xx, portail captif) peut se reparer seul.
    return (res.refusedLocally || res.statusCode == 404) ? CheckOutcome::FailedNoRetry
                                                         : CheckOutcome::Failed;
  }
  // L'API enveloppe ses reponses dans { success, data } — comme /api/me et
  // /api/live/access. Lire la RACINE donnait une version vide, donc une sortie
  // silencieuse trois lignes plus bas : aucune mise a jour n'a jamais pu etre
  // signalee, quelle que soit la version installee, et rien dans le journal ne
  // le disait. Meme bug que celui deja corrige sur /api/me.
  //
  // Tolerant dans les deux sens : si l'API cesse un jour d'envelopper, la
  // racine reprend la main sans changement de code.
  const auto root = doc.object();
  const auto data = root.value(QStringLiteral("data")).toObject();
  const auto obj = data.isEmpty() ? root : data;

  const QString latest = obj.value(QStringLiteral("version")).toString();
  const QString downloadUrl = obj.value(QStringLiteral("downloadUrl")).toString();
  const QString notes = obj.value(QStringLiteral("notes")).toString();
  QString severity = obj.value(QStringLiteral("severity")).toString();
  if (severity != QStringLiteral("recommended") && severity != QStringLiteral("critical")) {
    severity = QStringLiteral("info");
  }
  if (latest.isEmpty()) {
    // Plus de sortie muette : c'est ce silence qui a cache le bug ci-dessus
    // pendant des semaines. Un manifeste illisible doit se voir dans le
    // journal, c'est le seul endroit ou l'on peut le constater.
    blog(LOG_WARNING,
         "[semirprod-studio] manifeste de mise a jour sans champ 'version' — "
         "ignore (%d octets recus)",
         static_cast<int>(res.body.size()));
    return CheckOutcome::Failed;
  }

  // TOUT manifeste doit être signé, sans exception.
  //
  // La vérification vivait auparavant à l'INTÉRIEUR d'un
  // `if (!downloadUrl.isEmpty())`, alors que l'émission vers l'UI est à
  // l'extérieur : un manifeste avec `downloadUrl: ""` ne subissait donc AUCUN
  // contrôle, et `version`, `notes` et `severity` — tous choisis par le
  // serveur — traversaient jusqu'à une QMessageBox modale « Mise à jour
  // obligatoire » affichée dans OBS. Un serveur compromis pouvait faire
  // afficher son propre texte au streamer sans posséder aucun secret.
  //
  // Un `downloadUrl` vide fait partie du message canonique, il est donc
  // signable tel quel : rien n'empêche le backend de signer ce cas.
  // Empreinte et taille de l'artefact designe par `downloadUrl`. Les deux
  // sont COUVERTES par la signature (voir canonicalMessage) : c'est ce qui
  // les rend exploitables. En v1, `sizeBytes` vivait a cote de la signature
  // sans etre signe, donc reecrivable a volonte.
  //
  // Le plugin ne telecharge pas lui-meme — il ouvre l'URL dans le
  // navigateur — donc il ne peut pas VERIFIER le fichier. Ce qu'il peut, et
  // ce qui a de la valeur pour un produit distribue sans signature
  // Authenticode : republier une empreinte authentifiee, contre laquelle un
  // client ou le support compare son telechargement.
  const QString sha256Hex =
      obj.value(QStringLiteral("sha256")).toString().toLower();
  const qint64 sizeBytes =
      static_cast<qint64>(obj.value(QStringLiteral("sizeBytes")).toDouble(0));

  const auto sigObj = obj.value(QStringLiteral("signature")).toObject();
  const QString sigAlg = sigObj.value(QStringLiteral("alg")).toString();
  const int sigVersion = sigObj.value(QStringLiteral("v")).toInt(0);
  const QString sigB64 = sigObj.value(QStringLiteral("sig")).toString();
  if (!verifySignature(latest, downloadUrl, notes, severity, sha256Hex,
                       sizeBytes, sigAlg, sigVersion, sigB64)) {
    return CheckOutcome::Failed;
  }

  if (!downloadUrl.isEmpty()) {
    // Belt-and-suspenders: even after signature verification, the download
    // URL itself must point at our infra. Otherwise an attacker who finds
    // a way to sign with a stolen secret can still hijack where the
    // download actually lands (signed URL → payload served from attacker).
    // Et pas n'importe quel hote autorise : seulement celui des
    // telechargements (Config::HostUse::UpdateDownload), jamais le site
    // vitrine ni l'hote live.
    if (!Config::isAllowedHostFor(downloadUrl, Config::HostUse::UpdateDownload)) {
      blog(LOG_ERROR,
           "[semirprod-studio] update signed but downloadUrl not on allowlist: %s",
           downloadUrl.toUtf8().constData());
      return CheckOutcome::Failed;
    }
    // HTTPS obligatoire : un lien http:// vers un hote autorise laisserait
    // n'importe qui sur le reseau du client remplacer l'installeur en route.
    if (QUrl(downloadUrl).scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) != 0) {
      blog(LOG_ERROR,
           "[semirprod-studio] update signed but downloadUrl is not https: %s",
           downloadUrl.toUtf8().constData());
      return CheckOutcome::Failed;
    }
  }

  // isNewer() rend faux pour une version illisible. Sans ce filtre, une telle
  // version passerait pour « rien de plus recent » et effacerait une offre.
  if (!isComparableVersion(latest)) {
    blog(LOG_WARNING, "[semirprod-studio] update manifest: version illisible, ignoree");
    return CheckOutcome::Failed;
  }

  const QString current = QStringLiteral(SEMIRPROD_PLUGIN_VERSION);
  if (isNewer(latest, current)) {
    blog(LOG_INFO, "[semirprod-studio] update available: %s -> %s (severity=%s)",
         current.toUtf8().constData(), latest.toUtf8().constData(),
         severity.toUtf8().constData());
    emit updateAvailable(latest, downloadUrl, notes, severity);
  } else {
    // Reponse normale, pas une anomalie : une version retiree ou bloquee sort du
    // manifeste, qui retombe sur le build publie precedent, parfois plus ancien
    // que celui qui tourne ici.
    blog(LOG_INFO, "[semirprod-studio] no update: latest %s, running %s",
         latest.toUtf8().constData(), current.toUtf8().constData());
    emit noUpdateAvailable();
  }
  // Rien apres l'emission ne touche a l'objet : un abonne peut tout faire.
  return CheckOutcome::Verified;
}

bool UpdateChecker::isNewer(const QString& candidate, const QString& current) {
  // Strip leading 'v' if present, drop any pre-release suffix after '-'.
  // Pre-release ordering is deliberately coarse — we treat 1.0.0-rc.1 as
  // EQUAL to 1.0.0 (so we don't pop a "you need to upgrade" nag for a
  // release-candidate of the version we're already on). The downside is
  // that 1.0.0-rc.1 < 1.0.0 in semver, but for the plugin's purposes
  // "pre-release of X" and "X" are the same upgrade target.
  const QStringList a = normalizeVersion(candidate).split('.');
  const QStringList b = normalizeVersion(current).split('.');
  const int n = std::max(a.size(), b.size());
  for (int i = 0; i < n; ++i) {
    bool okA = false, okB = false;
    const int x = i < a.size() ? a[i].toInt(&okA) : 0;
    const int y = i < b.size() ? b[i].toInt(&okB) : 0;
    if (!okA && i < a.size()) return false;
    if (!okB && i < b.size()) return false;
    if (x > y) return true;
    if (x < y) return false;
  }
  return false;
}

} // namespace semirprod
