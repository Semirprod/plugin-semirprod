#include "TokenStore.hpp"
#include "../Config.hpp"
#include "../Edition.hpp"
#include "../Settings.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageAuthenticationCode>
#include <QRandomGenerator>
#include <QCoreApplication>
#include <QPointer>
#include <QSaveFile>
#include <QSysInfo>
#include <QThreadPool>
#include <QTimer>

#include <algorithm>

#include <obs-module.h>

#ifdef HAVE_QTKEYCHAIN
#include <qt6keychain/keychain.h>
#endif

#ifdef Q_OS_WIN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <wincrypt.h>
#endif

// Trousseau du systeme SANS QtKeychain :
//   * macOS : Security.framework, composant systeme toujours present ;
//   * Linux et autres Unix : libsecret, CHARGEE A L'EXECUTION (QLibrary,
//     libsecret-1.so.0). Le .so n'en depend pas : sans elle, ou dans
//     l'edition cloud (conteneur sans D-Bus), le repli fichier 0600 reste le
//     support. Une dependance a l'edition de liens aurait soit empeche le
//     module de charger sur une machine sans libsecret, soit (CI sans
//     libsecret-1-dev) produit un binaire public qui ne s'en servait jamais.
#if !defined(HAVE_QTKEYCHAIN) && defined(Q_OS_MACOS)
#  define SEMIRPROD_OS_SECRET_STORE 1
#  include <CoreFoundation/CoreFoundation.h>
#  include <Security/Security.h>
#elif !defined(HAVE_QTKEYCHAIN) && defined(Q_OS_UNIX)
#  define SEMIRPROD_OS_SECRET_STORE 1
#  define SEMIRPROD_LIBSECRET_RUNTIME 1
#  include <QLibrary>
#endif

namespace semirprod {

namespace {
constexpr auto kAccountName = "primary";

// On-disk format for the encrypted fallback file (when QtKeychain is
// unavailable):
//   { "v": 1, "alg": "dpapi", "ct": "<base64 ciphertext>" }
// Bumping `v` lets us add a new format later and migrate without losing
// existing sessions.
constexpr int kOnDiskFormatVersion = 1;

// Read the local machine's stable identifier. On Windows we use DPAPI
// directly, which is already tied to the user+machine — no extra salt needed.
// On other platforms the fallback is obfuscation-only (clearly documented).
#ifdef Q_OS_WIN
QByteArray dpapiProtect(const QByteArray& plaintext) {
  DATA_BLOB in{};
  in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plaintext.constData()));
  in.cbData = static_cast<DWORD>(plaintext.size());

  DATA_BLOB out{};
  const BOOL ok = CryptProtectData(
      &in, L"semirprod-obs-plugin", nullptr, nullptr, nullptr,
      CRYPTPROTECT_UI_FORBIDDEN, &out);
  if (!ok) return {};
  const QByteArray cipher(reinterpret_cast<const char*>(out.pbData),
                          static_cast<int>(out.cbData));
  LocalFree(out.pbData);
  return cipher;
}

QByteArray dpapiUnprotect(const QByteArray& cipher) {
  if (cipher.isEmpty()) return {};
  DATA_BLOB in{};
  in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(cipher.constData()));
  in.cbData = static_cast<DWORD>(cipher.size());

  DATA_BLOB out{};
  const BOOL ok = CryptUnprotectData(
      &in, nullptr, nullptr, nullptr, nullptr,
      CRYPTPROTECT_UI_FORBIDDEN, &out);
  if (!ok) {
    // Most common reason: the file was written by a different user / a
    // different machine, OR DPAPI master key is unavailable (rare, e.g.
    // fresh user profile). Surface as empty so the caller treats it as
    // "no token", not as a crash.
    return {};
  }
  const QByteArray plain(reinterpret_cast<const char*>(out.pbData),
                         static_cast<int>(out.cbData));
  LocalFree(out.pbData);
  return plain;
}
#endif // Q_OS_WIN

#ifndef Q_OS_WIN
// ---------------------------------------------------------------------------
// Repli fichier hors Windows : derivation de cle et controle d'integrite.
// ---------------------------------------------------------------------------

// Liaison a la machine, melangee au sel de chaque ecriture.
//
// EDITION PUBLIQUE : inchange, QSysInfo::machineUniqueId(). C'est le repli d'un
// poste de bureau sans trousseau, ou lier le fichier a la machine a un sens
// (un tokens.bin recopie ailleurs ne s'ouvre pas) et ou l'identite machine est
// stable.
//
// EDITION CLOUD : VIDE, volontairement (edition::bindsTokensToMachine). Dans un
// conteneur mono-locataire cette liaison ne protege de RIEN — qui peut lire
// tokens.bin est deja dans le conteneur, et y lit /etc/machine-id dans la
// foulee — alors qu'elle porte tout le risque. La valeur est aujourd'hui vide
// parce que /etc/machine-id est absent, ce qui la rend stable par accident : le
// jour ou l'image installe systemd ou dbus, ou ou le runtime injecte un
// machine-id, la cle change et toutes les instances se reveillent non
// authentifiees. Comme les jetons d'appairage sont a usage unique et deja
// consommes, il faudrait toutes les reappairer a la main. On echange donc une
// protection nulle contre un mode de panne de flotte : la liaison saute, le
// controle d'integrite ci-dessous reste.
QByteArray machineBinding() {
  if (!edition::bindsTokensToMachine()) return {};
  return QSysInfo::machineUniqueId();
}

QByteArray fileCipherKey(const QByteArray& salt) {
  return QCryptographicHash::hash(salt + machineBinding(),
                                  QCryptographicHash::Sha256);
}

// Cle de MAC DISTINCTE de la cle de chiffrement : reutiliser la meme cle pour
// les deux usages est la faute classique, et il n'en coute qu'un hachage.
QByteArray fileMac(const QByteArray& salt, const QByteArray& cipher) {
  const QByteArray macKey = QCryptographicHash::hash(
      salt + machineBinding() + QByteArrayLiteral("|semirprod-tokens-mac-v1"),
      QCryptographicHash::Sha256);
  return QMessageAuthenticationCode::hash(cipher, macKey,
                                          QCryptographicHash::Sha256);
}

QByteArray xorWithKey(const QByteArray& data, const QByteArray& key) {
  QByteArray out(data.size(), '\0');
  for (int i = 0; i < data.size(); ++i) {
    out[i] = data[i] ^ key[i % key.size()];
  }
  return out;
}
#endif // !Q_OS_WIN

#ifdef SEMIRPROD_OS_SECRET_STORE
// ---------------------------------------------------------------------------
// Trousseau du systeme, sans QtKeychain.
//
// macOS : meme service et meme compte que la branche QtKeychain
// (Config::keychainServiceName / kAccountName), meme classe d'element :
// QtKeychain range exactement la meme entree, un poste qui passe d'un build a
// l'autre la retrouve telle quelle.
//
// Linux : PAS la meme entree. QtKeychain emploie son propre schema
// (`org.qt.keychain`, attributs user / server / type). Une entree laissee par
// un build QtKeychain est donc relue sous ce schema au chargement, recopiee
// sous le notre, puis supprimee (osLegacyRead / osLegacyDelete). Seul son
// backend libsecret est repris : une entree KWallet ne l'est pas.
// ---------------------------------------------------------------------------
// Locked : le trousseau existe et l'entree aussi, mais elle n'a pas pu etre
// lue (verrouille, invite de deverrouillage refusee) -> on reessaie.
// Absent : pas de service de trousseau joignable (pas de demon Secret
// Service, Flatpak sans org.freedesktop.secrets, session sans D-Bus) -> repli
// fichier, quelques essais seulement.
// Refused : l'utilisateur a refuse l'invite (libsecret : invite fermee ;
// macOS : errSecUserCanceled / errSecAuthFailed). Definitif pour la session :
// aucun essai automatique, qui reafficherait l'invite qu'il vient de fermer.
enum class SecretRead { Found, NotFound, Locked, Refused, Absent };

