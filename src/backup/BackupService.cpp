#include "BackupService.hpp"

#include "../Config.hpp"
#include "../auth/JwtClaims.hpp"
#include "../http/HttpClient.hpp"
#include "CollectionImporter.hpp"
#include "CollectionPackager.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QDateTime>
#include <QDir>
#include <QPointer>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QThread>

#include <functional>
#include <memory>
#include "../Locale.hpp"

namespace semirprod {
namespace {

QString phaseName(BackupService::Phase phase) {
  switch (phase) {
    case BackupService::Phase::Idle: return QStringLiteral("idle");
    case BackupService::Phase::Packaging: return QStringLiteral("packaging");
    case BackupService::Phase::Reserving: return QStringLiteral("reserving");
    case BackupService::Phase::Uploading: return QStringLiteral("uploading");
    case BackupService::Phase::Done: return QStringLiteral("done");
    case BackupService::Phase::Failed: return QStringLiteral("failed");
  }
  return QStringLiteral("idle");
}

QString collectionsUrl() {
  return Config::apiBaseUrl() + QStringLiteral("/api/obs/scene-collections");
}

/**
 * Message pour un refus du serveur, dans SES termes a lui.
 *
 * Les motifs ne s'interchangent pas, c'est la regle du produit : un quota plein
 * appelle une suppression, une absence d'abonnement appelle une offre. Afficher
 * « erreur 409 » laisse le client sans geste a faire.
 */
QString serverMessage(int status, const QJsonObject& body) {
  // Le serveur formule lui-meme ses refus, avec le motif exact et le geste
  // (`describePluginFeatureRefusal` cote site) : on affiche SON texte des qu'il
  // en donne un. Ce `switch` imposait auparavant un texte fixe par code, et
  // disait « aucun abonnement » a un abonne dont seul le palier manquait, ou
  // « paquet altere » a un paquet refuse par l'antivirus.
  //
  // 401 excepte : le texte serveur y est technique (« jeton invalide »), et le
  // geste est local — reconnecter le plugin.
  const QString detail = body.value(QStringLiteral("error")).toString();
  if (status == 401) {
    return loc("Backup.Unrecognized");
  }
  if (!detail.isEmpty()) return detail;
  // Textes de repli, pour un serveur qui ne dit rien.
  switch (status) {
    case 403:
      return loc("Backup.NotAllowed");
    case 409:
      return loc("Backup.StorageFull");
    case 410:
      return loc("Backup.ReservationExpired");
    case 413:
      return loc("Backup.TooLarge");
    case 422:
      return loc("Backup.Mismatch");
    case 426:
      return loc("Backup.UpdatePlugin");
    case 503:
      return loc("Backup.Unavailable");
    default:
      break;
  }
  if (status == 0) return loc("Backup.ServerUnreachable");
  return loc("Backup.ServerRefused").arg(status);
}

}  // namespace

BackupService::BackupService(HttpClient* http, QObject* parent)
    : QObject(parent), http_(http) {}

BackupService::~BackupService() {
  // Un fil d'empaquetage encore vivant ecrirait dans un objet detruit : plantage
  // a la fermeture d'OBS, et ce genre de plantage n'accuse jamais sa cause. On
  // l'attend. Il ne fait que du disque, il finit donc tout seul ; la borne evite
  // seulement de bloquer la fermeture indefiniment si le disque ne repond plus.
  for (QThread** fil : {&worker_, &importWorker_}) {
    if (!*fil) continue;
    (*fil)->wait(30000);
    delete *fil;
    *fil = nullptr;
  }
}

void BackupService::setBearerToken(const QString& token) {
  // Identite du jeton : `sub` et `org_id`. Un jeton illisible comme JWT (forme
  // inattendue) garde une clef fixe : il ne vide pas la liste a chaque
  // renouvellement. Le garde-fou qui ne depend d'aucun claim est le jeton VIDE,
  // pose par le dock a la deconnexion (SemirprodDock::clearLocalBinding) :
  // changer de compte passe toujours par la.
  QString cle;
  if (!token.isEmpty()) {
    const JwtClaims claims = JwtClaims::parse(token);
    cle = claims.subject + QLatin1Char('|') + claims.orgId;
  }
  const bool autreCompte = token.isEmpty() || cle != accountKey_;
  bearerToken_ = token;
  accountKey_ = cle;
  if (!autreCompte) return;
  ++accountEpoch_;
  if (backups_.isEmpty() && quotaBytes_ < 0 && usedBytes_ < 0) return;
  backups_.clear();
  quotaBytes_ = -1;
  usedBytes_ = -1;
  reservedBytes_ = 0;
  emit changed();
}

void BackupService::setInstance(const QString& instanceId, const QString& instanceLabel) {
  instanceId_ = instanceId;
  instanceLabel_ = instanceLabel;
}

void BackupService::setPhase(Phase phase) {
  phase_ = phase;
  emit changed();
}

bool BackupService::busy() const {
  return phase_ == Phase::Packaging || phase_ == Phase::Reserving ||
         phase_ == Phase::Uploading;
}

void BackupService::fail(const QString& message) {
  // Echec de la SALVE, avant qu'elle ne commence (jeton absent, sortie active,
  // rien de valide a traiter). Distinct de l'echec d'une collection, qui laisse
  // la file avancer.
  lastError_ = message;
  blog(LOG_WARNING, "[semirprod-backup] refus : %s", message.toUtf8().constData());
  discardPackage();
  queue_.clear();
  currentCollection_.clear();
  setPhase(Phase::Failed);
}

void BackupService::discardPackage() {
  if (pendingPackagePath_.isEmpty()) return;
  // Le paquet vit dans le dossier temporaire du systeme : le laisser derriere
  // soi ferait grossir le disque du client d'un habillage entier a chaque
  // tentative.
  QFile::remove(pendingPackagePath_);
  pendingPackagePath_.clear();
}

void BackupService::backupCurrentCollection() {
  backupCollections({});
}

void BackupService::backupCollections(const QStringList& collectionNames) {
  if (busy()) {
    // Une salve tourne : ne rien relancer. Deux empaquetages simultanes
    // liraient le meme disque et reserveraient deux fois le quota pour un seul
    // habillage.
    return;
  }
  if (bearerToken_.isEmpty()) {
    fail(loc("Backup.NotConnected"));
    return;
  }
  if (obs_frontend_streaming_active() || obs_frontend_recording_active()) {
    // Refus EXPLICITE, et non une file d'attente : l'empaquetage lit des
    // centaines de mégaoctets sur le disque qui enregistre le direct.
    fail(loc("Backup.DuringStream"));
    return;
  }

  const QStringList known = CollectionPackager::availableCollections();

  // Liste vide = la collection active. C'est le geste par defaut de l'interface,
  // et il ne doit pas demander de choisir pour marcher.
  QStringList wanted = collectionNames;
  if (wanted.isEmpty()) {
    const QString active = CollectionPackager::currentCollectionName();
    if (active.isEmpty()) {
      fail(loc("Backup.NoActiveCollection"));
      return;
    }
    wanted = {active};
  }

  QStringList accepted;
  QVector<FailedCollection> rejected;
  for (const QString& name : wanted) {
    if (accepted.contains(name)) continue;  // deux fois la meme = une fois
    if (!known.contains(name)) {
      // Ecarte et SIGNALE, jamais approche : un nom voisin designerait
      // l'habillage de quelqu'un d'autre.
      rejected.append({name, loc("Backup.UnknownCollection")});
      continue;
    }
    accepted.append(name);
  }

  if (accepted.size() > kMaxBatch) {
    fail(loc("Backup.TooMany")
             .arg(accepted.size())
             .arg(kMaxBatch));
    return;
  }
  if (accepted.isEmpty()) {
    fail(loc("Backup.NoValidCollection"));
    return;
  }

  lastError_.clear();
  failures_ = rejected;
  progressPercent_ = 0;
  queue_ = accepted;
  batchTotal_ = accepted.size();
  batchDone_ = 0;
  startNext();
}

void BackupService::cancelReservation() {
  if (currentBackupId_.isEmpty()) return;
  const QString url =
      collectionsUrl() + QStringLiteral("/") + currentBackupId_;
  const QString id = currentBackupId_;
  currentBackupId_.clear();
  // Envoi « au fil de l'eau » : l'echec est deja affiche a l'operateur, et rater
  // l'annulation ne fait que retarder la liberation jusqu'a la peremption (30
  // minutes). On journalise, on n'insiste pas.
  http_->deleteJson(url, bearerToken_,
                    [id](const HttpResponse& response, const QJsonDocument&) {
                      if (response.ok()) {
                        blog(LOG_INFO,
                             "[semirprod-backup] réservation %s annulée, place rendue",
                             id.toUtf8().constData());
                      } else {
                        blog(LOG_WARNING,
                             "[semirprod-backup] annulation de la réservation %s refusée "
                             "(code %d) — la place se libère à sa péremption",
                             id.toUtf8().constData(), response.statusCode);
                      }
                    });
}

/**
 * Termine la collection en cours et enchaine.
 *
 * `error` vide = reussie. Une collection qui echoue n'arrete PAS la file : sur
 * « tout sauvegarder », un seul habillage casse (media introuvable, paquet trop
 * gros, quota atteint) ne doit pas emporter les neuf autres. Son motif est
 * conserve et affiche a la fin.
 */
void BackupService::finishCurrent(const QString& error) {
  if (!error.isEmpty()) {
    failures_.append({currentCollection_, error});
    lastError_ = error;
    blog(LOG_WARNING, "[semirprod-backup] « %s » échouée : %s",
         currentCollection_.toUtf8().constData(), error.toUtf8().constData());
  }
  discardPackage();
  if (!error.isEmpty()) {
    // La place reservee est rendue TOUT DE SUITE. Le serveur, lui, conserve
    // volontairement la reservation apres un paquet refuse (422) pour qu'un
    // reessai n'en consomme pas une seconde ; c'est donc au client, qui sait
    // qu'il abandonne, de la relacher.
    cancelReservation();
  }
  currentBackupId_.clear();
  ++batchDone_;
  currentCollection_.clear();
  startNext();
}

void BackupService::startNext() {
  if (queue_.isEmpty()) {
    currentCollection_.clear();
    progressPercent_ = failures_.isEmpty() ? 100 : progressPercent_;
    // La salve est finie. `Failed` uniquement si RIEN n'a abouti : une salve de
    // dix collections dont une echoue reste une salve reussie, et l'interface
    // affiche le detail des ecarts.
    const bool rienNAbouti = failures_.size() >= batchTotal_ && batchTotal_ > 0;
    setPhase(rienNAbouti ? Phase::Failed : Phase::Done);
    // La liste et le quota changent tous les deux : on les relit une fois, au
    // lieu de les recalculer localement et risquer d'afficher autre chose que ce
    // que le serveur compte.
    refresh();
    return;
  }

  currentCollection_ = queue_.takeFirst();
  progressPercent_ = 0;
  setPhase(Phase::Packaging);

  // Consultation d'OBS sur CE fil (le principal) : liste des collections,
  // ecriture de la collection active sur le disque, types de source disponibles.
  // Rapide, donc sans gel perceptible.
  const CollectionPackager::Job job = CollectionPackager::prepare(currentCollection_);
  if (!job.ok) {
    finishCurrent(job.error);
    return;
  }

  // Le travail lourd part sur un fil de travail. `QPointer` : si OBS ferme
  // pendant l'empaquetage, le service peut avoir disparu quand le resultat
  // revient, et on ne veut pas ecrire dans un objet mort.
  QPointer<BackupService> self(this);
  worker_ = QThread::create([self, job]() {
    const CollectionPackager::Result package = CollectionPackager::run(job);
    if (!self) return;
    // Retour sur le fil principal : tout ce qui suit (etat, requetes reseau,
    // instantane vers l'interface) y appartient.
    QMetaObject::invokeMethod(
        self, [self, package]() {
          if (self) self->onPackaged(package);
        },
        Qt::QueuedConnection);
  });
  connect(worker_, &QThread::finished, this, [this]() {
    // Le fil se detruit APRES avoir fini, et le pointeur est remis a zero pour
    // que le destructeur n'attende pas un fil deja parti.
    if (worker_) worker_->deleteLater();
    worker_ = nullptr;
  });
  worker_->start();
}

void BackupService::onPackaged(const CollectionPackager::Result& package) {
  if (!package.ok) {
    finishCurrent(package.error);
    return;
  }

  pendingPackagePath_ = package.packagePath;

  QJsonObject payload;
  payload[QStringLiteral("collectionName")] = package.collectionName;
  payload[QStringLiteral("obsVersion")] = package.obsVersion;
  payload[QStringLiteral("sizeBytes")] = static_cast<double>(package.sizeBytes);
  payload[QStringLiteral("mediaBytes")] = static_cast<double>(package.mediaBytes);
  payload[QStringLiteral("mediaCount")] = package.media.size();
  payload[QStringLiteral("sha256")] = package.sha256;
  payload[QStringLiteral("manifest")] = package.manifest();
  if (!instanceId_.isEmpty()) payload[QStringLiteral("instanceId")] = instanceId_;
  if (!instanceLabel_.isEmpty()) payload[QStringLiteral("instanceLabel")] = instanceLabel_;
  // Une collection sauvegardee sans changement ne cree pas de nouvelle version :
  // le serveur compare a la derniere et repond `unchanged`.
  payload[QStringLiteral("dedupe")] = true;

  setPhase(Phase::Reserving);
  // Rejouable : un jeton expire entre l'empaquetage et l'annonce ferait perdre
  // le paquet deja construit, parfois plusieurs centaines de megaoctets.
  auto annoncer = std::make_shared<std::function<void(bool)>>();
  *annoncer = [this, payload, annoncer](bool reprise) {
  http_->postJson(
      collectionsUrl(), payload, bearerToken_,
      [this, annoncer, reprise](const HttpResponse& response, const QJsonDocument& doc) {
        const QJsonObject body = doc.object();
        if (!response.ok()) {
          if (!reprise && rejouerApresRenouvellement(response.statusCode,
                                                    [annoncer]() { (*annoncer)(true); })) {
            return;
          }
          finishCurrent(serverMessage(response.statusCode, body));
          return;
        }
        // Enveloppe `{success, data}` pour `unchanged` (jsonSuccess), a plat pour
        // une reservation : les deux formes sont lues.
        const QJsonObject data = body.value(QStringLiteral("data")).toObject();
        if (body.value(QStringLiteral("unchanged")).toBool() ||
            data.value(QStringLiteral("unchanged")).toBool()) {
          blog(LOG_INFO, "[semirprod-backup] « %s » inchangee depuis la derniere version : rien a envoyer",
               currentCollection_.toUtf8().constData());
          finishCurrent({});
          return;
        }
        const QString id = body.value(QStringLiteral("id")).toString();
        const QString uploadPath = body.value(QStringLiteral("uploadPath")).toString();
        if (id.isEmpty() || uploadPath.isEmpty()) {
          finishCurrent(loc("Backup.IncompleteResponse"));
          return;
        }
        // Le quota renvoye avec la reservation est deja a jour : l'afficher tout
        // de suite evite une seconde requete pour la meme information.
        if (body.contains(QStringLiteral("quotaBytes"))) {
          quotaBytes_ = static_cast<qint64>(body.value(QStringLiteral("quotaBytes")).toDouble());
        }
        // Retenu pour pouvoir annuler : a partir d'ici, tout abandon doit rendre
        // la place au client.
        currentBackupId_ = id;
        uploadPackage(id, uploadPath);
      });
  };
  (*annoncer)(false);
}

void BackupService::uploadPackage(const QString& backupId, const QString& uploadPath) {
  if (pendingPackagePath_.isEmpty()) {
    finishCurrent(loc("Backup.PackageMissing"));
    return;
  }

  // Le chemin de depot vient du serveur, mais l'HOTE reste le notre : on ne
  // concatene jamais une URL complete renvoyee par une reponse. Sinon une
  // reponse alteree ferait deposer l'habillage du client ailleurs.
  const QString url = Config::apiBaseUrl() + uploadPath;

  setPhase(Phase::Uploading);
  auto deposer = std::make_shared<std::function<void(bool)>>();
  *deposer = [this, url, backupId, deposer](bool reprise) {
  http_->putFile(
      url, pendingPackagePath_, QStringLiteral("application/zip"), bearerToken_,
      [this](qint64 sent, qint64 total) {
        const int percent = total > 0 ? static_cast<int>((sent * 100) / total) : 0;
        if (percent == progressPercent_) return;  // pas d'instantane pour rien
        progressPercent_ = percent;
        emit changed();
      },
      [this, backupId, deposer, reprise](const HttpResponse& response, const QJsonDocument& doc) {
        if (!response.ok()) {
          if (!reprise && rejouerApresRenouvellement(response.statusCode,
                                                    [deposer]() { (*deposer)(true); })) {
            return;
          }
          finishCurrent(serverMessage(response.statusCode, doc.object()));
          return;
        }
        blog(LOG_INFO, "[semirprod-backup] sauvegarde %s confirmée",
             backupId.toUtf8().constData());
        finishCurrent({});
      });
  };
  (*deposer)(false);
}

void BackupService::importBackup(const QString& backupId) {
  if (backupId.isEmpty()) return;
  if (importPhase_ == ImportPhase::Downloading || importPhase_ == ImportPhase::Extracting) {
    // Un seul import a la fois : deux dezippages simultanes de plusieurs
    // centaines de megaoctets se disputent le meme disque pour finir plus tard.
    return;
  }
  if (bearerToken_.isEmpty()) {
    importPhase_ = ImportPhase::Failed;
    importError_ = loc("Import.NotConnected");
    emit changed();
    return;
  }

  // Le nom et l'empreinte viennent de la liste DEJA connue : on ne fait pas
  // confiance a un identifiant pour nommer une collection.
  QString collectionName;
  QString sha256;
  qint64 taille = 0;
  bool ownBackup = false;
  for (const RemoteBackup& backup : backups_) {
    if (backup.id != backupId) continue;
    collectionName = backup.collectionName;
    sha256 = backup.sha256;
    taille = backup.sizeBytes;
    ownBackup = backup.ownBackup;
  }
  if (collectionName.isEmpty()) {
    importPhase_ = ImportPhase::Failed;
    importError_ = loc("Import.UnknownBackup");
    emit changed();
    return;
  }

  importPhase_ = ImportPhase::Downloading;
  importingId_ = backupId;
  importCollectionName_ = collectionName;
  importPercent_ = 0;
  importError_.clear();
  importMissingTypes_.clear();
  emit changed();

  const QString url = collectionsUrl() + QStringLiteral("/") + backupId + QStringLiteral("/archive");
  // Plafond : la taille annoncee par la liste, avec une marge. Un serveur qui
  // enverrait davantage n'est plus celui qu'on croit.
  const qint64 plafond = taille > 0 ? taille + 1024 * 1024 : 4LL * 1024 * 1024 * 1024;

  // Provenance (`ownBackup`) relevee pour CE compte : si le compte change
  // pendant le telechargement, le paquet n'est pas importe (setBearerToken).
  const quint64 epoque = accountEpoch_;
  auto telecharger = std::make_shared<std::function<void(bool)>>();
  *telecharger = [this, url, sha256, plafond, collectionName, ownBackup, epoque,
                  telecharger](bool reprise) {
  // Le paquet transite par le dossier temporaire du systeme et n'y reste pas :
  // c'est un intermediaire, pas un livrable. L'operateur n'a rien a y chercher.
  //
  // Nom ALEATOIRE, fichier cree ici (0600 hors Windows), et UN NOUVEAU a
  // chaque tentative : le nom etait deduit de l'identifiant de la sauvegarde,
  // donc previsible, et un autre compte du poste pouvait poser le fichier (ou
  // un lien) avant nous dans un dossier temporaire partage. Reutiliser le nom
  // apres un echec (fichier efface, puis recree avec les droits par defaut)
  // rouvrait la meme fenetre. getToFile rouvre le fichier reserve en
  // troncature avec QIODevice::ExistingOnly : il ne peut plus le recreer.
  QString temporaire;
  {
    QTemporaryFile reserve(
        QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
            .absoluteFilePath(QStringLiteral("semirprod-import-XXXXXX.zip")));
    reserve.setAutoRemove(false);
    if (!reserve.open()) {
      importPhase_ = ImportPhase::Failed;
      importError_ = loc("Zip.WriteFailed").arg(reserve.errorString());
      emit changed();
      return;
    }
    temporaire = reserve.fileName();
  }
  http_->getToFile(
      url, bearerToken_, temporaire, sha256, plafond,
      [this](qint64 done, qint64 total) {
        const int percent = total > 0 ? static_cast<int>((done * 100) / total) : 0;
        if (percent == importPercent_) return;
        importPercent_ = percent;
        emit changed();
      },
      [this, collectionName, ownBackup, epoque, telecharger, reprise,
       temporaire](const HttpResponse& response, const QString& path) {
        if (path.isEmpty()) {
          // Echec, quel qu'il soit (URL refusee localement, reseau, empreinte) :
          // le fichier reserve ne doit pas rester dans le dossier temporaire.
          QFile::remove(temporaire);
          if (!reprise && rejouerApresRenouvellement(response.statusCode,
                                                    [telecharger]() { (*telecharger)(true); })) {
            return;
          }
          importPhase_ = ImportPhase::Failed;
          importError_ = response.errorString.isEmpty()
                             ? serverMessage(response.statusCode, {})
                             : response.errorString;
          emit changed();
          return;
        }
        if (epoque != accountEpoch_) {
          QFile::remove(path);
          importPhase_ = ImportPhase::Failed;
          importError_ = loc("Import.UnknownBackup");
          emit changed();
          return;
        }
        extractDownloaded(path, collectionName, ownBackup);
      });
  };
  (*telecharger)(false);
}

void BackupService::extractDownloaded(const QString& packagePath,
                                      const QString& collectionName, bool ownBackup) {
  // Consultation d'OBS sur CE fil (le principal) : collections existantes pour
  // ne rien ecraser, dossier des scenes, sortie active.
  CollectionImporter::Job job = CollectionImporter::prepare(packagePath, collectionName);
  job.ownBackup = ownBackup;
  if (!job.ok) {
    QFile::remove(packagePath);
    importPhase_ = ImportPhase::Failed;
    importError_ = job.error;
    emit changed();
    return;
  }

  // Les types de source disponibles se relevent aussi ici : l'enumeration passe
  // par libobs, donc pas depuis le fil de travail.
  const QStringList typesDisponibles = CollectionPackager::availableSourceTypes();

  importPhase_ = ImportPhase::Extracting;
  importPercent_ = 100;
  emit changed();

  QPointer<BackupService> self(this);
  importWorker_ = QThread::create([self, job, typesDisponibles, packagePath]() {
    const CollectionImporter::Result resultat = CollectionImporter::run(job, typesDisponibles);
    // Le paquet telecharge a fait son office : le garder ferait grossir le
    // dossier temporaire d'un habillage entier a chaque import.
    QFile::remove(packagePath);
    if (!self) return;
    QMetaObject::invokeMethod(
        self, [self, resultat, job]() {
          if (!self) return;
          if (resultat.ok) {
            // Rapport d'import dans le Journal : ce que l'operateur doit
            // refaire a la main (URL d'overlays retirees, raccourcis).
            for (const QString& source : resultat.droppedBrowserUrls) {
              emit self->notice(QStringLiteral("warn"), loc("Import.UrlRemoved").arg(source));
            }
            if (!resultat.droppedBrowserUrls.isEmpty()) {
              emit self->notice(QStringLiteral("warn"),
                                loc("Import.UrlsRemovedCount")
                                    .arg(resultat.droppedBrowserUrls.size()));
            }
            if (resultat.hotkeysDropped) {
              emit self->notice(QStringLiteral("info"), loc("Import.HotkeysDropped"));
            }
            // Sources restees sans fichier : deja bornees par l'import (50 noms
            // de 80 caracteres), le reste en une ligne.
            for (const QString& source : resultat.emptiedMediaSources) {
              emit self->notice(QStringLiteral("warn"), loc("Import.MediaSourceEmpty").arg(source));
            }
            if (resultat.emptiedMediaSourcesMore > 0) {
              emit self->notice(QStringLiteral("warn"), loc("Import.MediaSourcesMore")
                                                            .arg(resultat.emptiedMediaSourcesMore));
            }
            // Pourquoi elles sont vides : sauvegarde d'un autre compte (aucun
            // fichier repris), ou medias ecartes de la propre sauvegarde.
            if (!job.ownBackup) {
              if (resultat.mediaDroppedCount > 0) {
                emit self->notice(QStringLiteral("warn"),
                                  loc("Import.MediaDropped").arg(resultat.mediaDroppedCount));
              } else if (!resultat.emptiedMediaSources.isEmpty()) {
                emit self->notice(QStringLiteral("warn"), loc("Import.MediaDroppedNoFile"));
              }
            } else if (resultat.mediaRejectedCount > 0) {
              emit self->notice(QStringLiteral("warn"),
                                loc("Import.MediaRejected").arg(resultat.mediaRejectedCount));
            }
            // FAIRE APPARAITRE LA COLLECTION, et pas seulement l'ecrire.
            //
            // Le fil de travail a depose un fichier sur le disque ; OBS ne
            // relit jamais ce dossier apres son demarrage, donc sans l'etape
            // qui suit l'import reussit en restant INVISIBLE, ce qui est pire
            // qu'un echec : le client clique, rien ne bouge, il recommence.
            //
            // Ici on est de retour sur le fil principal (invokeMethod en file
            // d'attente), seul endroit d'ou l'API frontend d'OBS s'appelle.
            QString erreurPublication;
            const bool publiee = CollectionImporter::publishToObs(
                resultat.collectionName, resultat.collectionFile, job.scenesDir,
                &erreurPublication);
            if (!publiee) {
              // Le contenu est sur le disque et n'est pas perdu : il
              // apparaitra au prochain demarrage d'OBS. On le DIT, plutot que
              // d'annoncer une reussite que l'ecran ne montre pas.
              self->importPhase_ = ImportPhase::Failed;
              self->importError_ =
                  erreurPublication +
                  QStringLiteral(" ") + loc("Import.SavedHint");
              emit self->changed();
              return;
            }
            self->importPhase_ = ImportPhase::Done;
            self->importCollectionName_ = resultat.collectionName;
            self->importMissingTypes_ = resultat.missingSourceTypes;
          } else {
            self->importPhase_ = ImportPhase::Failed;
            self->importError_ = resultat.error;
          }
          emit self->changed();
        },
        Qt::QueuedConnection);
  });
  connect(importWorker_, &QThread::finished, this, [this]() {
    if (importWorker_) importWorker_->deleteLater();
    importWorker_ = nullptr;
  });
  importWorker_->start();
}

void BackupService::refresh() {
  if (bearerToken_.isEmpty()) return;
  // Liste demandee pour CE compte : ignoree si le compte a change entre-temps.
  const quint64 epoque = accountEpoch_;
  auto lister = std::make_shared<std::function<void(bool)>>();
  *lister = [this, lister, epoque](bool reprise) {
  http_->getJson(collectionsUrl(), bearerToken_,
                 [this, lister, reprise, epoque](const HttpResponse& response,
                                                 const QJsonDocument& doc) {
                   if (epoque != accountEpoch_) return;
                   if (!response.ok()) {
                     if (!reprise) {
                       rejouerApresRenouvellement(response.statusCode,
                                                  [lister]() { (*lister)(true); });
                     }
                     return;
                   }
                   // Les routes du dashboard enveloppent dans `{success, data}`.
                   const QJsonObject root = doc.object();
                   const QJsonObject data = root.contains(QStringLiteral("data"))
                                                ? root.value(QStringLiteral("data")).toObject()
                                                : root;
                   quotaBytes_ = static_cast<qint64>(data.value(QStringLiteral("quotaBytes")).toDouble(-1));
                   usedBytes_ = static_cast<qint64>(data.value(QStringLiteral("usedBytes")).toDouble(-1));
                   reservedBytes_ =
                       static_cast<qint64>(data.value(QStringLiteral("reservedBytes")).toDouble(0));

                   backups_.clear();
                   for (const QJsonValue& item : data.value(QStringLiteral("items")).toArray()) {
                     const QJsonObject entry = item.toObject();
                     RemoteBackup backup;
                     backup.id = entry.value(QStringLiteral("id")).toString();
                     backup.collectionName = entry.value(QStringLiteral("collectionName")).toString();
                     backup.sizeBytes = static_cast<qint64>(entry.value(QStringLiteral("sizeBytes")).toDouble());
                     backup.mediaCount = entry.value(QStringLiteral("mediaCount")).toInt();
                     backup.createdAt = entry.value(QStringLiteral("createdAt")).toString();
                     backup.instanceLabel = entry.value(QStringLiteral("instanceLabel")).toString();
                     backup.sha256 = entry.value(QStringLiteral("sha256")).toString();
                     backup.skippedCount = entry.value(QStringLiteral("skippedCount")).toInt();
                     // Provenance dite par le SERVEUR (reponse authentifiee),
                     // jamais lue dans le paquet. Champ absent : pas la notre.
                     backup.ownBackup =
                         entry.value(QStringLiteral("createdByCurrentUser")).toBool(false);
                     for (const QJsonValue& type :
                          entry.value(QStringLiteral("unknownSourceTypes")).toArray()) {
                       const QString typeId = type.toString();
                       if (!typeId.isEmpty()) backup.unknownSourceTypes.append(typeId);
                     }
                     backups_.append(backup);
                   }
                   emit changed();
                 });
  };
  (*lister)(false);
}

/**
 * Une expiration de jeton n'est pas une erreur d'utilisateur.
 *
 * Le 401 arrivait jusqu'a l'ecran sous la forme « Compte non reconnu.
 * Reconnectez le plugin a votre compte. » Sur une instance infogeree, ce geste
 * n'existe pas : le jeton vient de l'appairage au demarrage, il n'y a aucun
 * ecran de connexion, et le client n'avait donc AUCUNE issue jusqu'au
 * redemarrage de son OBS.
 *
 * Une seule reprise, et jamais depuis une reprise : si le serveur refuse encore
 * avec un jeton neuf, ce n'est plus une expiration (organisation absente,
 * habilitation retiree) et rejouer en boucle ne ferait que cacher la cause
 * derriere un ecran qui tourne.
 */
bool BackupService::rejouerApresRenouvellement(int statusCode,
                                               std::function<void()> rejouer) {
  if (statusCode != 401) return false;
  if (!renewer_) return false;
  if (renouvellementEnCours_) return false;

  renouvellementEnCours_ = true;
  blog(LOG_INFO, "[semirprod-backup] 401 — renouvellement du jeton puis reprise");
  renewer_([this, rejouer](const QString& frais) {
    renouvellementEnCours_ = false;
    if (frais.isEmpty()) {
      // Le renouvellement a echoue. Le geste doit se CONCLURE : le laisser en
      // attente afficherait un telechargement qui ne finit jamais. On termine
      // celui qui etait en cours, et lui seul — une sauvegarde et un import ont
      // chacun leur etat, les melanger afficherait un echec la ou rien ne
      // tournait.
      blog(LOG_WARNING, "[semirprod-backup] renouvellement refusé — reprise abandonnée");
      const QString motif =
          loc("Backup.SessionExpired");
      if (importPhase_ == ImportPhase::Downloading ||
          importPhase_ == ImportPhase::Extracting) {
        importPhase_ = ImportPhase::Failed;
        importError_ = motif;
        emit changed();
      }
      if (phase_ != Phase::Idle && phase_ != Phase::Failed) fail(motif);
      return;
    }
    setBearerToken(frais);
    rejouer();
  });
  return true;
}

QJsonObject BackupService::toJson() const {
  QJsonArray items;
  for (const RemoteBackup& backup : backups_) {
    QJsonObject entry;
    entry[QStringLiteral("id")] = backup.id;
    entry[QStringLiteral("collectionName")] = backup.collectionName;
    entry[QStringLiteral("sizeBytes")] = static_cast<double>(backup.sizeBytes);
    entry[QStringLiteral("mediaCount")] = backup.mediaCount;
    entry[QStringLiteral("createdAt")] = backup.createdAt;
    entry[QStringLiteral("instanceLabel")] = backup.instanceLabel;
    entry[QStringLiteral("skippedCount")] = backup.skippedCount;
    QJsonArray inconnus;
    for (const QString& typeId : backup.unknownSourceTypes) inconnus.append(typeId);
    entry[QStringLiteral("unknownSourceTypes")] = inconnus;
    items.append(entry);
  }

  QJsonArray failures;
  for (const FailedCollection& failure : failures_) {
    QJsonObject entry;
    entry[QStringLiteral("collectionName")] = failure.collectionName;
    entry[QStringLiteral("error")] = failure.error;
    failures.append(entry);
  }

  // Les collections empaquetables, telles qu'OBS les declare. C'est CETTE liste
  // que l'interface propose : elle ne devine aucun nom, et le plugin refuse de
  // toute facon tout nom qui n'y est pas.
  QJsonArray collections;
  for (const QString& name : CollectionPackager::availableCollections()) {
    collections.append(name);
  }

  QJsonObject state;
  state[QStringLiteral("phase")] = phaseName(phase_);
  state[QStringLiteral("progress")] = progressPercent_;
  state[QStringLiteral("error")] = lastError_;
  state[QStringLiteral("collections")] = collections;
  state[QStringLiteral("activeCollection")] = CollectionPackager::currentCollectionName();
  state[QStringLiteral("currentCollection")] = currentCollection_;
  state[QStringLiteral("batchTotal")] = batchTotal_;
  state[QStringLiteral("batchDone")] = batchDone_;
  state[QStringLiteral("failures")] = failures;
  // `-1` = « pas encore lu », distinct de zero qui serait un quota nul. L'ecran
  // n'affiche la barre d'occupation qu'a partir d'une valeur connue.
  state[QStringLiteral("quotaBytes")] = static_cast<double>(quotaBytes_);
  state[QStringLiteral("usedBytes")] = static_cast<double>(usedBytes_);
  // La part retenue par un envoi non confirme, dite a part : sinon un operateur
  // dont deux envois ont ete coupes lit un espace occupe qui ne correspond a
  // aucune sauvegarde de la liste, et conclut que le compteur est casse.
  state[QStringLiteral("reservedBytes")] = static_cast<double>(reservedBytes_);
  state[QStringLiteral("items")] = items;

  // Import dans OBS : etat SEPARE de la file d'empaquetage. Un geste qui echoue
  // ne doit pas donner l'impression que tout est bloque.
  //
  // Pas de chemin de fichier ici, volontairement : il n'aide a rien decider. Ce
  // que l'operateur doit savoir tient en trois choses — ou on en est, sous quel
  // nom la collection est arrivee, et ce qui lui manquera.
  QJsonObject importState;
  importState[QStringLiteral("phase")] =
      importPhase_ == ImportPhase::Downloading
          ? QStringLiteral("downloading")
          : importPhase_ == ImportPhase::Extracting
                ? QStringLiteral("extracting")
                : importPhase_ == ImportPhase::Done
                      ? QStringLiteral("done")
                      : importPhase_ == ImportPhase::Failed ? QStringLiteral("failed")
                                                            : QStringLiteral("idle");
  importState[QStringLiteral("id")] = importingId_;
  importState[QStringLiteral("collectionName")] = importCollectionName_;
  importState[QStringLiteral("progress")] = importPercent_;
  importState[QStringLiteral("error")] = importError_;
  QJsonArray manquants;
  for (const QString& type : importMissingTypes_) manquants.append(type);
  importState[QStringLiteral("missingSourceTypes")] = manquants;
  state[QStringLiteral("import")] = importState;
  return state;
}

}  // namespace semirprod
