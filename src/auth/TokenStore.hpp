#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QThreadPool>
#include <QTimer>
#include <atomic>
#include <functional>
#include <memory>

namespace semirprod {

/**
 * Liaison d'un poste v2 (0.2.0) : le COMPTE (jetons OAuth), l'APPAREIL
 * (identifiant + secret tirés par le serveur à l'appairage), l'ESPACE auquel
 * l'appareil appartient, et la session courante (JWT de service, 15 min).
 */
struct TokenBundle {
  /** JWT de service de la session d'appareil. Seul porteur envoyé aux API. */
  QString accessToken;
  /** Jeton de rafraîchissement OAuth (90 j). */
  QString refreshToken;
  /** Expiration du JWT, LUE DANS LE JWT (claim `exp`), nulle part ailleurs. */
  QDateTime expiresAt;

  /** Jeton OAuth opaque (1 h) : ré-échangé contre un JWT sans refresh. */
  QString oauthAccessToken;
  QDateTime oauthExpiresAt;

  QString deviceId;
  QString deviceSecret;
  QString orgId;
  QString orgName;
  QString orgSlug;

  bool isValid() const { return !accessToken.isEmpty(); }
  /** Appareil v2 appairé : identifiant et secret présents. */
  bool isBound() const { return !deviceId.isEmpty() && !deviceSecret.isEmpty(); }
  /** Le jeton OAuth opaque reste utilisable au moins `marginSeconds`. */
  bool oauthUsable(int marginSeconds = 300) const {
    return !oauthAccessToken.isEmpty() && oauthExpiresAt.isValid() &&
           QDateTime::currentDateTimeUtc().addSecs(marginSeconds) < oauthExpiresAt;
  }

  QJsonObject toJson() const;
  /** Rend un bundle vide pour tout format autre que v2 (liaison 0.1.x). */
  static TokenBundle fromJson(const QJsonObject& obj);
  bool isExpired(int thresholdSeconds = 30) const {
    if (!expiresAt.isValid()) return false;
    return QDateTime::currentDateTimeUtc().addSecs(thresholdSeconds) >= expiresAt;
  }
};

/**
 * Secure token storage:
 *  - QtKeychain when compiled in (OS keyring);
 *  - otherwise macOS Keychain (Security.framework) or the Linux Secret
 *    Service (libsecret loaded at RUNTIME, never in the cloud edition), with
 *    one-time migration of a legacy tokens.bin and, on Linux, of an entry
 *    left by a QtKeychain build;
 *  - otherwise a file in the OBS config directory: DPAPI on Windows, 0600
 *    obfuscated file elsewhere (cloud edition, Linux without libsecret).
 *
 * Operations are asynchronous-safe: callers pass callbacks that are
 * guaranteed to fire on the Qt main thread.
 */
class TokenStore : public QObject {
  Q_OBJECT

public:
  explicit TokenStore(QObject* parent = nullptr);
  ~TokenStore() override;

  using LoadCallback = std::function<void(TokenBundle)>;
  using SimpleCallback = std::function<void(bool ok)>;

  void load(LoadCallback cb);
  void save(const TokenBundle& bundle, SimpleCallback cb);

  /**
   * Enregistre un jeton de rafraîchissement qui vient de tourner, AVANT l'échange
   * de session. Le serveur a déjà révoqué l'ancien : si cet échange échoue et que
   * le plugin rejoue l'ancien jeton, le serveur y voit un vol et supprime tous les
   * jetons du compte, ceux de ses autres postes compris.
   */
  void saveRotated(const TokenBundle& bundle);
  void clear(SimpleCallback cb);

  // Synchronous cached access — only valid after a successful load().
  const TokenBundle& cached() const { return cached_; }
  void setCached(const TokenBundle& bundle) { cached_ = bundle; }

  /** Vrai si le dernier load() n'a PAS pu lire le trousseau (verrouille,
      indisponible) : la liaison vide n'est alors pas « jamais appaire ». Le
      magasin reessaie seul et rappelle le callback de load() des que la
      liaison se lit. */
  bool isUnavailable() const { return unavailable_; }
  /** Vrai si l'utilisateur a REFUSE l'acces au trousseau (invite fermee) :
      aucun essai automatique pendant cette session. */
  bool isRefused() const { return refused_; }

  /** Abandonne une relecture en attente (appairage qui demarre) : elle ne doit
      pas reconnecter l'ancienne liaison pendant qu'on en cree une neuve. */
  void cancelPendingLoad();

signals:
  void tokenChanged();
  /** isUnavailable() a change sans qu'une liaison soit rendue (essais
      termines, trousseau lisible mais vide) : l'ecran doit se mettre a jour. */
  void storeStateChanged();

private:
  TokenBundle cached_;

  // Trousseau indisponible au chargement : relecture en arriere-plan.
  bool unavailable_ = false;
  bool refused_ = false;
  // Mis a vrai par le destructeur : les lectures en attente sur le fil du
  // trousseau s'arretent (les ecritures, elles, vont au bout).
  std::shared_ptr<std::atomic<bool>> closing_ = std::make_shared<std::atomic<bool>>(false);
  // Derniere ecriture demandee au fil du trousseau (enregistrement ou
  // deconnexion) et son numero ; le fil note le numero de la derniere qu'il a
  // finie. A la fermeture, une ecriture pas finie est rejouee sur le fil Qt
  // sans trousseau (~TokenStore).
  quint64 writeSeq_ = 0;
  bool lastWriteIsSave_ = false;
  TokenBundle lastSavedBundle_;
  std::shared_ptr<std::atomic<quint64>> writesDone_ =
      std::make_shared<std::atomic<quint64>>(0);
  QTimer* retryTimer_ = nullptr;
  LoadCallback retryCallback_;
  int retryDelayMs_ = 15000;
  int retryAttempts_ = 0;
  // Toute operation (save, clear, appairage, nouveau load) perime les lectures
  // en vol : leur resultat est ignore s'il revient avec une autre generation.
  quint64 loadGeneration_ = 0;
  // Fil unique des appels au trousseau (jamais le fil Qt : invite possible).
  // Sans parent : detruit a la main, sans attente illimitee (~TokenStore).
  QThreadPool* secretPool_ = nullptr;

  void loadFromKeychain(LoadCallback cb);
  void saveToKeychain(const TokenBundle& bundle, SimpleCallback cb);
  void clearKeychain(SimpleCallback cb);

  // Trousseau du systeme sans QtKeychain (macOS, Linux + libsecret). Definies
  // seulement quand ce support est compile.
  void loadFromSecretStore(LoadCallback cb);
  void saveToSecretStore(const TokenBundle& bundle, SimpleCallback cb);
  void clearSecretStore(SimpleCallback cb);
  /** Lecture complete (marqueur de deconnexion, migrations, trousseau), sur
      le fil de travail. 0 = lu, 1 = verrouille, 2 = service absent, 3 = refus
      de l'utilisateur. `allowPrompt` faux : aucune invite possible. */
  static int readSecretStore(TokenBundle* out, bool allowPrompt);
  /** Resultat d'une lecture, de retour sur le fil Qt. */
  void onSecretRead(quint64 generation, const TokenBundle& bundle, int etat, bool retry);
  void runOnSecretThread(std::function<void()> job);
  static QString loggedOutMarkerPath();

  void persist(const TokenBundle& bundle, SimpleCallback cb);
  void loadFromFile(LoadCallback cb);
  void saveToFile(const TokenBundle& bundle, SimpleCallback cb);
  void clearFile(SimpleCallback cb);

  static QString fallbackFilePath();
};

} // namespace semirprod