#ifdef Q_OS_MACOS
CFMutableDictionaryRef keychainBaseQuery() {
  CFMutableDictionaryRef query = CFDictionaryCreateMutable(
      kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks);
  CFStringRef service = CFStringCreateWithCString(
      kCFAllocatorDefault, Config::keychainServiceName, kCFStringEncodingUTF8);
  CFStringRef account =
      CFStringCreateWithCString(kCFAllocatorDefault, kAccountName, kCFStringEncodingUTF8);
  CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
  CFDictionarySetValue(query, kSecAttrService, service);
  CFDictionarySetValue(query, kSecAttrAccount, account);
  CFRelease(service);
  CFRelease(account);
  return query;
}

// `allowPrompt` faux (essais en arriere-plan) : aucune invite, un trousseau
// verrouille rend errSecInteractionNotAllowed au lieu de demander.
SecretRead osSecretRead(QByteArray& out, bool allowPrompt) {
  CFMutableDictionaryRef query = keychainBaseQuery();
  CFDictionarySetValue(query, kSecReturnData, kCFBooleanTrue);
  CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitOne);
  if (!allowPrompt) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    CFDictionarySetValue(query, kSecUseAuthenticationUI, kSecUseAuthenticationUIFail);
#pragma clang diagnostic pop
  }
  CFTypeRef result = nullptr;
  const OSStatus status = SecItemCopyMatching(query, &result);
  CFRelease(query);
  if (status == errSecItemNotFound) return SecretRead::NotFound;
  if (status != errSecSuccess || !result) {
    blog(LOG_WARNING, "[semirprod-tokenstore] trousseau macOS illisible (OSStatus %d)",
         static_cast<int>(status));
    if (result) CFRelease(result);
    // Refus explicite de l'utilisateur : definitif pour la session.
    // Seulement si une invite a pu etre montree : sans invite permise,
    // errSecAuthFailed veut dire « verrouille », pas « refuse ».
    if (allowPrompt && (status == errSecUserCanceled || status == errSecAuthFailed)) {
      return SecretRead::Refused;
    }
    // errSecInteractionNotAllowed (verrouille, pas d'invite possible) et le
    // reste : le trousseau est la, l'entree hors d'atteinte pour l'instant.
    return SecretRead::Locked;
  }
  if (CFGetTypeID(result) == CFDataGetTypeID()) {
    const auto data = static_cast<CFDataRef>(result);
    out = QByteArray(reinterpret_cast<const char*>(CFDataGetBytePtr(data)),
                     static_cast<int>(CFDataGetLength(data)));
  }
  CFRelease(result);
  return out.isEmpty() ? SecretRead::Locked : SecretRead::Found;
}

bool osSecretWrite(const QByteArray& secret) {
  CFDataRef value = CFDataCreate(kCFAllocatorDefault,
                                 reinterpret_cast<const UInt8*>(secret.constData()),
                                 static_cast<CFIndex>(secret.size()));
  if (!value) return false;

  // Mise a jour d'abord : l'entree existe apres la premiere liaison.
  CFMutableDictionaryRef query = keychainBaseQuery();
  CFMutableDictionaryRef changes = CFDictionaryCreateMutable(
      kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks);
  CFDictionarySetValue(changes, kSecValueData, value);
  OSStatus status = SecItemUpdate(query, changes);
  CFRelease(changes);

  if (status == errSecItemNotFound) {
    CFStringRef label = CFStringCreateWithCString(kCFAllocatorDefault, "Semirprod for OBS",
                                                  kCFStringEncodingUTF8);
    CFDictionarySetValue(query, kSecValueData, value);
    CFDictionarySetValue(query, kSecAttrLabel, label);
    CFDictionarySetValue(query, kSecAttrAccessible,
                         kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly);
    status = SecItemAdd(query, nullptr);
    if (status == errSecParam) {
      // Trousseau fichier historique qui refuserait l'attribut d'accessibilite :
      // on reessaie sans plutot que de perdre le jeton.
      CFDictionaryRemoveValue(query, kSecAttrAccessible);
      status = SecItemAdd(query, nullptr);
    }
    CFRelease(label);
  }
  CFRelease(query);
  CFRelease(value);

  if (status != errSecSuccess) {
    blog(LOG_WARNING, "[semirprod-tokenstore] ecriture dans le trousseau macOS en echec (OSStatus %d)",
         static_cast<int>(status));
    return false;
  }
  return true;
}

// `allowPrompt` faux : suppression sans invite (essai differe en arriere-plan).
bool osSecretDelete(bool allowPrompt = true) {
  CFMutableDictionaryRef query = keychainBaseQuery();
  if (!allowPrompt) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    CFDictionarySetValue(query, kSecUseAuthenticationUI, kSecUseAuthenticationUIFail);
#pragma clang diagnostic pop
  }
  const OSStatus status = SecItemDelete(query);
  CFRelease(query);
  return status == errSecSuccess || status == errSecItemNotFound;
}

// macOS : QtKeychain range la MEME entree que nous, rien a migrer.
bool secretStoreUsable() { return true; }
SecretRead osLegacyRead(QByteArray&, bool) { return SecretRead::NotFound; }
void osLegacyDelete(bool = true) {}
#else  // libsecret, chargee a l'execution
// Structures de libsecret/GLib recopiees a l'identique de leur ABI C publique,
// stable depuis libsecret 0.18 : aucun en-tete GLib a la compilation.
struct GErrorAbi {
  quint32 domain;  // GQuark
  int code;
  char* message;
};
struct SecretSchemaAttributeAbi {
  const char* name;
  int type;  // SecretSchemaAttributeType : 0 = chaine
};
struct SecretSchemaAbi {
  const char* name;
  int flags;  // SecretSchemaFlags : 0 = aucun, 2 = DONT_MATCH_NAME
  SecretSchemaAttributeAbi attributes[32];
  int reserved;
  void* reserved1;
  void* reserved2;
  void* reserved3;
  void* reserved4;
  void* reserved5;
  void* reserved6;
  void* reserved7;
};

struct LibSecretApi {
  using LookupFn = char* (*)(const SecretSchemaAbi*, void*, GErrorAbi**, ...);
  using StoreFn = int (*)(const SecretSchemaAbi*, const char*, const char*, const char*, void*,
                          GErrorAbi**, ...);
  using ClearFn = int (*)(const SecretSchemaAbi*, void*, GErrorAbi**, ...);
  // secret_password_search_sync (libsecret >= 0.19) : GList* d'elements.
  using SearchFn = void* (*)(const SecretSchemaAbi*, int, void*, GErrorAbi**, ...);
  using FreeFn = void (*)(char*);
  using ErrorFreeFn = void (*)(GErrorAbi*);
  using DestroyFn = void (*)(void*);
  using ListFreeFullFn = void (*)(void*, DestroyFn);
  LookupFn lookup = nullptr;
  StoreFn store = nullptr;
  ClearFn clear = nullptr;
  SearchFn search = nullptr;  // optionnelle
  FreeFn freePassword = nullptr;
  ErrorFreeFn errorFree = nullptr;
  ListFreeFullFn listFreeFull = nullptr;  // avec search seulement
  DestroyFn objectUnref = nullptr;        // avec search seulement
  using ItemLockedFn = int (*)(void*);
  ItemLockedFn itemGetLocked = nullptr;   // secret_item_get_locked, idem
  // Type GObject de l'element : la recherche du backend FICHIER (Flatpak) rend
  // des SecretFileItem, sur lesquels secret_item_get_locked echoue (g_critical)
  // et rend VRAI. On verifie donc le type avant de demander.
  using GetTypeFn = size_t (*)();
  using InstanceIsAFn = int (*)(void*, size_t);
  GetTypeFn itemGetType = nullptr;          // secret_item_get_type
  InstanceIsAFn instanceIsA = nullptr;      // g_type_check_instance_is_a
  bool usable() const { return lookup && store && clear && freePassword && errorFree; }
  bool canSearch() const { return search && listFreeFull && objectUnref; }
};

