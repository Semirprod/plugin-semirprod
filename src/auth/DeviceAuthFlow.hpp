#pragma once

#include "../auth/TokenStore.hpp"

#include <QObject>
#include <QString>

#include <functional>

class QTimer;

namespace semirprod {

class HttpClient;

/** Public state of an in-flight device pairing. The dock binds this to
    the UI: shows the user code, opens the verification URL in the
    browser, optionally lets the user copy the code manually. */
struct DeviceFlowSession {
  /** Browser URL the user opens. Includes ?user_code= so the consent
      page can pre-fill — but the plugin still surfaces the bare code so
      the user can type it on a phone. */
  QString verificationUri;
  /** Same as verificationUri but without the user_code query — what to
      show in plain text when the URL above is too long for a QR / phone
      retype. */
  QString verificationUriBase;
  /** XXXX-XXXX human-typed pairing code. Already formatted for display. */
  QString userCode;
  /** Plugin-side expiry derived from /device/code.expires_in. The dock
      uses this to show a countdown and to stop polling once the code is
      stale instead of hammering the server. */
  qint64 expiresAtMsSinceEpoch = 0;
};

/** OAuth 2.0 Device Authorization Grant (RFC 8628) client.
 *
 * Flow:
 *   1. start() POSTs /api/auth/obs/device/code → emits deviceFlowStarted
 *      with the user code + verification URL.
 *   2. The dock opens the verification URL in the user's browser. On any
 *      machine where that's impossible (kiosk, RDP), the user copies the
 *      code to a phone and visits the URL there.
 *   3. We poll /api/auth/obs/device/token at the server-advertised
 *      interval. Backs off on slow_down per RFC §3.5.
 *   4. On success, exchange the OAuth bearer for a live-ws JWT via
 *      /api/auth/obs/token. Emits deviceFlowSucceeded with the bundle.
 *   5. refresh() reuses /api/auth/oauth2/token (refresh_token grant) —
 *      the device flow only handles initial pairing; refresh is identical
 *      to what the OIDC code grant would do.
 *
 * Same signal surface as the legacy PkceFlow so SemirprodDock barely
 * notices the swap. */
class DeviceAuthFlow : public QObject {
  Q_OBJECT

public:
  explicit DeviceAuthFlow(HttpClient* http, QObject* parent = nullptr);

  void start();
  /**
   * Rouvre une session d'appareil (JWT de 15 min). Ré-échange le jeton OAuth
   * opaque s'il vit encore au moins 5 min, sinon le rafraîchit d'abord : une
   * ligne de jeton OAuth par heure au lieu de quatre.
   */
  void refresh(const TokenBundle& bundle);
  void cancel();
  /**
   * L'appareil se retire lui-même (bouton « Déconnecter »). Au mieux, 3 s au
   * plus : le rappel part toujours, et l'appelant efface la liaison locale
   * quoi qu'il arrive. Un appareil qui n'a pas pu se retirer reste révocable
   * depuis le dashboard.
   */
  void release(const TokenBundle& bundle, std::function<void()> done);
  bool isRunning() const { return running_; }
  /** Appairage interactif en cours (code affiché, en attente du streamer). */
  bool isPairing() const { return running_ && !deviceCode_.isEmpty(); }
  DeviceFlowSession currentSession() const { return session_; }

signals:
  void deviceFlowStarted(const semirprod::DeviceFlowSession& session);
  void deviceFlowSucceeded(const semirprod::TokenBundle& bundle);
  void deviceFlowFailed(const QString& errorMessage);

  // La session stockee est DEFINITIVEMENT morte : jeton de rafraichissement
  // revoque, expire, ou emis par un autre environnement. A distinguer de
  // deviceFlowFailed, qui couvre les pannes passageres (reseau coupe, 5xx) ou
  // reessayer a du sens.
  //
  // Sans ce signal, les deux cas se confondaient : le plugin gardait ses jetons
  // et reessayait en boucle, donc l'interface affichait « connecte » pour une
  // session que le serveur refusait — l'etat « connecte » ne veut dire que
  // « j'ai une chaine de jeton en memoire ».
  void sessionRevoked(const QString& reason);
  /** Mode securite : cette version est bloquee (426 `plugin_blocked`). La
      liaison est GARDEE : apres la mise a jour, le poste repart sans appairage. */
  void pluginBlocked();

  void pollingChanged(bool active);

private:
  void requestDeviceCode();
  void schedulePoll(int delaySeconds);
  void poll();
  /** Échange compte + appareil contre un JWT de service. */
  void openDeviceSession(TokenBundle binding, bool oauthJustRefreshed, quint64 generation);
  void refreshAccessToken(TokenBundle binding);
  static QString deviceLabel();
  static QString machineKey();
  static QString platformName();

  HttpClient* http_ = nullptr;
  QTimer* pollTimer_ = nullptr;
  DeviceFlowSession session_;
  QString deviceCode_;
  int intervalSeconds_ = 5;
  bool running_ = false;
  // Incremente par cancel() : une reponse HTTP qui revient avec une autre
  // generation est ignoree (deconnexion pendant un renouvellement).
  quint64 generation_ = 0;
  // Un renouvellement (refresh) en vol : un second appel attend son resultat.
  bool refreshInFlight_ = false;
  // Tracks which token we're refreshing so we can return it unchanged
  // if the server rotates the refresh_token (server returns the same
  // refresh_token for now, but the contract allows rotation).
  QString pendingRefreshToken_;
  // Organisation annoncée à l'appairage (nom, slug) : l'échange de session ne
  // rend que l'identifiant, le nom sert à l'affichage.
  QString pairedOrgName_;
  QString pairedOrgSlug_;
};

} // namespace semirprod
