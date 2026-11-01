#pragma once

#include <QString>
#include <QStringList>

namespace semirprod {

/**
 * Importe un paquet dans OBS : les medias sur le disque, la collection dans la
 * liste des collections de scenes.
 *
 * ─── Importer, et pas telecharger ──────────────────────────────────────────
 *
 * Rendre un ZIP a l'operateur lui laisse tout le travail : trouver le dossier
 * des collections, y copier un JSON, retrouver ses medias, corriger un par un
 * les chemins qui pointaient l'ancienne machine. Personne ne fait ca. L'import
 * fait les trois.
 *
 * ─── Ce qu'il ne fait JAMAIS ───────────────────────────────────────────────
 *
 * - **Il n'ecrase aucune collection existante.** Un nom deja pris devient
 *   « <nom> (importée) », puis « (importée 2) ». Ecraser prendrait la seule copie
 *   d'un habillage en cours de travail, et un import est justement le geste
 *   qu'on fait quand on n'est pas sur de soi.
 * - **Il ne bascule pas OBS dedans.** La collection apparait, l'operateur y va
 *   quand il veut. Basculer emporterait la scene a l'antenne.
 * - **Il ne touche a rien pendant une diffusion ou un enregistrement.** Ecrire
 *   des centaines de megaoctets sur le disque qui enregistre le direct produit
 *   des images sautees.
 * - **Il n'execute rien.** Le paquet ne contient aucun script (ils sont exclus a
 *   la sauvegarde), et le lecteur d'archive refuse tout chemin qui sortirait du
 *   dossier d'import.
 *
 * ─── La limite a dire a l'operateur ────────────────────────────────────────
 *
 * OBS construit sa liste de collections au demarrage. Ecrire le fichier ne le
 * fait donc pas apparaitre tout de suite : il faut relancer OBS, et l'interface
 * doit le dire. Aucune API frontend ne permet d'ajouter une collection a chaud —
 * verifie sur l'API 32.x.
 */
class CollectionImporter {
public:
  /** Ce qu'il faut savoir d'OBS pour importer, releve sur le fil principal. */
  struct Job {
    bool ok = false;
    QString error;

    /** Le ZIP deja telecharge et verifie. */
    QString packagePath;
    /** Nom souhaite, tel que le serveur l'a stocke. */
    QString wantedName;
    /** Nom REELLEMENT libre, calcule ici pour ne rien ecraser. */
    QString finalName;
    /** Dossier `basic/scenes` d'OBS, tel que l'API frontend le donne. */
    QString scenesDir;
    /** Dossier ou deposer les medias importes. */
    QString mediaDir;
    /**
     * Sauvegarde creee par le compte qui l'importe, SELON LE SERVEUR
     * (BackupService::RemoteBackup::ownBackup), jamais selon le paquet, qui se
     * fabrique. Faux par defaut : raccourcis clavier, projecteurs et fichiers
     * medias ne sont repris que s'il est vrai.
     */
    bool ownBackup = false;
  };

  struct Result {
    bool ok = false;
    QString error;

    QString collectionName;
    /** Chemin du fichier de collection ecrit, pour le journal. */
    QString collectionFile;
    int mediaCount = 0;
    qint64 mediaBytes = 0;
    /**
     * Types de source que CE poste ne sait pas rendre.
     *
     * Recalcules a l'import, pas repris du manifeste : ce qui compte est ce qui
     * manque ICI, sur la machine de destination, pas ce qui manquait sur celle
     * d'origine.
     */
    QStringList missingSourceTypes;
    /** Sources navigateur dont l'URL a ete retiree : « nom (hote) ». */
    QStringList droppedBrowserUrls;
    /** Raccourcis clavier et projecteurs non repris (provenance non confirmee). */
    bool hotkeysDropped = false;
    /** Sauvegarde d'un autre compte : fichiers medias du paquet non importes. */
    int mediaDroppedCount = 0;
    /** Propre sauvegarde : medias du paquet ecartes a l'extraction (contenu). */
    int mediaRejectedCount = 0;
    /**
     * Sources (et filtres) restees sans fichier, a completer a la main : au
     * plus 50 noms de 80 caracteres, le reste compte a part. Dans les deux cas
     * (autre compte, ou media ecarte de la propre sauvegarde).
     */
    QStringList emptiedMediaSources;
    int emptiedMediaSourcesMore = 0;
  };

  /**
   * Prepare l'import : dossiers, nom libre, refus si une sortie tourne.
   *
   * ⚠️ FIL PRINCIPAL : appelle l'API frontend d'OBS.
   */
  static Job prepare(const QString& packagePath, const QString& wantedName);

  /**
   * Fait le travail : dezippe, reecrit les chemins, ecrit la collection.
   *
   * ⚠️ NE TOUCHE PAS a l'API d'OBS : c'est ce qui permet de l'executer sur un
   * fil de travail pendant qu'OBS continue de rendre ses images. Les types de
   * source disponibles arrivent par le `Job`.
   */
  static Result run(const Job& job, const QStringList& availableSourceTypes);

  /**
   * Fait APPARAITRE la collection importee dans OBS, sans redemarrage.
   *
   * ⚠️ FIL PRINCIPAL : appelle l'API frontend d'OBS et fait changer de
   * collection.
   *
   * ─── Le probleme que ceci resout ──────────────────────────────────────
   *
   * OBS ne relit JAMAIS son dossier de collections apres le demarrage. Il
   * construit sa liste une fois, au lancement, et ne la reconstruit que sur
   * ses propres gestes (nouvelle, renommer, supprimer, son propre import). Un
   * fichier depose a cote par un tiers reste donc invisible jusqu'au
   * redemarrage suivant, et `obs_frontend_set_current_scene_collection` ne le
   * trouve pas non plus : elle cherche dans la liste en memoire, et un nom
   * absent ne bascule sur rien, en silence.
   *
   * Mesure du 31/08/2026 sur une instance : les fichiers etaient bien ecrits
   * (« Taswar.json », « azaz.json »), l'etat publie par le plugin ne listait
   * toujours que « Sans nom ». Le client cliquait, rien ne se passait, il
   * recommencait.
   *
   * ─── La sortie : laisser OBS creer la collection, et ecrire dessous ───
   *
   * 1. `obs_frontend_add_scene_collection(nom)` : c'est OBS qui cree le
   *    fichier, l'ajoute a sa liste et a son menu, et bascule dessus. C'est
   *    la seule facon d'entrer dans cette liste sans redemarrer.
   * 2. On relit le nom de fichier qu'OBS a choisi (`SceneCollectionFile`) : il
   *    n'a aucune raison d'etre celui qu'on aurait forme nous-memes, et taper
   *    a cote laisserait une collection vide a l'ecran.
   * 3. On rebascule sur la collection de depart. L'ORDRE EST TOUT : ce
   *    changement fait ecrire a OBS la collection vide qu'il vient de creer.
   *    Ecraser le fichier avant ce moment-la, c'est le voir efface aussitot.
   * 4. Le contenu importe remplace ce fichier.
   * 5. On ne bascule PAS sur la collection importee : elle est dans la
   *    liste, et l'operateur l'ouvre quand il veut (Studio : bouton
   *    « Ouvrir », SemirprodDock::openImportedCollection). OBS la relira
   *    alors depuis le disque.
   *
   * Rend faux et remplit `error` si OBS refuse la creation.
   */
  static bool publishToObs(const QString& collectionName,
                           const QString& importedFile,
                           const QString& scenesDir,
                           QString* error);
};

}  // namespace semirprod