const LibSecretApi& libSecret() {
  static const LibSecretApi api = [] {
    LibSecretApi a;
    // Edition cloud : pas de session D-Bus dans le conteneur, le fichier est
    // le bon support (voir CMakeLists.txt, QtKeychain ecarte pour la meme raison).
    if (edition::isCloud()) return a;
    // QLibrary("secret-1", 0) -> libsecret-1.so.0. Jamais dechargee.
    static QLibrary lib(QStringLiteral("secret-1"), 0);
    if (!lib.load()) {
      blog(LOG_WARNING,
           "[semirprod-tokenstore] libsecret introuvable : jetons dans tokens.bin (0600)");
      return a;
    }
    a.lookup = reinterpret_cast<LibSecretApi::LookupFn>(lib.resolve("secret_password_lookup_sync"));
    a.store = reinterpret_cast<LibSecretApi::StoreFn>(lib.resolve("secret_password_store_sync"));
    a.clear = reinterpret_cast<LibSecretApi::ClearFn>(lib.resolve("secret_password_clear_sync"));
    a.freePassword = reinterpret_cast<LibSecretApi::FreeFn>(lib.resolve("secret_password_free"));
    // g_error_free vit dans GLib, dependance de libsecret : dlsym sur ce
    // handle la trouve dans son arbre de dependances.
    a.errorFree = reinterpret_cast<LibSecretApi::ErrorFreeFn>(lib.resolve("g_error_free"));
    // Recherche SANS deverrouillage : distingue « entree absente » de « entree
    // verrouillee » (voir lookupClassified). Absente des libsecret < 0.19 : on
    // fait alors sans, comme avant.
    a.search = reinterpret_cast<LibSecretApi::SearchFn>(lib.resolve("secret_password_search_sync"));
    a.listFreeFull = reinterpret_cast<LibSecretApi::ListFreeFullFn>(lib.resolve("g_list_free_full"));
    a.objectUnref = reinterpret_cast<LibSecretApi::DestroyFn>(lib.resolve("g_object_unref"));
    a.itemGetLocked = reinterpret_cast<LibSecretApi::ItemLockedFn>(lib.resolve("secret_item_get_locked"));
    a.itemGetType = reinterpret_cast<LibSecretApi::GetTypeFn>(lib.resolve("secret_item_get_type"));
    a.instanceIsA =
        reinterpret_cast<LibSecretApi::InstanceIsAFn>(lib.resolve("g_type_check_instance_is_a"));
    if (!a.usable()) {
      blog(LOG_WARNING, "[semirprod-tokenstore] libsecret incomplete : jetons dans tokens.bin");
      a = LibSecretApi{};
    }
    return a;
  }();
  return api;
}

bool secretStoreUsable() { return libSecret().usable(); }

// Notre schema : un seul attribut, `account`.
const SecretSchemaAbi* tokenSchema() {
  static const SecretSchemaAbi* schema = [] {
    static SecretSchemaAbi s{};
    s.name = Config::keychainServiceName;
    s.flags = 0;
    s.attributes[0].name = "account";
    s.attributes[0].type = 0;
    return &s;
  }();
  return schema;
}

// Schema de QtKeychain (backend libsecret), pour la migration seulement.
const SecretSchemaAbi* qtKeychainSchema() {
  static const SecretSchemaAbi* schema = [] {
    static SecretSchemaAbi s{};
    s.name = "org.qt.keychain";
    s.flags = 2;  // SECRET_SCHEMA_DONT_MATCH_NAME
    s.attributes[0].name = "user";
    s.attributes[1].name = "server";
    s.attributes[2].name = "type";
    return &s;
  }();
  return schema;
}

// Rend vrai et vide `error` s'il y en avait une (message journalise).
bool consumeError(GErrorAbi*& error, const char* what) {
  if (!error) return false;
  blog(LOG_WARNING, "[semirprod-tokenstore] Secret Service : %s (%s)", what,
       error->message ? error->message : "?");
  libSecret().errorFree(error);
  error = nullptr;
  return true;
}

/**
 * Lecture classee. `lookup` et `search` font la meme requete (memes
 * attributs) ; `search` ne deverrouille rien et n'affiche aucune invite.
 *
 * Une invite de deverrouillage REFUSEE fait rendre NULL a la lecture, sans
 * erreur : lu tel quel, c'etait « jamais appaire », plus aucun essai, et la
 * seconde lecture (entree QtKeychain) reaffichait une invite. Donc : lecture
 * vide ou en erreur alors qu'une entree EXISTE -> Locked ; erreur et aucune
 * entree visible (service injoignable) -> Absent ; rien -> NotFound.
 */
// Maillon de GList (GLib) : seuls `data` et `next` sont lus.
struct GListAbi {
  void* data;
  GListAbi* next;
  GListAbi* prev;
};

// Etat des entrees correspondantes, SANS deverrouiller ni afficher d'invite :
// 0 = aucune, 1 = au moins une verrouillee, 2 = toutes deverrouillees,
// -1 = recherche impossible (service injoignable, libsecret < 0.19).
template <typename Search>
int itemsState(Search search) {
  const LibSecretApi& api = libSecret();
  if (!api.canSearch()) return -1;
  GErrorAbi* error = nullptr;
  auto* list = static_cast<GListAbi*>(search(&error));
  if (error) {
    api.errorFree(error);
    return -1;
  }
  if (!list) return 0;
  bool locked = false;
  for (GListAbi* it = list; it; it = it->next) {
    if (!it->data) continue;
    // Fonctions absentes : on ne sait pas, donc verrouille (pas de lecture,
    // donc pas d'invite). Element qui n'est pas un SecretItem (backend
    // fichier) : pas de notion de verrou, deverrouille.
    if (!api.itemGetLocked || !api.itemGetType || !api.instanceIsA) {
      locked = true;
      continue;
    }
    if (api.instanceIsA(it->data, api.itemGetType()) == 0) continue;
    if (api.itemGetLocked(it->data) != 0) locked = true;
  }
  api.listFreeFull(list, api.objectUnref);
  return locked ? 1 : 2;
}

/**
 * Lecture classee. `lookup` et `search` font la meme requete (memes
 * attributs) ; `search` ne deverrouille rien et n'affiche aucune invite.
 *
 * `allowPrompt` vrai (premier chargement) : la lecture peut demander le
 * deverrouillage. Une invite FERMEE fait rendre NULL sans erreur alors que
 * l'entree existe : c'est un refus, definitif pour la session (Refused). Une
 * erreur alors que l'entree existe : verrouille (Locked) ; erreur sans entree
 * visible : service injoignable (Absent).
 *
 * `allowPrompt` faux (essais en arriere-plan) : la recherche d'abord ; tant
 * qu'une entree est verrouillee on ne lit PAS (la lecture afficherait
 * l'invite), on rend Locked. La lecture n'a lieu qu'une fois tout deverrouille.
 */
template <typename Lookup, typename Search>
SecretRead lookupClassified(QByteArray& out, Lookup lookup, Search search, bool allowPrompt) {
  const LibSecretApi& api = libSecret();
  if (!api.usable()) return SecretRead::Absent;

  if (!allowPrompt) {
    const int etat = itemsState(search);
    if (etat == -1) return api.canSearch() ? SecretRead::Absent : SecretRead::Locked;
    if (etat == 0) return SecretRead::NotFound;
    if (etat == 1) return SecretRead::Locked;
  }

  GErrorAbi* error = nullptr;
  char* secret = lookup(&error);
  if (consumeError(error, "lecture impossible")) {
    if (secret) api.freePassword(secret);
    return itemsState(search) > 0 ? SecretRead::Locked : SecretRead::Absent;
  }
  if (!secret) {
    if (itemsState(search) <= 0) return SecretRead::NotFound;
    return allowPrompt ? SecretRead::Refused : SecretRead::Locked;
  }
  out = QByteArray(secret);
  api.freePassword(secret);
  return out.isEmpty() ? SecretRead::Locked : SecretRead::Found;
}

