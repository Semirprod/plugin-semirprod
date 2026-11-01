#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace semirprod {

/**
 * Met une collection de scenes OBS en PAQUET : le fichier de collection, ses
 * medias, et un manifeste.
 *
 * ─── Le probleme que ce fichier resout ─────────────────────────────────────
 *
 * OBS enregistre dans une collection des CHEMINS de fichiers, pas leur contenu.
 * Sauvegarder le seul JSON de collection — ce que faisait la premiere version du
 * contrat — donne donc, sur un second PC, un OBS rempli de sources rouges :
 * l'habillage, les jingles et les boucles video ne sont pas la. Une sauvegarde
 * qui ne restaure pas le studio ne sert a rien.
 *
 * ─── Ce qui entre dans le paquet, et ce qui n'y entre pas ──────────────────
 *
 * Entrent : les fichiers references par les sources et leurs filtres, quand leur
 * extension est dans `kAllowedExtensions` — images, videos, sons, polices, et les
 * fichiers locaux d'une source navigateur (l'habillage HTML/CSS/JS du client, que
 * son OBS execute deja chez lui).
 *
 * N'entrent PAS, et c'est une decision produit, pas un oubli :
 *
 *   - les scripts Lua et Python (`modules.scripts-tool`). Ils sont retires du
 *     JSON empaquete ET listes dans le manifeste. Deployer un paquet ecrirait
 *     sinon du code arbitraire sur une autre machine, ou il s'executerait au
 *     chargement de la collection ;
 *   - tout ce dont l'extension n'est pas dans l'allowlist. Un `.exe`, un `.dll`
 *     ou un `.bat` reference par une source n'a rien a faire dans un fichier
 *     qu'on redistribue ;
 *   - les fichiers absents du disque au moment de la sauvegarde. Ils sont listes,
 *     pour que le client sache ce qu'il devra remettre.
 *
 * ─── Les chemins du JSON empaquete sont RELATIFS ───────────────────────────
 *
 * Chaque chemin embarque devient `media/<empreinte courte>-<nom>`. Le chemin
 * d'origine reste dans le manifeste, uniquement pour l'afficher au client. Le
 * deploiement (lot suivant) reecrira ces chemins relatifs vers le dossier ou il
 * aura depose les medias : c'est ce qui rend un paquet portable d'une machine a
 * l'autre, ce que des chemins absolus `C:\\Users\\...` ne seront jamais.
 *
 * ─── Ce que le paquet n'est PAS ────────────────────────────────────────────
 *
 * Ni une image du disque, ni une sauvegarde de profil OBS (encodeurs, debits,
 * clefs de stream). Une clef de stream ne doit jamais quitter la machine par ce
 * chemin, et le profil n'est pas dans le fichier de collection.
 */
class CollectionPackager {
public:
  /** Un media embarque, tel que le manifeste le decrit. */
  struct MediaEntry {
    QString originalPath;
    QString packagedPath;  ///< Toujours sous `media/`, toujours relatif.
    qint64 sizeBytes = 0;
    QString sha256;
  };

  /** Motifs d'ecart. Les memes chaines que la base de donnees attend. */
  struct SkippedEntry {
    QString originalPath;
    QString reason;  ///< `script` | `missing` | `rejected` | `quota`
    qint64 sizeBytes = -1;
  };

  /**
   * Ce qu'il faut savoir d'OBS pour empaqueter, releve UNE FOIS sur le fil
   * principal.
   *
   * ─── Pourquoi cette separation existe ──────────────────────────────────
   *
   * L'empaquetage lit des centaines de megaoctets et calcule autant
   * d'empreintes. Le faire sur le fil principal d'OBS gele toute l'interface le
   * temps que ca dure — l'operateur voit un logiciel qui ne repond plus, et
   * Windows finit par le declarer bloque. C'est ce qui s'est produit le
   * 20/08/2026.
   *
   * L'API d'OBS, elle, n'est pas appelable depuis n'importe quel fil. On la
   * consulte donc ICI, en une passe rapide (`prepare`), et le travail lourd
   * (`run`) ne touche plus qu'au disque et a des donnees deja copiees.
   */
  struct Job {
    bool ok = false;
    QString error;

