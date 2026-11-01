#pragma once

#include "CollectionPackager.hpp"

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

class QThread;

namespace semirprod {

class HttpClient;

/**
 * Sauvegarde de collections : empaquetage, reservation de quota, depot.
 *
 * ─── Le protocole, et pourquoi il a trois temps ────────────────────────────
 *
 * 1. le plugin met la collection en paquet (`CollectionPackager`) ;
 * 2. il ANNONCE le paquet (`POST /api/obs/scene-collections`) : nom, taille,
 *    empreinte, manifeste. Le serveur verifie le quota de l'organisation et
 *    RESERVE la place avant que le moindre octet ne parte ;
 * 3. le paquet monte (`PUT .../{id}/package`), et le serveur confirme apres
 *    avoir recompte les octets et recalcule l'empreinte.
 *
 * Annoncer avant d'envoyer evite d'occuper une connexion domestique pendant
 * vingt minutes pour se faire refuser a l'arrivee : un quota plein se sait au
 * premier aller-retour. Cote serveur, la reservation empeche deux sauvegardes
 * simultanees de passer chacune le controle en se croyant seules.
 *
 * ─── Plusieurs collections : une FILE, jamais du parallele ─────────────────
 *
 * L'operateur choisit une collection, plusieurs, ou toutes. Elles sont traitees
 * l'une APRES l'autre : empaqueter lit des centaines de megaoctets et calcule
 * autant d'empreintes, deux paquets a la fois se disputeraient le meme disque
 * pour finir plus tard qu'en serie. Une collection qui echoue n'arrete pas la
 * file — son erreur est conservee et la suivante part. Sur « tout sauvegarder »,
 * un seul habillage casse ne doit pas emporter les neuf autres.
 *
 * ─── L'EMPAQUETAGE NE TOURNE PAS SUR LE FIL D'OBS ─────────────────────────
 *
 * Lire des centaines de megaoctets et calculer autant d'empreintes sur le fil
 * principal gele toute l'interface d'OBS le temps que ca dure : l'operateur voit
 * un logiciel qui ne repond plus, et le systeme finit par le declarer bloque.
 * C'est ce qui s'est produit le 20/08/2026.
 *
 * Le partage est donc net : `CollectionPackager::prepare` consulte l'API d'OBS
 * sur le fil principal (rapide), puis `CollectionPackager::run` fait le travail
 * lourd sur un fil de travail, sans toucher a libobs. Le resultat revient par
 * `QMetaObject::invokeMethod` en file d'attente, donc sur le fil principal.
 *
 * Corollaire a respecter : RIEN dans ce service ne doit appeler libobs depuis le
 * fil de travail, et le service ne doit pas etre detruit pendant qu'un fil
 * tourne (`QPointer` cote fil, arret attendu au destructeur).
 *
 * ─── Ce que ce service refuse de faire ────────────────────────────────────
 *
 * Sauvegarder pendant une DIFFUSION ou un ENREGISTREMENT. L'empaquetage lit des
 * centaines de megaoctets sur le disque et calcule autant d'empreintes : le
 * faire pendant que le meme disque enregistre le direct est le meilleur moyen de
 * produire des images sautees. Le refus est explicite, pas silencieux.
 *
 * Empaqueter une collection qu'OBS ne declare pas. La verification vit dans
 * `CollectionPackager::packageCollection` : un nom, jamais un chemin.
 */
class BackupService : public QObject {
  Q_OBJECT

public:
  /** Ou en est la sauvegarde. Le nom est celui que l'interface affiche. */
  enum class Phase {
    Idle,
    Packaging,
    Reserving,
    Uploading,
    Done,
    Failed,
  };

  /** Une sauvegarde deja stockee, telle que le serveur la liste. */
  struct RemoteBackup {
    QString id;
    QString collectionName;
    qint64 sizeBytes = 0;
    int mediaCount = 0;
    QString createdAt;
    QString instanceLabel;
    QString sha256;
    /** Fichiers ecartes du paquet, tous motifs confondus. */
    int skippedCount = 0;
    /**
     * Types de source que le poste d'origine ne savait pas rendre — donc
     * fournis par un plugin tiers. A afficher : sans eux, la collection
     * restauree aura des sources manquantes que rien n'explique.
     */
    QStringList unknownSourceTypes;
    /** Creee par le compte connecte, selon le serveur (`createdByCurrentUser`). */
    bool ownBackup = false;
  };

  /**
   * Ou en est un IMPORT dans OBS.
   *
   * `Downloading` puis `Extracting` sont distingues parce que le premier depend
   * du reseau et le second du disque : quand ca traine, l'operateur doit savoir
   * lequel des deux attendre, sinon il relance.
   */
  enum class ImportPhase { Idle, Downloading, Extracting, Done, Failed };