constexpr int kSearchAll = 1 << 1;  // SECRET_SEARCH_ALL, sans UNLOCK ni LOAD_SECRETS

SecretRead osSecretRead(QByteArray& out, bool allowPrompt) {
  const LibSecretApi& api = libSecret();
  return lookupClassified(
      out,
      [&](GErrorAbi** error) {
        return api.lookup(tokenSchema(), nullptr, error, "account", kAccountName,
                          static_cast<char*>(nullptr));
      },
      [&](GErrorAbi** error) {
        return api.search(tokenSchema(), kSearchAll, nullptr, error, "account", kAccountName,
                          static_cast<char*>(nullptr));
      },
      allowPrompt);
}

bool osSecretWrite(const QByteArray& secret) {
  const LibSecretApi& api = libSecret();
  if (!api.usable()) return false;
  GErrorAbi* error = nullptr;
  const int ok = api.store(tokenSchema(), nullptr /* collection par defaut */, "Semirprod for OBS",
                           secret.constData(), nullptr, &error, "account", kAccountName,
                           static_cast<char*>(nullptr));
  if (consumeError(error, "ecriture impossible")) return false;
  return ok != 0;
}

bool osSecretDelete(bool allowPrompt = true) {
  const LibSecretApi& api = libSecret();
  if (!api.usable()) return false;
  // Sans invite permise : une entree verrouillee n'est pas touchee (la
  // supprimer demanderait le deverrouillage).
  if (!allowPrompt && itemsState([&](GErrorAbi** error) {
        return api.search(tokenSchema(), kSearchAll, nullptr, error, "account", kAccountName,
                          static_cast<char*>(nullptr));
      }) == 1) {
    return false;
  }
  GErrorAbi* error = nullptr;
  api.clear(tokenSchema(), nullptr, &error, "account", kAccountName, static_cast<char*>(nullptr));
  return !consumeError(error, "suppression impossible");
}

// Entree laissee par un build QtKeychain (user = compte, server = service).
// QtKeychain range le texte tel quel (type « plaintext »), ou en base64 pour
// des donnees binaires : les deux formes sont acceptees.
SecretRead osLegacyRead(QByteArray& out, bool allowPrompt) {
  const LibSecretApi& api = libSecret();
  QByteArray raw;
  const SecretRead lu = lookupClassified(
      raw,
      [&](GErrorAbi** error) {
        return api.lookup(qtKeychainSchema(), nullptr, error, "user", kAccountName, "server",
                          Config::keychainServiceName, static_cast<char*>(nullptr));
      },
      [&](GErrorAbi** error) {
        return api.search(qtKeychainSchema(), kSearchAll, nullptr, error, "user", kAccountName,
                          "server", Config::keychainServiceName, static_cast<char*>(nullptr));
      },
      allowPrompt);
  if (lu != SecretRead::Found) return lu;
  if (!QJsonDocument::fromJson(raw).isObject()) raw = QByteArray::fromBase64(raw);
  out = raw;
  return out.isEmpty() ? SecretRead::NotFound : SecretRead::Found;
}

void osLegacyDelete(bool allowPrompt = true) {
  const LibSecretApi& api = libSecret();
  if (!api.usable()) return;
  if (!allowPrompt && itemsState([&](GErrorAbi** error) {
        return api.search(qtKeychainSchema(), kSearchAll, nullptr, error, "user", kAccountName,
                          "server", Config::keychainServiceName, static_cast<char*>(nullptr));
      }) == 1) {
    return;
  }
  GErrorAbi* error = nullptr;
  api.clear(qtKeychainSchema(), nullptr, &error, "user", kAccountName, "server",
            Config::keychainServiceName, static_cast<char*>(nullptr));
  consumeError(error, "suppression de l'entree QtKeychain impossible");
}
#endif // Q_OS_MACOS
#endif // SEMIRPROD_OS_SECRET_STORE
} // namespace

// Format v2 : une liaison 0.1.x (sans appareil) n'est PAS reprise. Le poste
// repasse par l'appairage, qui crée un appareil côté serveur : c'est la rupture
// voulue (plan poste v2), pas une perte de données.
constexpr int kBindingFormatVersion = 2;

QJsonObject TokenBundle::toJson() const {
  QJsonObject obj;
  obj[QStringLiteral("v")] = kBindingFormatVersion;
  obj[QStringLiteral("access_token")] = accessToken;
  obj[QStringLiteral("refresh_token")] = refreshToken;
  obj[QStringLiteral("expires_at")] = expiresAt.toString(Qt::ISODate);
  obj[QStringLiteral("oauth_access_token")] = oauthAccessToken;
  obj[QStringLiteral("oauth_expires_at")] = oauthExpiresAt.toString(Qt::ISODate);
  QJsonObject device;
  device[QStringLiteral("id")] = deviceId;
  device[QStringLiteral("secret")] = deviceSecret;
  obj[QStringLiteral("device")] = device;
  QJsonObject org;
  org[QStringLiteral("id")] = orgId;
  org[QStringLiteral("name")] = orgName;
  org[QStringLiteral("slug")] = orgSlug;
  obj[QStringLiteral("organization")] = org;
  return obj;
}

TokenBundle TokenBundle::fromJson(const QJsonObject& obj) {
  if (obj.value(QStringLiteral("v")).toInt(0) != kBindingFormatVersion) {
    blog(LOG_INFO,
         "[semirprod-tokenstore] liaison d'un plugin anterieur a 0.2.0 : "
         "ignoree, le poste doit etre reappaire");
    return {};
  }
  TokenBundle b;
  b.accessToken = obj.value(QStringLiteral("access_token")).toString();
  b.refreshToken = obj.value(QStringLiteral("refresh_token")).toString();
  b.expiresAt = QDateTime::fromString(obj.value(QStringLiteral("expires_at")).toString(), Qt::ISODate);
  b.oauthAccessToken = obj.value(QStringLiteral("oauth_access_token")).toString();
  b.oauthExpiresAt =
      QDateTime::fromString(obj.value(QStringLiteral("oauth_expires_at")).toString(), Qt::ISODate);
  const QJsonObject device = obj.value(QStringLiteral("device")).toObject();
  b.deviceId = device.value(QStringLiteral("id")).toString();
  b.deviceSecret = device.value(QStringLiteral("secret")).toString();
  const QJsonObject org = obj.value(QStringLiteral("organization")).toObject();
  b.orgId = org.value(QStringLiteral("id")).toString();
  b.orgName = org.value(QStringLiteral("name")).toString();
  b.orgSlug = org.value(QStringLiteral("slug")).toString();
  if (!b.isBound()) return {};
  return b;
}

// ============================================================================
// Fichier de repli (tokens.bin) — JAMAIS en clair.
//
// Windows : DPAPI (CryptProtectData / CryptUnprotectData). La cle est liee au
//   compte de l'utilisateur sur cette machine ; un autre compte ou une autre
//   machine ne peut pas dechiffrer. C'est l'OS qui l'impose : aucune cle a
//   extraire du binaire.
//
// Ailleurs : brouillage XOR + MAC, qui n'est PAS une frontiere de securite
//   (tout processus local de l'utilisateur sait refaire la derivation). La
//   protection reelle, ce sont les permissions 0600 posees AVANT le premier
//   octet ecrit. Ce support ne sert plus que la ou aucun trousseau n'existe :
//   edition cloud (conteneur sans D-Bus), Linux sans libsecret ni QtKeychain,
//   ou trousseau momentanement indisponible. macOS et Linux avec Secret
//   Service passent par le trousseau du systeme (voir plus bas), et y migrent
//   un tokens.bin existant au premier chargement.
// ============================================================================

