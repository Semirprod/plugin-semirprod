#pragma once

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QString>

class QTcpServer;

namespace semirprod {

class SemirprodDock;
class WsServerConnection;

/**
 * Type MIME d'avatar que le pont accepte de servir, normalise (minuscules, sans
 * parametre), ou chaine VIDE s'il est refuse.
 *
 * Acceptes : PNG, JPEG, WebP, GIF. SVG seulement avec `allowSvg` : c'est le
 * format du degrade par defaut que dessine NOTRE site (`/avatars/<id>`), pas
 * celui d'une image de profil, qui peut venir d'ailleurs. Un SVG porte du
 * script : servi sur l'origine du pont et ouvert directement, il s'y
 * executerait. Le meme controle sert a la reception (SemirprodDock) et a
 * l'envoi (GET /avatar).
 */
QString acceptedAvatarType(const QString& contentType, bool allowSvg);

/**
 * Local WebSocket bridge for the browser-side Semirprod UI.
 *
 * Built on QTcpServer + a hand-rolled RFC 6455 server handshake/framing (see
 * WsFraming.hpp) instead of Qt6::WebSockets, so the plugin doesn't link a Qt
 * module OBS doesn't ship — that module pinned us to one Qt version and broke
 * on every OBS Qt update.
 *
 * Security model:
 *   - Listens on 127.0.0.1 only (no LAN exposure).
 *   - Each session generates a random 32-byte token at startup. The browser
 *     UI must include the token as a subprotocol (`bearer-<token>`) to be
 *     accepted. A raw local process that connects to `ws://127.0.0.1:<port>/`
 *     without the token is rejected at the HTTP upgrade, before any framing.
 *   - The token is NEVER written to the OBS log (users publish full logs for
 *     support) and never rendered on screen (streamers share their OBS
 *     window). The dock exposes it only through a copy-to-clipboard button.
 *   - The token changes every OBS restart — no persistent token means a
 *     process that scraped yesterday's token is locked out today.
 *
 * Known limitations:
 *   - The token transits through the system clipboard when the user clicks
 *     the copy button — any process running as the OBS user can read the
 *     clipboard at that moment. Good enough to keep drive-by browser tabs
 *     and naive malware from driving the plugin, not good enough against
 *     targeted local malware. A future revision can move to a named pipe /
 *     UNIX socket with per-connection OS-level identity checks.
 */
class WebUiServer : public QObject {
  Q_OBJECT
 public:
  explicit WebUiServer(SemirprodDock* dock, QObject* parent = nullptr);
  ~WebUiServer() override;

  // Le port de DEPART est deliberement SOUS 49152 : sur Windows, 49152 est le
  // premier port ephemere (dynamique), donc une socket sortante — et le plugin
  // en ouvre beaucoup, HTTPS et WSS — peut le prendre comme port source avant
  // qu'on ne se lie, rendant listen() intermittent. 47615 est dans la plage
  // enregistree, hors des plages ephemeres courantes.
  //
  // UN SEUL port : le produit est mono-instance, et ce port en est le verrou.
  // Voir start() pour pourquoi un repli sur le port suivant a ete ecrit puis
  // retire.
  static constexpr uint16_t kDefaultPort = 47615;
  bool start(uint16_t firstPort = kDefaultPort);
  void broadcastState(const QJsonObject& state);

  /** Port reellement ecoute, ou 0 si le serveur n'a pas demarre.
      A utiliser plutot que la constante : le jour ou le port devient variable,
      les appelants n'auront rien a changer. */
  uint16_t port() const { return port_; }

  /** The bearer token issued at the most recent start() — exposed so the
      dock can surface it in a UI hint and copy-to-clipboard button. */
  QString sessionToken() const { return sessionToken_; }

  /** Cle de l'URL `/avatar`, tiree a chaque start(), DISTINCTE du jeton du pont.
      Elle ne voyage que dans l'etat, donc par le WebSocket authentifie : une
      page tierce ne peut plus charger l'avatar (et apprendre ainsi que le
      plugin tourne et qu'un compte est lie). La fuiter ne donne pas le pont. */
  QString avatarKey() const { return avatarKey_; }

 private slots:
  void onNewConnection();
  void onMessageReceived(const QString& message);
  void onClientClosed();

 private:
  /** Budget de commandes par connexion (fenetre glissante).

      Le token protege l'acces au pont, pas son debit : une page rechargee en
      boucle, un bug de rendu ou un process local ayant recupere le token
      pourraient transformer le pont en generateur de requetes vers l'API
      Semirprod (plusieurs commandes declenchent un appel HTTP cote dock). Le
      trop-plein est JETE, jamais mis en file : reprogrammer une rafale ne fait
      que la deplacer. */
  struct CommandBudget {
    qint64 windowStartMs = 0;
    int count = 0;
    bool overflowLogged = false;
  };

  /** false => la commande doit etre ignoree (budget epuise). */
  bool allowCommand(WsServerConnection* conn);

  QTcpServer* server_;
  /** Connexions authentifiees, seules a recevoir les diffusions d'etat. */
  QSet<WsServerConnection*> clients_;
  /** Connexions acceptees mais pas encore authentifiees.

      Comptees avec les precedentes pour le plafond de connexions : sans elles,
      un processus local ouvrant des sockets muettes n'aurait rien consomme du
      budget, puisque `clients_` ne se remplit qu'apres le handshake. Chacune
      porte un delai de handshake qui la ferme si elle reste muette. */
  QSet<WsServerConnection*> pending_;
  QHash<WsServerConnection*, CommandBudget> budgets_;
  SemirprodDock* dock_;
  QString sessionToken_;
  QString avatarKey_;
  // Port retenu par start(). 0 tant que rien n'ecoute.
  uint16_t port_ = 0;
};

}  // namespace semirprod