    QString collectionName;
    /** Chemin du fichier de collection, deja resolu et verifie. */
    QString collectionFile;
    QString obsVersion;
    /**
     * Types de source qu'OBS SAIT rendre sur ce poste.
     *
     * Sert a reperer ce qui vient d'un plugin tiers : une source dont le type
     * n'est pas la ne s'affichera pas sur la machine de destination, meme avec
     * tous ses medias. Releve ici parce que l'enumeration passe par libobs.
     */
    QStringList availableSourceTypes;
  };

  struct Result {
    bool ok = false;
    QString error;

    QString collectionName;
    QString obsVersion;
    QString packagePath;  ///< Le ZIP, dans le dossier temporaire du systeme.
    qint64 sizeBytes = 0;
    QString sha256;
    qint64 mediaBytes = 0;
    QVector<MediaEntry> media;
    QVector<SkippedEntry> skipped;

    /** Tous les types de source employes par la collection, dedupliques. */
    QStringList sourceTypes;
    /**
     * Ceux qu'OBS ne sait PAS rendre sur ce poste — donc fournis par un plugin
     * tiers absent, ou retires depuis.
     *
     * N'empeche RIEN : le paquet se fait quand meme, et c'est voulu. Une
     * collection qui reference une source d'un plugin tiers reste parfaitement
     * sauvegardable, et refuser reviendrait a exiger que la machine de
     * sauvegarde soit deja la machine de destination. C'est une information a
     * afficher, pas un motif de refus.
     */
    QStringList unknownSourceTypes;

    /** Manifeste pret a poster, forme `{version, media[], skipped[]}`. */
    QJsonObject manifest() const;
  };

  /**
   * Nombre maximal d'entrees dans un paquet.
   *
   * Deux raisons, et les deux sont dures : le catalogue central d'un ZIP sans
   * ZIP64 compte les entrees sur 16 bits, et le serveur refuse un manifeste de
   * plus de 2000 entrees. Au-dela, le paquet est refuse avec un message, jamais
   * tronque en silence — une sauvegarde amputee sans le dire est pire qu'une
   * sauvegarde refusee.
   */
  static constexpr int kMaxEntries = 2000;

  /** Extensions autorisees dans `media/`, en minuscules, sans le point. */
  static const QStringList& allowedExtensions();

  /** Clefs de reglages qui portent un chemin de fichier (partagees avec l'import). */
  static const QStringList& settingsPathKeys();
  /** Clefs qui portent une liste de chemins `[{ "value": ... }]`. */
  static const QStringList& settingsPathListKeys();

  /**
   * Type sous lequel OBS CREERA l'objet (source, filtre, transition) : libobs
   * (obs_load_source_type) prend `versioned_id` s'il est renseigne, et `id`
   * seulement sinon. Filtrer sur `id` seul laissait passer un paquet fabrique
   * `"id":"color_source","versioned_id":"browser_source"`.
   *
   * `coherent` recoit faux si `versioned_id` n'est ni `id` ni `id_v<N>` : OBS
   * n'ecrit jamais un tel couple, il ne peut venir que d'un paquet fabrique
   * pour tromper un filtre. Type rendu vide si les deux champs le sont.
   */
  static QString effectiveSourceType(const QJsonObject& object, bool* coherent = nullptr);

  /**
   * Retire de `collection.modules` les donnees que l'import ne laisse pas
   * entrer, et rend le nombre de modules retires.
   *
   * Toujours gardes : les notres (`semirprod*`). Avec `keepNativeObs`, aussi
   * les modules natifs d'OBS (liste fermee : selecteur automatique de scene,
   * minuteur de sortie, sous-titres). Tout le reste part, `scripts-tool` (que
   * OBS executerait a l'ouverture de la collection) comme les donnees des
   * plugins tiers, dont certains declenchent des actions (Advanced Scene
   * Switcher lance des programmes et ecrit des fichiers).
   *
   * UNE regle pour les deux editions : l'import public garde les modules
   * natifs, l'instance cloud ne garde que les notres
   * (src/cloud/CloudImportFilter.cpp).
   */
  static int filterImportedModules(QJsonObject& collection, bool keepNativeObs);