namespace {

// Contenu de tokens.bin -> liaison. Liaison vide (avec une ligne de journal) si
// le fichier est illisible, d'un autre format ou d'une autre machine.
TokenBundle decodeTokenFile(const QByteArray& raw) {
  QByteArray plain;
#ifdef Q_OS_WIN
  {
    // Format: {"v":1,"alg":"dpapi","ct":"<base64 DPAPI blob>"}
    const auto doc = QJsonDocument::fromJson(raw);
    if (!doc.isObject() ||
        doc.object().value(QStringLiteral("v")).toInt(0) != kOnDiskFormatVersion ||
        doc.object().value(QStringLiteral("alg")).toString() != QStringLiteral("dpapi")) {
      blog(LOG_WARNING,
           "[semirprod-tokenstore] tokens.bin has unknown format; ignoring "
           "(forcing re-login)");
      return {};
    }
    const QString ctB64 = doc.object().value(QStringLiteral("ct")).toString();
    const QByteArray cipher = QByteArray::fromBase64(ctB64.toLatin1());
    plain = dpapiUnprotect(cipher);
  }
#else
  {
    // Format: {"v":1,"alg":"xor","ct":"<b64>","salt":"<b64>","mac":"<b64>"}
    const auto doc = QJsonDocument::fromJson(raw);
    if (!doc.isObject() ||
        doc.object().value(QStringLiteral("v")).toInt(0) != kOnDiskFormatVersion) {
      blog(LOG_WARNING,
           "[semirprod-tokenstore] tokens.bin illisible ou de format inconnu : "
           "ignore (nouvelle authentification necessaire)");
      return {};
    }
    const QString alg = doc.object().value(QStringLiteral("alg")).toString();
    const QByteArray cipher = QByteArray::fromBase64(
        doc.object().value(QStringLiteral("ct")).toString().toLatin1());
    const QByteArray salt = QByteArray::fromBase64(
        doc.object().value(QStringLiteral("salt")).toString().toLatin1());
    const QByteArray mac = QByteArray::fromBase64(
        doc.object().value(QStringLiteral("mac")).toString().toLatin1());

    // CONTROLE D'INTEGRITE, avant tout dechiffrement.
    //
    // Dechiffrer avec la mauvaise cle ne rate pas : ca rend des octets
    // quelconques, l'analyse JSON echoue plus bas, et la fonction rendait un
    // jeton vide SANS UN SEUL MESSAGE. Symptome quand on l'enleve : apres une
    // reconstruction d'image qui change l'identite machine, toutes les
    // instances se reveillent non authentifiees en silence, et rien dans le
    // journal ne relie la panne au magasin de jetons.
    if (mac.isEmpty()) {
      // Cloud : refuse (edition::acceptsTokenFileWithoutMac).
      if (!edition::acceptsTokenFileWithoutMac()) {
        blog(LOG_ERROR,
             "[semirprod-tokenstore] tokens.bin sans controle d'integrite "
             "(ecrit par une version anterieure ?) : refuse, l'instance devra "
             "etre reappairee avec un jeton neuf");
        return {};
      }
      // Edition publique : un fichier ecrit avant l'ajout du MAC reste
      // lisible, le refuser deconnecterait des postes clients qui
      // fonctionnent.
      blog(LOG_INFO,
           "[semirprod-tokenstore] tokens.bin sans MAC (format anterieur) : "
           "accepte, il sera reecrit avec controle d'integrite");
    } else if (mac != fileMac(salt, cipher)) {
      blog(LOG_ERROR,
           "[semirprod-tokenstore] tokens.bin : controle d'integrite en echec "
           "(cle de machine differente, fichier corrompu ou modifie). Jetons "
           "ignores — une nouvelle authentification est necessaire.");
      return {};
    }

    if (alg != QStringLiteral("xor")) {
      blog(LOG_WARNING,
           "[semirprod-tokenstore] tokens.bin : algorithme inattendu, ignore");
      return {};
    }

    plain = xorWithKey(cipher, fileCipherKey(salt));
  }
#endif

  if (plain.isEmpty()) {
    blog(LOG_WARNING,
         "[semirprod-tokenstore] tokens.bin : dechiffrement vide, jetons "
         "ignores (nouvelle authentification necessaire)");
    return {};
  }

  const auto doc = QJsonDocument::fromJson(plain);
  if (!doc.isObject()) {
    // Ne devrait plus arriver hors corruption : le MAC attrape la mauvaise
    // cle en amont. On le dit quand meme, l'echec muet ici etait le defaut
    // d'origine.
    blog(LOG_WARNING,
         "[semirprod-tokenstore] tokens.bin : contenu dechiffre non "
         "exploitable, jetons ignores (nouvelle authentification necessaire)");
    return {};
  }
  return TokenBundle::fromJson(doc.object());
}

// Liaison -> contenu de tokens.bin. Vide si le chiffrement a echoue.
QByteArray encodeTokenFile(const TokenBundle& bundle) {
  // Inner plaintext is still the same JSON shape — keeps load symmetric
  // and means the cipher format is the only thing that varies by OS.
  const QByteArray plain = QJsonDocument(bundle.toJson()).toJson(QJsonDocument::Compact);

  QJsonObject envelope;
  envelope[QStringLiteral("v")] = kOnDiskFormatVersion;

#ifdef Q_OS_WIN
  const QByteArray cipher = dpapiProtect(plain);
  if (cipher.isEmpty()) return {};
  envelope[QStringLiteral("alg")] = QStringLiteral("dpapi");
  envelope[QStringLiteral("ct")] = QString::fromLatin1(cipher.toBase64());
#else
  // Per-write random salt — even though the obfuscation isn't security,
  // we still don't want two writes of the same token to produce the same
  // ciphertext (otherwise an attacker could diff two captures).
  QByteArray salt(16, '\0');
  QRandomGenerator::system()->fillRange(
      reinterpret_cast<quint32*>(salt.data()),
      salt.size() / static_cast<int>(sizeof(quint32)));
  const QByteArray cipher = xorWithKey(plain, fileCipherKey(salt));
  envelope[QStringLiteral("alg")] = QStringLiteral("xor");
  envelope[QStringLiteral("ct")] = QString::fromLatin1(cipher.toBase64());
  envelope[QStringLiteral("salt")] = QString::fromLatin1(salt.toBase64());
  // MAC sur le CHIFFRE (encrypt-then-MAC) : verifiable sans rien dechiffrer,
  // donc une cle qui ne correspond pas se dit au lieu de rendre un jeton vide.
  envelope[QStringLiteral("mac")] = QString::fromLatin1(fileMac(salt, cipher).toBase64());
#endif
  return QJsonDocument(envelope).toJson(QJsonDocument::Compact);
}

TokenBundle readTokenFile(const QString& path) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) return {};
  const QByteArray raw = f.readAll();
  f.close();
  return decodeTokenFile(raw);
}

bool writeTokenFile(const QString& path, const TokenBundle& bundle) {
  const QByteArray bytes = encodeTokenFile(bundle);
  if (bytes.isEmpty()) return false;
#ifdef Q_OS_WIN
  // DPAPI : le contenu est deja lie au compte Windows, les permissions n'y
  // ajoutent rien. Ecriture inchangee.
  QFile f(path);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
  const bool written = f.write(bytes) == bytes.size();
  f.close();
  return written;
#else
  // Fichier temporaire + renommage atomique, et permissions 0600 posees AVANT
  // le premier octet : avant, le fichier etait cree avec l'umask (0644 en
  // general), rempli, puis seulement restreint — un autre compte du poste
  // pouvait le lire entre les deux, et un ancien tokens.bin en 0644 le restait
  // jusqu'a la fin de l'ecriture.
  QSaveFile f(path);
  if (!f.open(QIODevice::WriteOnly)) return false;
  if (!f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
    // Systeme de fichiers sans permissions POSIX (FAT, partage reseau) : elles
    // n'y protegeraient rien de toute facon. On le dit, on n'empeche pas.
    blog(LOG_WARNING,
         "[semirprod-tokenstore] permissions 0600 impossibles a poser sur tokens.bin");
  }
  if (f.write(bytes) != bytes.size()) {
    f.cancelWriting();
    return false;
  }
  return f.commit();
#endif
}