  /** Une collection de la file qui a echoue, avec son motif. */
  struct FailedCollection {
    QString collectionName;
    QString error;
  };

  /**
   * Plafond d'une salve.
   *
   * Un OBS peut porter des dizaines de collections ; les envoyer toutes d'un
   * coup remplirait le quota du client avant qu'il ne voie ce qui se passe. Au
   * dela, la salve est refusee avec un message, jamais tronquee en silence.
   */
  static constexpr int kMaxBatch = 20;

  explicit BackupService(HttpClient* http, QObject* parent = nullptr);
  ~BackupService() override;

  /**
   * Jeton du compte, relu a chaque appel : il tourne au fil des sessions.
   *
   * Jeton d'un AUTRE compte (ou d'un autre espace, ou vide a la deconnexion) :
   * la liste des sauvegardes et le quota en memoire sont oublies. Ils portent
   * la provenance (`ownBackup`, « creee par toi ») du compte precedent : un
   * import lance depuis cette liste rendrait a un autre compte les medias,
   * raccourcis et projecteurs d'une sauvegarde qui n'est pas la sienne.
   */
  void setBearerToken(const QString& token);

  /**
   * Renouvellement du jeton, fourni par le dock, appele sur un 401.
   *
   * Le jeton du compte vit environ une heure. Une sauvegarde ou un import
   * lances apres son expiration se faisaient refuser en 401 et rendaient
   * « Compte non reconnu. Reconnectez le plugin a votre compte. » : un geste
   * IMPOSSIBLE sur une instance infogeree, ou le jeton vient de l'appairage
   * au demarrage et ou aucun ecran de connexion n'existe. Le client restait
   * devant un echec sans issue jusqu'au redemarrage de l'instance.
   *
   * La fonction recoit un rappel a qui elle rend le jeton FRAIS, ou une chaine
   * vide si le renouvellement echoue. Elle doit toujours appeler ce rappel,
   * exactement une fois : sans lui, la requete en attente ne se conclut jamais
   * et l'ecran reste sur « telechargement ».
   */
  using TokenRenewer = std::function<void(std::function<void(const QString&)>)>;
  void setTokenRenewer(TokenRenewer renewer) { renewer_ = std::move(renewer); }

  /** Nom et identifiant du poste, joints a la sauvegarde pour l'historique. */
  void setInstance(const QString& instanceId, const QString& instanceLabel);

  /** Lance l'empaquetage puis le depot de la collection ACTIVE. */
  void backupCurrentCollection();

  /**
   * Lance la file sur ces collections, dans cet ordre.
   *
   * Les noms inconnus d'OBS sont ECARTES et signales, pas devines : un nom
   * approche designerait l'habillage de quelqu'un d'autre. Liste vide = la
   * collection active, ce qui est le geste par defaut de l'interface.
   */
  void backupCollections(const QStringList& collectionNames);

  /** Relit la liste et le quota depuis le serveur. */
  void refresh();

  /**
   * Recoit un paquet et l'INSTALLE dans OBS (cf. `CollectionImporter`).
   *
   * Deux phases, nommees separement dans l'etat : « Reception » puis
   * « Installation ». Le paquet est ecrit AU FIL DE L'EAU
   * (`HttpClient::getToFile`) et son empreinte verifiee — un fichier partiel ou
   * altere est efface plutot que laisse a quelqu'un qui le prendrait pour une
   * sauvegarde valide. Le dezippage et la reecriture des chemins tournent
   * ensuite sur un fil de travail.
   *
   * N'ECRASE jamais une collection existante (un nom deja pris devient
   * « <nom> (importee) ») et ne bascule pas OBS dedans.
   *
   * ⚠️ Ne bloque rien, et ne bloque pas non plus une sauvegarde en cours :
   * l'import a son propre etat, distinct de la file d'empaquetage. Un geste qui
   * echoue ne doit pas immobiliser les autres.
   */
  void importBackup(const QString& backupId);

  /**
   * Nom de la collection que le dernier import a installee, VIDE tant que cet
   * import n'a pas reussi. Seul nom que le bouton « Ouvrir » du Studio peut
   * faire ouvrir : la page ne nomme pas la collection.
   */
  QString importedCollectionName() const {
    return importPhase_ == ImportPhase::Done ? importCollectionName_ : QString();
  }

