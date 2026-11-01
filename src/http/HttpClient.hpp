#pragma once

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QObject>
#include <QString>
#include <QUrlQuery>
#include <functional>

class QNetworkAccessManager;

namespace semirprod {

struct HttpResponse {
  int statusCode = 0;
  QByteArray body;
  QNetworkReply::NetworkError networkError = QNetworkReply::NoError;
  QString errorString;

  /// Vrai quand la requete n'a JAMAIS ete emise : URL invalide, schema non TLS,
  /// hote hors allowlist. Le rappel part alors de facon SYNCHRONE, avec un
  /// statut 0 comme une panne reseau.
  ///
  /// Sans ce drapeau les deux cas sont indistinguables cote appelant, et le
  /// symptome se voit sur PairingFlow : une URL mal configuree (statut 0)
  /// declenchait la meme reprise exponentielle qu'un cluster pas encore pret,
  /// soit 8 tentatives sur trois minutes pour une erreur purement locale qui ne
  /// se reparera jamais toute seule.
  bool refusedLocally = false;

  bool ok() const { return networkError == QNetworkReply::NoError && statusCode >= 200 && statusCode < 300; }
};

class HttpClient : public QObject {
  Q_OBJECT

public:
  explicit HttpClient(QObject* parent = nullptr);

  /// Coupe la journalisation du CORPS des reponses en echec pour CE client.
  ///
  /// Par defaut le corps des reponses non-2xx est tronque a 400 octets et
  /// journalise : c'est ce qui rend une panne d'API diagnosticable a partir
  /// d'un simple journal OBS, et rien ne doit changer pour les clients
  /// existants.
  ///
  /// Mais un client dedie a l'appairage envoie un secret dans le CORPS de sa
  /// requete, et un backend de validation renvoie volontiers ce qu'il a recu :
  /// un 400 du type {"error":"invalid","received":{"pairingToken":"..."}}
  /// deposait le jeton en clair dans le journal d'OBS — qui part dans les
  /// tickets de support et, en instance infogeree, dans les journaux du conteneur.
  /// Le soin pris plus haut (PairingFlow n'ecrit jamais le corps) ne protegeait
  /// rien : la fuite se produisait un etage plus bas, avant le rappel.
  ///
  /// A poser sur le client de l'appairage UNIQUEMENT, jamais globalement.
  void setLogResponseBodies(bool enabled) { logResponseBodies_ = enabled; }

  using JsonCallback = std::function<void(const HttpResponse&, const QJsonDocument&)>;
  /// `contentType` = l'en-tête Content-Type de la réponse, tel quel.
  using BytesCallback = std::function<void(const HttpResponse&, const QString& contentType)>;

  void getJson(const QString& url, const QString& bearerToken, JsonCallback cb);

  /// GET binaire (avatar de compte). Mêmes gardes que getJson : TLS, hôte dans
  /// l'allowlist, redirections surveillées. `maxBytes` interrompt le transfert
  /// dès que la réponse annonce ou dépasse la taille — on ne charge pas en
  /// mémoire ce qu'un serveur voudrait bien nous envoyer.
  void getBytes(const QString& url, const QString& bearerToken, qint64 maxBytes,
                BytesCallback cb);
  void postJson(const QString& url, const QJsonObject& payload, const QString& bearerToken, JsonCallback cb);

  /// Progression d'un transfert : octets faits, octets attendus.
  using ProgressCallback = std::function<void(qint64 done, qint64 total)>;

  /// Fin d'un telechargement vers fichier. `path` est vide en cas d'echec.
  using FileCallback = std::function<void(const HttpResponse& response, const QString& path)>;

  /**
   * GET ECRIT DIRECTEMENT DANS UN FICHIER, par morceaux.
   *
   * Distinct de `getBytes`, qui accumule la reponse en memoire : un paquet de
   * collection pese des centaines de megaoctets, le charger d'un bloc ferait
   * respirer la memoire d'OBS pendant un direct. Ici chaque morceau recu part
   * sur le disque et n'est plus garde.
   *
   * `expectedSha256` (facultatif, hexadecimal) est verifie AU FIL DE L'EAU : le
   * fichier est efface si l'empreinte ne correspond pas, plutot que laisse a
   * l'operateur avec un contenu qu'il croirait bon.
   *
   * `destPath` doit EXISTER, cree par l'appelant (QTemporaryFile : nom
   * aleatoire, 0600) : il est rouvert en troncature, jamais cree ici. Un
   * fichier efface entre-temps fait echouer l'appel au lieu d'etre recree avec
   * les droits par defaut sous un nom desormais connu.
   */
  void getToFile(const QString& url, const QString& bearerToken, const QString& destPath,
                 const QString& expectedSha256, qint64 maxBytes,
                 ProgressCallback onProgress, FileCallback cb);

  /**
   * PUT d'un FICHIER, en flux depuis le disque.
   *
   * Le fichier n'est jamais charge en memoire : `QFile` est passe comme
   * peripherique a Qt, qui l'envoie par blocs. Un paquet de collection pese
   * couramment plusieurs centaines de megaoctets, et le lire d'un bloc ferait
   * respirer la memoire d'OBS pendant un direct.
   *
   * `Content-Length` est renseigne par Qt depuis la taille du fichier : c'est ce
   * que MinIO exige, il refuse l'envoi par morceaux.
   *
   * Le peripherique est detruit avec la reponse, pas avant : Qt lit encore
   * dedans pendant tout le transfert, et le liberer plus tot est un
   * plantage differe qu'aucun journal n'explique.
   */
  void putFile(const QString& url, const QString& filePath, const QString& contentType,
               const QString& bearerToken, ProgressCallback onProgress, JsonCallback cb);
  void postForm(const QString& url, const QUrlQuery& form, const QString& bearerToken, JsonCallback cb);

  /**
   * DELETE, memes gardes que le reste (TLS, allowlist d'hotes, redirections
   * surveillees).
   *
   * Existe pour ANNULER une reservation de quota quand une sauvegarde abandonne :
   * sans ce chemin, la place reservee reste comptee au client jusqu'a sa
   * peremption, et il lit un espace occupe qui ne correspond a aucune sauvegarde
   * visible.
   */
  void deleteJson(const QString& url, const QString& bearerToken, JsonCallback cb);

private:
  // Wire the per-reply security guards: never ignore SSL errors, and abort
  // any redirect whose target is non-TLS or outside the host allowlist
  // BEFORE the redirected request (carrying the bearer) is sent.
  void attachGuards(QNetworkReply* reply);

  QNetworkAccessManager* manager_;
  // Vrai par defaut : le comportement d'un poste client ne change pas.
  bool logResponseBodies_ = true;
};

} // namespace semirprod