#if defined(HAVE_QTKEYCHAIN) || defined(SEMIRPROD_OS_SECRET_STORE)
QString serialize(const TokenBundle& b) {
  const QJsonObject obj = b.toJson();
  return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

TokenBundle deserialize(const QString& blob) {
  const auto doc = QJsonDocument::fromJson(blob.toUtf8());
  if (!doc.isObject()) return {};
  const auto obj = doc.object();
  return TokenBundle::fromJson(obj);
}
#endif

} // namespace

TokenStore::TokenStore(QObject* parent) : QObject(parent) {}

TokenStore::~TokenStore() {
  // A la fermeture, AUCUNE tache pas encore commencee ne part sur le fil du
  // trousseau : elle pourrait tourner apres le dechargement du module (code
  // decharge = plantage a la sortie). Les taches en file sont abandonnees
  // (clear), celle en cours a 2 s pour finir. Au-dela, le pool est laisse tel
  // quel plutot que d'attendre : fermer OBS ne doit jamais dependre d'une
  // fenetre de trousseau restee ouverte.
  //
  // Les ECRITURES qui comptent ne sont pas perdues pour autant : la derniere
  // demandee, si le fil ne l'a pas finie, est rejouee ICI sur le fil Qt par le
  // seul moyen qui n'affiche aucune invite.
  //   - enregistrement (jeton qui vient de tourner) : tokens.bin (0600), relu
  //     en priorite au lancement suivant puis remigre dans le trousseau ;
  //   - deconnexion : le marqueur est deja pose (clearSecretStore l'ecrit sur
  //     le fil Qt), tokens.bin est retire ; le lancement suivant finit la
  //     suppression (readSecretStore, etape 1).
  closing_->store(true);
  if (!secretPool_) return;
  secretPool_->clear();
  const bool fini = secretPool_->waitForDone(2000);
#ifdef SEMIRPROD_OS_SECRET_STORE
  if (writeSeq_ > writesDone_->load()) {
    if (lastWriteIsSave_) {
      if (writeTokenFile(fallbackFilePath(), lastSavedBundle_)) {
        QFile::remove(loggedOutMarkerPath());
        blog(LOG_INFO, "[semirprod-tokenstore] fermeture : jetons ecrits dans tokens.bin, "
                       "remigres au prochain lancement");
      }
    } else {
      QFile::remove(fallbackFilePath());
    }
  }
#endif
  if (fini) delete secretPool_;
  secretPool_ = nullptr;
}

void TokenStore::cancelPendingLoad() {
  ++loadGeneration_;
  if (retryTimer_) retryTimer_->stop();
  retryCallback_ = nullptr;
  refused_ = false;
  if (unavailable_) {
    unavailable_ = false;
    emit storeStateChanged();
  }
}

void TokenStore::load(LoadCallback cb) {
#ifdef HAVE_QTKEYCHAIN
  loadFromKeychain(std::move(cb));
#elif defined(SEMIRPROD_OS_SECRET_STORE)
  if (secretStoreUsable()) {
    loadFromSecretStore(std::move(cb));
  } else {
    loadFromFile(std::move(cb));
  }
#else
  loadFromFile(std::move(cb));
#endif
}

void TokenStore::save(const TokenBundle& bundle, SimpleCallback cb) {
  // Update cached_ synchronously so callers (LiveClient, refresh chain)
  // immediately see the new token — the running session must not be left
  // holding the rotated-out refresh_token. Persistence is fire-and-log: if
  // the keychain write fails (locked, AV interfering, disk full), the live
  // session continues until OBS restarts. Restart-after-failure is the
  // unfixable case — refresh token rotation means the previous bundle on
  // disk is already dead by the time we got here, so there's no useful
  // fallback content to write either.
  cached_ = bundle;
  emit tokenChanged();

  auto wrappedCb = [bundle, cb = std::move(cb)](bool ok) {
    if (!ok) {
      blog(LOG_WARNING,
           "[semirprod-tokenstore] persist failed — session ok in memory but "
           "next OBS restart will require re-login (refresh token already "
           "rotated by server)");
    }
    if (cb) cb(ok);
  };

#ifdef HAVE_QTKEYCHAIN
  saveToKeychain(bundle, std::move(wrappedCb));
#elif defined(SEMIRPROD_OS_SECRET_STORE)
  if (secretStoreUsable()) {
    saveToSecretStore(bundle, std::move(wrappedCb));
  } else {
    saveToFile(bundle, std::move(wrappedCb));
  }
#else
  saveToFile(bundle, std::move(wrappedCb));
#endif
}

void TokenStore::clear(SimpleCallback cb) {
  cached_ = {};
#ifdef HAVE_QTKEYCHAIN
  clearKeychain(std::move(cb));
#elif defined(SEMIRPROD_OS_SECRET_STORE)
  if (secretStoreUsable()) {
    clearSecretStore(std::move(cb));
  } else {
    clearFile(std::move(cb));
  }
#else
  clearFile(std::move(cb));
#endif
  emit tokenChanged();
}

#ifdef HAVE_QTKEYCHAIN

void TokenStore::loadFromKeychain(LoadCallback cb) {
  auto* job = new QKeychain::ReadPasswordJob(Config::keychainServiceName, this);
  job->setKey(QString::fromLatin1(kAccountName));
  connect(job, &QKeychain::Job::finished, this, [this, cb = std::move(cb)](QKeychain::Job* j) {
    auto* read = qobject_cast<QKeychain::ReadPasswordJob*>(j);
    if (!read || read->error() != QKeychain::NoError) {
      cached_ = {};
      if (cb) cb(cached_);
      return;
    }
    cached_ = deserialize(read->textData());
    if (cb) cb(cached_);
  });
  job->start();
}

void TokenStore::saveToKeychain(const TokenBundle& bundle, SimpleCallback cb) {
  auto* job = new QKeychain::WritePasswordJob(Config::keychainServiceName, this);
  job->setKey(QString::fromLatin1(kAccountName));
  job->setTextData(serialize(bundle));
  connect(job, &QKeychain::Job::finished, this, [cb = std::move(cb)](QKeychain::Job* j) {
    if (cb) cb(j->error() == QKeychain::NoError);
  });
  job->start();
}

void TokenStore::clearKeychain(SimpleCallback cb) {
  auto* job = new QKeychain::DeletePasswordJob(Config::keychainServiceName, this);
  job->setKey(QString::fromLatin1(kAccountName));
  connect(job, &QKeychain::Job::finished, this, [cb = std::move(cb)](QKeychain::Job* j) {
    if (cb) cb(j->error() == QKeychain::NoError);
  });
  job->start();
}

#endif // HAVE_QTKEYCHAIN

#ifdef SEMIRPROD_OS_SECRET_STORE
// ============================================================================
// Trousseau du systeme (macOS : Security.framework ; Linux : libsecret).
//
// MIGRATION, obligatoire : un poste mis a jour a ses jetons dans tokens.bin.
// Regle unique : quand tokens.bin existe et se lit, il fait foi. Il n'existe
// en effet que dans deux cas, et dans les deux il est au moins aussi recent
// que le trousseau :
//   * premiere ouverture apres la mise a jour (le trousseau est vide) ;
//   * une ecriture dans le trousseau a echoue et la liaison est partie dans le
//     fichier (trousseau verrouille, Secret Service absent).
// On le recopie alors dans le trousseau, puis on le supprime. Si la recopie
// echoue, le fichier reste : le poste n'est jamais deconnecte par la mise a
// jour. Meme traitement pour une entree laissee par un build QtKeychain sous
// Linux (osLegacyRead), dont le schema differe du notre.
//
// INDISPONIBLE N'EST PAS VIDE. Un trousseau verrouille (errSecInteraction-
// NotAllowed, trousseau de session pas encore deverrouille au demarrage
// automatique, invite de deverrouillage refusee) se lisait comme « jamais
// appaire » : le poste paraissait deconnecte et un nouvel appairage creait un
// second appareil. Desormais :
//   * verrouille (Locked) : relecture en arriere-plan, 15 s puis jusqu'a
//     5 min, au plus kMaxLockedRetries fois ;
//   * service absent (Absent : pas de demon, Flatpak sans acces) : repli
//     fichier, et seulement kMaxAbsentRetries essais (le demon peut demarrer
//     apres OBS a l'ouverture de session), puis on arrete.
// La liaison est rendue au callback de load() des qu'elle se lit ; un essai
// qui se termine sans liaison emet storeStateChanged().
//
// DECONNEXION : si l'entree ne peut pas etre supprimee, un marqueur
// (tokens.bin.loggedout) est pose. Tant qu'il existe, le chargement ne relit
// PAS le trousseau (pas de reconnexion silencieuse au lancement suivant) et
// retente la suppression. Toute nouvelle liaison enregistree le retire.
//
// JAMAIS SUR LE FIL Qt. Une lecture ou une ecriture peut afficher une invite de
// deverrouillage et attendre l'utilisateur : sur le fil principal, OBS gelait,
// y compris en direct au renouvellement du jeton. Tous les appels au trousseau
// passent par un fil de travail unique (QThreadPool a un fil : l'ordre des
// operations est garanti), et le resultat revient sur le fil Qt.
// ============================================================================