  /**
   * Empaquete la collection ACTIVE.
   *
   * Demande d'abord a OBS d'ecrire son etat sur le disque
   * (`obs_frontend_save`) : sans cela, le fichier de collection lu ici serait
   * celui du dernier enregistrement automatique, donc l'habillage d'il y a
   * peut-etre une heure. C'est le genre d'ecart qu'on ne decouvre qu'a la
   * restauration.
   *
   * A appeler depuis le fil principal : l'API frontend d'OBS l'exige.
   */
  static Result packageCurrentCollection();

  /**
   * Releve ce qu'il faut savoir d'OBS pour empaqueter cette collection.
   *
   * ⚠️ FIL PRINCIPAL OBLIGATOIRE : appelle l'API frontend. Rapide (une lecture
   * de liste, un `obs_frontend_save` quand la collection est celle qui est
   * ouverte), donc sans gel perceptible.
   */
  static Job prepare(const QString& collectionName);

  /**
   * Fait le travail lourd : lecture, parcours, empreintes, ZIP.
   *
   * ⚠️ NE TOUCHE PLUS A L'API D'OBS, et ne doit jamais y toucher : c'est ce qui
   * permet de l'executer sur un autre fil pendant qu'OBS continue de rendre ses
   * images. Tout ce dont elle a besoin est dans le `Job`.
   */
  static Result run(const Job& job);

  /**
   * Empaquete la collection PORTANT CE NOM, active ou non.
   *
   * ⚠️ Le nom est verifie contre `obs_frontend_get_scene_collections()` avant
   * toute lecture de disque. C'est la garde qui compte : l'interface locale peut
   * demander une collection, elle ne peut pas designer un FICHIER. Sans cette
   * verification, quiconque parle au pont local ferait lire, empaqueter et
   * monter n'importe quel fichier de la machine.
   *
   * Aucun changement de collection dans OBS : on lit le fichier de la collection
   * demandee. Basculer pour sauvegarder detruirait le travail en cours de
   * l'operateur, et serait refuse en direct de toute facon.
   *
   * Consequence a connaitre : pour une collection NON active, l'etat sur le
   * disque est celui de la derniere fois qu'OBS l'a quittee — c'est exactement
   * ce qu'OBS lui-meme rechargerait. Seule la collection active est d'abord
   * poussee sur le disque (`obs_frontend_save`).
   */
  static Result packageCollection(const QString& collectionName);

  /** Les collections qu'OBS connait. Rien d'autre n'est empaquetable. */
  static QStringList availableCollections();

  /**
   * Les types de source qu'OBS sait rendre sur ce poste.
   *
   * ⚠️ FIL PRINCIPAL : enumeration libobs. Sert dans les deux sens — a la
   * sauvegarde pour signaler ce qui vient d'un plugin tiers, a l'import pour
   * dire ce qui manquera ICI.
   */
  static QStringList availableSourceTypes();

  /** Le nom de la collection actuellement chargee dans OBS. */
  static QString currentCollectionName();

  /**
   * Le dossier `basic/scenes` d'OBS, deduit du chemin de profil courant.
   *
   * Expose parce que l'IMPORT y ecrit : sans ca, il faudrait redecouvrir le
   * dossier ailleurs, avec une seconde regle qui finirait par diverger — et une
   * divergence ici veut dire « collection importee invisible dans OBS ».
   */
  static QString scenesDirectoryPublic() { return scenesDirectory(); }

private:
  static QString scenesDirectory();
  static QString collectionFileFor(const QString& collectionName, QString* errorOut);
};

}  // namespace semirprod