  Phase phase() const { return phase_; }
  const QString& lastError() const { return lastError_; }
  int progressPercent() const { return progressPercent_; }
  qint64 quotaBytes() const { return quotaBytes_; }
  qint64 usedBytes() const { return usedBytes_; }
  const QVector<RemoteBackup>& backups() const { return backups_; }

  /** Etat complet, pour l'instantane envoye a l'interface. */
  QJsonObject toJson() const;

signals:
  /** Emis a CHAQUE changement d'etat : l'interface ne sonde jamais. */
  void changed();
  /** Ligne pour le Journal de l'operateur (`level` : info, warn, error). */
  void notice(const QString& level, const QString& message);

private:
  bool busy() const;
  void startNext();
  /** Suite de `startNext`, appelee sur le fil principal quand le paquet est pret. */
  void onPackaged(const CollectionPackager::Result& package);
  /** Suite de `importBackup`, une fois le paquet recu : dezippe sur un fil. */
  void extractDownloaded(const QString& packagePath, const QString& collectionName,
                         bool ownBackup);
  void finishCurrent(const QString& error);
  void fail(const QString& message);
  void setPhase(Phase phase);
  void uploadPackage(const QString& backupId, const QString& uploadPath);
  void discardPackage();
  /** Rend au client la place reservee pour une sauvegarde abandonnee. */
  void cancelReservation();

  /**
   * Rejoue UNE fois une requete refusee en 401, apres renouvellement du jeton.
   *
   * Rend vrai quand la requete est reprise en charge : l'appelant doit alors
   * abandonner son propre traitement d'erreur, la suite se joue dans `rejouer`.
   * Rend faux dans tous les autres cas (autre code, pas de renouvellement
   * disponible, renouvellement deja en cours, deuxieme 401 d'affilee) et
   * l'appelant affiche son message comme avant.
   *
   * Une seule reprise : si le serveur refuse encore avec un jeton neuf, ce
   * n'est plus une expiration, et boucler ne ferait que masquer la cause.
   */
  bool rejouerApresRenouvellement(int statusCode, std::function<void()> rejouer);

  HttpClient* http_ = nullptr;
  QString bearerToken_;
  /** Compte et espace du jeton (`sub` et `org` du JWT), pour voir qu'il change. */
  QString accountKey_;
  /**
   * Avance a chaque oubli de la liste (setBearerToken) : une reponse de liste
   * ou un telechargement lance pour le compte precedent est ignore a son
   * retour, sinon il remettrait sa provenance en memoire.
   */
  quint64 accountEpoch_ = 0;
  TokenRenewer renewer_;
  /** Un renouvellement a la fois, et jamais deux reprises pour un meme geste. */
  bool renouvellementEnCours_ = false;
  QString instanceId_;
  QString instanceLabel_;

  Phase phase_ = Phase::Idle;
  QString lastError_;
  int progressPercent_ = 0;

  /** Collections restant a traiter, dans l'ordre. */
  QStringList queue_;
  /** Celle en cours, vide hors traitement. */
  QString currentCollection_;
  /** Taille de la salve et nombre de collections deja terminees (reussies ou non). */
  int batchTotal_ = 0;
  int batchDone_ = 0;
  QVector<FailedCollection> failures_;

  ImportPhase importPhase_ = ImportPhase::Idle;
  QString importingId_;
  QString importCollectionName_;
  int importPercent_ = 0;
  QString importError_;
  /** Types de source absents de CE poste, releves a l'import. */
  QStringList importMissingTypes_;
  /** Fil de dezippage. Meme regle que l'empaquetage : jamais sur le fil d'OBS. */
  QThread* importWorker_ = nullptr;

  qint64 quotaBytes_ = -1;
  qint64 usedBytes_ = -1;
  /** Part de `usedBytes_` retenue par un envoi non confirme. Affichee a part. */
  qint64 reservedBytes_ = 0;
  QVector<RemoteBackup> backups_;

  /**
   * Fil d'empaquetage en cours. Un seul a la fois : la file est en serie.
   *
   * Attendu et detruit au destructeur — un fil qui ecrit dans un objet detruit
   * est un plantage a la fermeture d'OBS, et ce genre de plantage n'accuse jamais
   * sa cause.
   */
  QThread* worker_ = nullptr;

  /** Paquet en cours, dans le dossier temporaire. Efface des qu'il est inutile. */
  QString pendingPackagePath_;
  /**
   * Reservation ouverte cote serveur pour la collection en cours.
   *
   * Conservee pour pouvoir l'ANNULER : une reservation abandonnee compte dans le
   * quota du client jusqu'a sa peremption, et il lirait alors un espace occupe ne
   * correspondant a aucune sauvegarde visible.
   */
  QString currentBackupId_;
};

}  // namespace semirprod