namespace {
constexpr int kMaxLockedRetries = 30;  // ~2 h a 5 min d'intervalle
constexpr int kMaxAbsentRetries = 3;
}  // namespace

QString TokenStore::loggedOutMarkerPath() {
  return fallbackFilePath() + QStringLiteral(".loggedout");
}

void TokenStore::runOnSecretThread(std::function<void()> job) {
  if (!secretPool_) {
    // PAS enfant du magasin : le destructeur d'un QThreadPool enfant attend
    // ses taches, et une invite de trousseau en attente bloquait alors la
    // fermeture d'OBS. Voir ~TokenStore.
    secretPool_ = new QThreadPool();
    secretPool_->setMaxThreadCount(1);
    // Le fil reste vivant entre deux operations rares : pas de recreation.
    secretPool_->setExpiryTimeout(-1);
  }
  secretPool_->start(std::move(job));
}

// Execute `fn` sur le fil Qt de `store`, s'il existe encore.
template <typename Fn>
void postToStore(const QPointer<TokenStore>& store, Fn fn) {
  QCoreApplication* app = QCoreApplication::instance();
  if (!app) return;  // OBS se ferme : plus personne a prevenir
  QMetaObject::invokeMethod(
      app,
      [store, fn = std::move(fn)]() mutable {
        if (store) fn(store.data());
      },
      Qt::QueuedConnection);
}

// Sur le fil de travail. Rend 0 = lu (liaison ou vide), 1 = verrouille,
// 2 = service absent, 3 = refus de l'utilisateur. `allowPrompt` faux : aucune
// operation qui puisse afficher une invite (essais en arriere-plan).
int TokenStore::readSecretStore(TokenBundle* out, bool allowPrompt) {
  *out = {};

  // 1. Deconnexion demandee mais pas encore effacee : on ne relit rien.
  const QString marker = loggedOutMarkerPath();
  if (QFile::exists(marker)) {
    if (osSecretDelete(allowPrompt)) {
      osLegacyDelete(allowPrompt);
      QFile::remove(marker);
      blog(LOG_INFO, "[semirprod-tokenstore] suppression differee de la liaison : faite");
    }
    QFile::remove(fallbackFilePath());
    return 0;
  }

  // 2. tokens.bin lisible : il fait foi (voir plus haut), puis migre.
  const QString path = fallbackFilePath();
  if (QFile::exists(path)) {
    const TokenBundle legacy = readTokenFile(path);
    if (legacy.isBound()) {
      // Sans invite permise, la recopie (qui peut demander le deverrouillage)
      // attend le prochain chargement : le fichier reste, il fait foi.
      if (!allowPrompt) {
        *out = legacy;
        return 0;
      }
      if (osSecretWrite(serialize(legacy).toUtf8())) {
        QFile::remove(path);
        blog(LOG_INFO, "[semirprod-tokenstore] jetons migres de tokens.bin vers le trousseau du systeme");
      } else {
        blog(LOG_WARNING,
             "[semirprod-tokenstore] trousseau du systeme indisponible : jetons "
             "conserves dans tokens.bin (0600)");
      }
      *out = legacy;
      return 0;
    }
  }

  // 3. Le trousseau, sous notre schema.
  QByteArray blob;
  const SecretRead lu = osSecretRead(blob, allowPrompt);
  if (lu == SecretRead::Found) {
    *out = deserialize(QString::fromUtf8(blob));
    return 0;
  }
  if (lu == SecretRead::Locked) return 1;
  if (lu == SecretRead::Absent) return 2;
  if (lu == SecretRead::Refused) return 3;

  // 4. Absent : une entree d'un build QtKeychain (Linux) ?
  QByteArray legacyBlob;
  const SecretRead ancien = osLegacyRead(legacyBlob, allowPrompt);
  if (ancien == SecretRead::Locked) return 1;
  if (ancien == SecretRead::Refused) return 3;
  if (ancien == SecretRead::Found) {
    const TokenBundle legacy = deserialize(QString::fromUtf8(legacyBlob));
    if (legacy.isBound()) {
      if (allowPrompt && osSecretWrite(serialize(legacy).toUtf8())) {
        osLegacyDelete();
        blog(LOG_INFO, "[semirprod-tokenstore] jetons migres de l'entree QtKeychain");
      }
      *out = legacy;
    }
  }
  return 0;
}

void TokenStore::loadFromSecretStore(LoadCallback cb) {
  // Chaque load() remplace un essai en attente : c'est son callback qui compte.
  if (retryTimer_) retryTimer_->stop();
  retryCallback_ = cb;
  retryAttempts_ = 0;
  retryDelayMs_ = 15000;
  const quint64 generation = ++loadGeneration_;
  QPointer<TokenStore> self(this);
  runOnSecretThread([self, generation, closing = closing_]() {
    if (closing->load()) return;  // OBS se ferme : lecture inutile
    TokenBundle bundle;
    const int etat = readSecretStore(&bundle, /*allowPrompt=*/true);
    postToStore(self, [generation, bundle, etat](TokenStore* store) {
      store->onSecretRead(generation, bundle, etat, /*retry=*/false);
    });
  });
}

void TokenStore::onSecretRead(quint64 generation, const TokenBundle& bundle, int etat,
                              bool retry) {
  // Lecture perimee : un save(), un clear() ou un appairage est passe depuis.
  if (generation != loadGeneration_) return;

  if (etat != 0) {
    const bool premier = !retry;
    // Session en cours deja liee (reprise, rechargement) : le trousseau
    // illisible ne la tue pas. La liaison en memoire reste celle qui sert.
    if (cached_.isBound()) {
      if (premier && retryCallback_) retryCallback_(cached_);
      return;
    }
    unavailable_ = true;
    if (etat == 3) {
      // Refus de l'utilisateur : rien d'automatique pour cette session, une
      // nouvelle tentative reafficherait l'invite qu'il vient de fermer. Le
      // prochain lancement d'OBS, ou un nouvel appairage, repart de zero.
      refused_ = true;
      if (premier) {
        cached_ = {};
        blog(LOG_WARNING,
             "[semirprod-tokenstore] acces au trousseau refuse : aucun nouvel essai "
             "pendant cette session");
        if (retryCallback_) retryCallback_(cached_);
      }
      return;
    }
    const bool verrouille = etat == 1;
    const int plafond = verrouille ? kMaxLockedRetries : kMaxAbsentRetries;
    if (premier) {
      cached_ = {};
      blog(LOG_WARNING,
           verrouille
               ? "[semirprod-tokenstore] trousseau verrouille : nouvel essai en arriere-plan, "
                 "la liaison n'est PAS effacee"
               : "[semirprod-tokenstore] service de trousseau injoignable : jetons dans "
                 "tokens.bin si besoin, quelques essais encore");
    }
    if (retryAttempts_ < plafond) {
      ++retryAttempts_;
      if (!retryTimer_) {
        retryTimer_ = new QTimer(this);
        retryTimer_->setSingleShot(true);
        connect(retryTimer_, &QTimer::timeout, this, [this]() {
          const quint64 gen = loadGeneration_;
          QPointer<TokenStore> self(this);
          runOnSecretThread([self, gen, closing = closing_]() {
            if (closing->load()) return;
            TokenBundle b;
            // Essai en arriere-plan : jamais d'invite (recherche sans
            // deverrouillage, lecture seulement si tout est deverrouille).
            const int e = readSecretStore(&b, /*allowPrompt=*/false);
            postToStore(self, [gen, b, e](TokenStore* store) {
              store->onSecretRead(gen, b, e, /*retry=*/true);
            });
          });
        });
      }
      if (retry) retryDelayMs_ = std::min(retryDelayMs_ * 2, 5 * 60 * 1000);
      retryTimer_->start(retryDelayMs_);
    } else {
      // Plafond atteint : on arrete d'essayer, l'etat redevient « lu, vide ».
      unavailable_ = false;
      blog(LOG_WARNING, "[semirprod-tokenstore] trousseau toujours illisible : essais arretes");
      emit storeStateChanged();
    }
    if (premier && retryCallback_) retryCallback_(cached_);
    return;
  }

  const bool etaitIndisponible = unavailable_;
  unavailable_ = false;
  if (retry && !bundle.isBound()) {
    // Lisible, mais rien dedans : l'ecran d'appairage doit le refleter.
    if (etaitIndisponible) emit storeStateChanged();
    return;
  }
  if (retry) blog(LOG_INFO, "[semirprod-tokenstore] trousseau de nouveau lisible : liaison retrouvee");
  cached_ = bundle;
  if (retryCallback_) retryCallback_(cached_);
}


void TokenStore::saveToSecretStore(const TokenBundle& bundle, SimpleCallback cb) {
  // Liaison neuve : une relecture en attente rendrait l'ancienne.
  ++loadGeneration_;
  if (retryTimer_) retryTimer_->stop();
  retryCallback_ = nullptr;
  unavailable_ = false;
  refused_ = false;
  // Derniere ecriture demandee : rejouee a la fermeture si le fil ne l'a pas
  // faite (~TokenStore).
  const quint64 seq = ++writeSeq_;
  lastWriteIsSave_ = true;
  lastSavedBundle_ = bundle;
  QPointer<TokenStore> self(this);
  runOnSecretThread([self, bundle, cb = std::move(cb), seq, done = writesDone_]() {
    const QString path = fallbackFilePath();
    const QString marker = loggedOutMarkerPath();
    bool ok = false;
    if (osSecretWrite(serialize(bundle).toUtf8())) {
      // Le trousseau fait foi : un tokens.bin restant serait relu en priorite,
      // et un marqueur de deconnexion masquerait la liaison neuve.
      if (QFile::exists(path)) QFile::remove(path);
      QFile::remove(marker);
      ok = true;
    } else {
      // Le jeton de rafraichissement vient d'etre tourne cote serveur : ne pas
      // le persister du tout deconnecterait le poste au prochain demarrage.
      // Repli sur le fichier 0600, relu en priorite et remigre au chargement.
      blog(LOG_WARNING,
           "[semirprod-tokenstore] trousseau du systeme indisponible : jetons "
           "ecrits dans tokens.bin (0600) en attendant");
      ok = writeTokenFile(path, bundle);
      if (ok) QFile::remove(marker);
    }
    done->store(seq);
    postToStore(self, [cb, ok](TokenStore*) {
      if (cb) cb(ok);
    });
  });
}

void TokenStore::clearSecretStore(SimpleCallback cb) {
  // Plus aucune relecture : elle rendrait la liaison qu'on efface.
  ++loadGeneration_;
  if (retryTimer_) retryTimer_->stop();
  retryCallback_ = nullptr;
  unavailable_ = false;
  refused_ = false;
  // Marqueur de deconnexion pose TOUT DE SUITE, sur le fil Qt : si OBS se
  // ferme avant que le fil du trousseau ait supprime l'entree, le prochain
  // lancement ne se reconnecte pas. Retire par le fil des que la suppression
  // a reussi, ou par une liaison neuve (saveToSecretStore).
  {
    QFile f(loggedOutMarkerPath());
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
      f.write("1");
      f.close();
    }
  }
  const quint64 seq = ++writeSeq_;
  lastWriteIsSave_ = false;
  lastSavedBundle_ = {};
  QPointer<TokenStore> self(this);
  runOnSecretThread([self, cb = std::move(cb), seq, done = writesDone_]() {
    // L'entree QtKeychain aussi : survivante, elle serait remigree au
    // prochain lancement et reconnecterait le poste apres la deconnexion.
    const bool secretCleared = osSecretDelete();
    osLegacyDelete();
    const QString path = fallbackFilePath();
    const QString marker = loggedOutMarkerPath();
    QFile::remove(path);
    if (secretCleared) {
      QFile::remove(marker);
    } else {
      // Entree impossible a supprimer (trousseau verrouille) : le marqueur est
      // REPOSE ici. Celui du fil Qt a pu etre retire entre-temps par un
      // enregistrement demande avant la deconnexion (jeton qui venait de
      // tourner) : le fil est unique, cet enregistrement est passe AVANT
      // cette tache. Une liaison neuve demandee APRES passera apres elle et
      // retirera le marqueur, comme il se doit.
      QFile f(marker);
      if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write("1");
        f.close();
      }
      blog(LOG_WARNING,
           "[semirprod-tokenstore] deconnexion : entree du trousseau non supprimee, "
           "nouvel essai plus tard (marqueur pose)");
    }
    const bool ok = secretCleared && !QFile::exists(path);
    done->store(seq);
    postToStore(self, [cb, ok, secretCleared, marker](TokenStore* store) {
      if (!secretCleared) {
        // Nouvel essai dans une minute, seulement si aucune liaison neuve n'a
        // retire le marqueur entre-temps (sinon on effacerait celle-ci).
        QTimer::singleShot(60000, store, [store, marker]() {
          // Sans invite : un essai differe ne doit jamais faire surgir une
          // fenetre de trousseau une minute apres la deconnexion.
          store->runOnSecretThread([marker]() {
            if (QFile::exists(marker) && osSecretDelete(/*allowPrompt=*/false)) {
              osLegacyDelete(/*allowPrompt=*/false);
              QFile::remove(marker);
            }
          });
        });
      }
      if (cb) cb(ok);
    });
  });
}

#endif // SEMIRPROD_OS_SECRET_STORE

QString TokenStore::fallbackFilePath() {
  QFileInfo info(Settings::configFilePath());
  return info.absoluteDir().filePath(QStringLiteral("tokens.bin"));
}

void TokenStore::loadFromFile(LoadCallback cb) {
  QTimer::singleShot(0, this, [this, cb = std::move(cb)]() {
#ifndef Q_OS_WIN
    // Hors Windows, ce fichier n'est protege que par ses permissions. Normal
    // dans l'edition cloud (conteneur sans trousseau) ; sur un poste, c'est un
    // poste sans QtKeychain ni libsecret utilisable, et ca merite d'etre su.
    if (!edition::isCloud()) {
      blog(LOG_WARNING,
           "[semirprod-tokenstore] aucun trousseau systeme dans ce build : jetons "
           "dans tokens.bin, proteges par les seules permissions du fichier (0600)");
    }
#endif
    cached_ = readTokenFile(fallbackFilePath());
    if (cb) cb(cached_);
  });
}

void TokenStore::saveToFile(const TokenBundle& bundle, SimpleCallback cb) {
  QTimer::singleShot(0, this, [bundle, cb = std::move(cb)]() {
    const bool ok = writeTokenFile(fallbackFilePath(), bundle);
    if (cb) cb(ok);
  });
}

void TokenStore::clearFile(SimpleCallback cb) {
  QTimer::singleShot(0, this, [cb = std::move(cb)]() {
    const bool removed = QFile::remove(fallbackFilePath());
    if (cb) cb(removed || !QFile::exists(fallbackFilePath()));
  });
}

} // namespace semirprod
