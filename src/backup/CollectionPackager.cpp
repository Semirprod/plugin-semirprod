#include "CollectionPackager.hpp"

#include "ZipWriter.hpp"
#include "../SecretKeys.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <obs.h>
#include <util/platform.h>

#include <QCryptographicHash>
#include <QDir>
#include <QHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QStandardPaths>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>
#include "../Locale.hpp"

namespace semirprod {
namespace {

/**
 * Clefs de reglages qui portent un CHEMIN de fichier, dans les sources comme
 * dans les filtres.
 *
 * Liste explicite plutot que « toute chaine qui ressemble a un chemin » : un
 * champ de texte libre contient volontiers `C:\\` sans designer un fichier, et
 * embarquer au hasard ferait monter des documents du client dans un paquet qu'il
 * partagera.
 */
const QStringList& pathKeys() {
  static const QStringList keys = {
      QStringLiteral("file"),        // image_source, ffmpeg_source (media local)
      QStringLiteral("local_file"),  // browser_source, ffmpeg_source
      QStringLiteral("text_file"),   // text_gdiplus / text_ft2 lus depuis un fichier
      QStringLiteral("image_path"),  // filtre de masque / fondu par image
      QStringLiteral("path"),        // stinger (video), certaines sources tierces
      QStringLiteral("track_matte_path"),  // stinger : video du cache de piste
  };
  return keys;
}

/**
 * Clefs qui portent une LISTE de chemins, sous la forme `[{ "value": "..." }]`.
 * C'est la forme d'OBS pour la source diaporama et la playlist VLC.
 */
const QStringList& pathListKeys() {
  static const QStringList keys = {
      QStringLiteral("playlist"),
      QStringLiteral("files"),
  };
  return keys;
}

QString shortHash(const QString& text) {
  return QString::fromUtf8(
             QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha256).toHex())
      .left(10);
}

/**
 * Retire, recursivement, toute clef dont le nom ressemble a un secret
 * (isSecretLookingKey, src/SecretKeys.hpp : la meme regle que le masquage du
 * journal), quelle que soit la forme de sa valeur. Pour les modules de
 * plugins tiers A L'EXPORT : la sauvegarde de l'operateur garde leur
 * configuration, sans leurs identifiants. Rend le nombre de clefs retirees.
 */
int stripSecretLookingKeys(QJsonValue& value) {
  const auto sensible = [](const QString& cle) { return isSecretLookingKey(cle); };
  int removed = 0;
  if (value.isArray()) {
    QJsonArray array = value.toArray();
    for (int i = 0; i < array.size(); ++i) {
      QJsonValue item = array.at(i);
      removed += stripSecretLookingKeys(item);
      array[i] = item;
    }
    value = array;
  } else if (value.isObject()) {
    QJsonObject object = value.toObject();
    for (const QString& key : object.keys()) {
      if (sensible(key)) {
        object.remove(key);
        ++removed;
        continue;
      }
      QJsonValue child = object.value(key);
      if (child.isObject() || child.isArray()) {
        removed += stripSecretLookingKeys(child);
        object[key] = child;
      }
    }
    value = object;
  }
  return removed;
}

/**
 * Retire, recursivement, les clefs qui portent une destination de diffusion ou
 * un secret. Applique aux modules `semirprod*` d'une collection importee ou
 * exportee : ils
 * sont gardes tels quels pour le reste, mais un paquet venu d'ailleurs ne doit
 * pas pouvoir poser une destination (serveur + cle) que la sortie verticale ou
 * multistream utiliserait au prochain direct. Rend le nombre de clefs retirees.
 */
int stripStreamSecrets(QJsonValue& value) {
  static const QStringList kSecretKeys = {
      QStringLiteral("key"),         QStringLiteral("streamkey"),
      QStringLiteral("stream_key"),  QStringLiteral("server"),
      QStringLiteral("rtmpurl"),     QStringLiteral("rtmp_url"),
      QStringLiteral("url"),         QStringLiteral("service"),
      QStringLiteral("destination"), QStringLiteral("destinations"),
      QStringLiteral("token"),       QStringLiteral("password"),
  };
  int removed = 0;
  if (value.isArray()) {
    QJsonArray array = value.toArray();
    for (int i = 0; i < array.size(); ++i) {
      QJsonValue item = array.at(i);
      removed += stripStreamSecrets(item);
      array[i] = item;
    }
    value = array;
  } else if (value.isObject()) {
    QJsonObject object = value.toObject();
    for (const QString& key : object.keys()) {
      if (kSecretKeys.contains(key.toLower())) {
        object.remove(key);
        ++removed;
        continue;
      }
      QJsonValue child = object.value(key);
      if (child.isObject() || child.isArray()) {
        removed += stripStreamSecrets(child);
        object[key] = child;
      }
    }
    value = object;
  }
  return removed;
}

/**
 * URL d'une source (navigateur, surtout) vers un service d'alertes tiers :
 * les parametres qui portent un secret sont retires du paquet EXPORTE. Le
 * paquet part sur le compte et se redistribue ; un jeton StreamElements ou
 * Streamlabs dedans permet de declencher des alertes sur la chaine.
 *
 * Ces services mettent souvent le jeton dans le CHEMIN (`/alert-box/v3/<jeton>`) :
 * celui-la n'est pas retire (la source serait inutilisable a la restauration),
 * mais il est signale dans le journal. Rend l'URL inchangee si rien n'est retire.
 */
QString scrubThirdPartyUrl(const QString& raw) {
  const QUrl url(raw);
  if (!url.isValid()) return raw;
  const QString host = url.host().toLower();
  static const QStringList kHosts = {QStringLiteral("streamelements.com"),
                                     QStringLiteral("streamlabs.com")};
  bool thirdParty = false;
  for (const QString& h : kHosts) {
    if (host == h || host.endsWith(QLatin1Char('.') + h)) thirdParty = true;
  }
  if (!thirdParty) return raw;

  for (const QString& segment : url.path().split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
    if (segment.size() >= 32) {
      blog(LOG_WARNING,
           "[semirprod-backup] source vers %s : jeton probable dans le chemin de l'URL, "
           "conserve dans la sauvegarde",
           host.toUtf8().constData());
      break;
    }
  }

  static const QStringList kSecretParams = {
      QStringLiteral("token"),  QStringLiteral("key"),          QStringLiteral("apikey"),
      QStringLiteral("api_key"), QStringLiteral("jwt"),         QStringLiteral("access_token"),
      QStringLiteral("secret"),
  };
  QUrlQuery query(url);
  bool changed = false;
  const auto items = query.queryItems();
  for (const auto& item : items) {
    if (kSecretParams.contains(item.first.toLower())) {
      query.removeAllQueryItems(item.first);
      changed = true;
    }
  }
  if (!changed) return raw;
  blog(LOG_INFO, "[semirprod-backup] source vers %s : jeton retire de l'URL exportee",
       host.toUtf8().constData());
  QUrl cleaned(url);
  cleaned.setQuery(query);
  return cleaned.toString();
}

QString sha256OfFile(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return {};
  QCryptographicHash hash(QCryptographicHash::Sha256);
  if (!hash.addData(&file)) return {};
  return QString::fromUtf8(hash.result().toHex());
}

}  // namespace

const QStringList& CollectionPackager::settingsPathKeys() {
  return pathKeys();
}

const QStringList& CollectionPackager::settingsPathListKeys() {
  return pathListKeys();
}

QString CollectionPackager::effectiveSourceType(const QJsonObject& object, bool* coherent) {
  const QString id = object.value(QStringLiteral("id")).toString();
  const QString versioned = object.value(QStringLiteral("versioned_id")).toString();
  if (coherent) *coherent = true;
  if (versioned.isEmpty() || versioned == id) return id;

  // `<id>_v<chiffres>` : la seule forme qu'OBS ecrit (color_source -> color_source_v3).
  const QString prefix = id + QStringLiteral("_v");
  bool ok = !id.isEmpty() && versioned.startsWith(prefix) && versioned.size() > prefix.size();
  for (int i = prefix.size(); ok && i < versioned.size(); ++i) {
    if (!versioned.at(i).isDigit()) ok = false;
  }
  if (!ok && coherent) *coherent = false;
  return versioned;
}

int CollectionPackager::filterImportedModules(QJsonObject& collection, bool keepNativeObs) {
  // Clefs ecrites par OBS lui-meme dans une collection (frontend : selecteur
  // automatique de scene, minuteur de sortie, sous-titres Windows).
  // `scripts-tool` n'y figure PAS, volontairement.
  static const QStringList kNativeObsModules = {
      QStringLiteral("auto-scene-switcher"),
      QStringLiteral("output-timer"),
      QStringLiteral("captions"),
  };
  if (!collection.contains(QStringLiteral("modules"))) return 0;

  QJsonObject modules = collection.value(QStringLiteral("modules")).toObject();
  int retires = 0;
  int secrets = 0;
  for (const QString& cle : modules.keys()) {
    const bool notre = cle.startsWith(QStringLiteral("semirprod"));
    const bool garde = notre || (keepNativeObs && kNativeObsModules.contains(cle));
    if (!garde) {
      modules.remove(cle);
      ++retires;
      continue;
    }
    if (notre) {
      // Garde, mais sans destination ni cle de diffusion : voir stripStreamSecrets.
      QJsonValue module = modules.value(cle);
      secrets += stripStreamSecrets(module);
      modules[cle] = module;
    } else {
      // Module natif garde, mais NEUTRALISE : rien ne demarre seul a
      // l'ouverture d'une collection importee. Selecteur automatique de scene
      // (`active`), minuteurs de sortie (`autoStartStreamTimer`,
      // `autoStartRecordTimer`), sous-titres (`enabled`) : tout drapeau
      // booleen d'activation ou de demarrage automatique passe a faux.
      QJsonObject module = modules.value(cle).toObject();
      for (const QString& drapeau : module.keys()) {
        const QString d = drapeau.toLower();
        if (module.value(drapeau).isBool() &&
            (d == QStringLiteral("active") || d == QStringLiteral("enabled") ||
             d.startsWith(QStringLiteral("auto")))) {
          module[drapeau] = false;
        }
      }
      modules[cle] = module;
    }
  }
  if (secrets > 0) {
    blog(LOG_INFO,
         "[semirprod-backup] import : %d reglage(s) de destination retire(s) des modules Semirprod",
         secrets);
  }
  collection[QStringLiteral("modules")] = modules;
  return retires;
}

const QStringList& CollectionPackager::allowedExtensions() {
  // Ce que l'habillage d'un streamer contient reellement. Volontairement SANS
  // executables, archives et scripts : un paquet se redistribue, et il ne doit
  // pas pouvoir transporter de code.
  //
  // Ni page web ni script (html, htm, js, css, svg) : une source navigateur sert
  // ses fichiers locaux sous une origine unique (`http://absolute/`), d'ou une
  // page importee pourrait lire n'importe quel autre fichier du poste. L'import
  // vide donc `local_file` des sources navigateur, et ces fichiers n'ont plus
  // de raison d'etre embarques.
  static const QStringList extensions = {
      // Images
      QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
      QStringLiteral("gif"), QStringLiteral("bmp"), QStringLiteral("webp"),
      QStringLiteral("tga"), QStringLiteral("psd"),
      // Video
      QStringLiteral("mp4"), QStringLiteral("mov"), QStringLiteral("mkv"),
      QStringLiteral("webm"), QStringLiteral("avi"), QStringLiteral("flv"),
      QStringLiteral("ts"), QStringLiteral("m4v"),
      // Audio
      QStringLiteral("mp3"), QStringLiteral("wav"), QStringLiteral("ogg"),
      QStringLiteral("flac"), QStringLiteral("aac"), QStringLiteral("m4a"),
      QStringLiteral("opus"),
      // Polices
      QStringLiteral("ttf"), QStringLiteral("otf"), QStringLiteral("woff"),
      QStringLiteral("woff2"),
      // Textes de sources texte, sous-titres
      QStringLiteral("json"), QStringLiteral("txt"),
      QStringLiteral("srt"), QStringLiteral("vtt"),
  };
  return extensions;
}

QJsonObject CollectionPackager::Result::manifest() const {
  QJsonArray mediaArray;
  for (const MediaEntry& entry : media) {
    QJsonObject item;
    item[QStringLiteral("originalPath")] = entry.originalPath;
    item[QStringLiteral("packagedPath")] = entry.packagedPath;
    item[QStringLiteral("sizeBytes")] = static_cast<double>(entry.sizeBytes);
    item[QStringLiteral("sha256")] = entry.sha256;
    mediaArray.append(item);
  }

  QJsonArray skippedArray;
  for (const SkippedEntry& entry : skipped) {
    QJsonObject item;
    item[QStringLiteral("originalPath")] = entry.originalPath;
    item[QStringLiteral("reason")] = entry.reason;
    if (entry.sizeBytes >= 0) item[QStringLiteral("sizeBytes")] = static_cast<double>(entry.sizeBytes);
    skippedArray.append(item);
  }

  QJsonArray types;
  for (const QString& typeId : sourceTypes) types.append(typeId);
  QJsonArray unknownTypes;
  for (const QString& typeId : unknownSourceTypes) unknownTypes.append(typeId);

  QJsonObject manifest;
  manifest[QStringLiteral("version")] = 1;
  manifest[QStringLiteral("media")] = mediaArray;
  manifest[QStringLiteral("skipped")] = skippedArray;
  // Les types de source employes, et ceux que ce poste ne sait pas rendre.
  // C'est ce qui permet a une restauration de dire « il te manque ces plugins »
  // au lieu de laisser l'operateur decouvrir des sources absentes.
  manifest[QStringLiteral("sourceTypes")] = types;
  manifest[QStringLiteral("unknownSourceTypes")] = unknownTypes;
  return manifest;
}

/**
 * Dossier des collections de scenes.
 *
 * Deduit du dossier du PROFIL courant, et non de `os_get_config_path` : en mode
 * portable, OBS range toute sa configuration a cote de son executable, et le
 * chemin de configuration du systeme designerait alors un dossier vide ou celui
 * d'une autre installation. Le chemin du profil, lui, vient d'OBS lui-meme, donc
 * il est juste dans les deux modes.
 *
 * `<...>/basic/profiles/<profil>` a pour frere `<...>/basic/scenes`.
 */
QString CollectionPackager::scenesDirectory() {
  char* profilePath = obs_frontend_get_current_profile_path();
  if (profilePath) {
    const QDir profileDir(QString::fromUtf8(profilePath));
    bfree(profilePath);
    // profiles/<profil> -> remonter deux crans donne `basic`.
    QDir basic(profileDir);
    if (basic.cdUp() && basic.cdUp()) {
      const QString candidate = basic.absoluteFilePath(QStringLiteral("scenes"));
      if (QDir(candidate).exists()) return candidate;
    }
  }

  // Repli : l'emplacement standard. Utile si l'API frontend rend un chemin
  // inattendu sur une version d'OBS qu'on n'a pas eprouvee.
  char configPath[512];
  if (os_get_config_path(configPath, sizeof(configPath), "obs-studio/basic/scenes") > 0) {
    const QString candidate = QString::fromUtf8(configPath);
    if (QDir(candidate).exists()) return candidate;
  }
  return {};
}

/**
 * Fichier de la collection portant ce NOM.
 *
 * OBS derive le nom de fichier du nom de collection en le nettoyant, et la regle
 * de nettoyage a change au fil des versions : la deviner produirait un
 * « fichier introuvable » sur les collections aux noms accentues ou ponctues. On
 * lit donc le champ `name` de chaque fichier du dossier, ce qui est vrai pour
 * toutes les versions.
 */
QString CollectionPackager::collectionFileFor(const QString& collectionName, QString* errorOut) {
  const QString dirPath = scenesDirectory();
  if (dirPath.isEmpty()) {
    if (errorOut) *errorOut = loc("Backup.FolderMissing");
    return {};
  }

  QDir dir(dirPath);
  const QStringList files = dir.entryList({QStringLiteral("*.json")}, QDir::Files);
  for (const QString& fileName : files) {
    QFile file(dir.absoluteFilePath(fileName));
    if (!file.open(QIODevice::ReadOnly)) continue;
    // Lecture complete : un fichier de collection pese des centaines de
    // kilooctets, pas des megaoctets, et le champ `name` peut etre n'importe ou.
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject()) continue;
    if (doc.object().value(QStringLiteral("name")).toString() == collectionName) {
      return dir.absoluteFilePath(fileName);
    }
  }

  if (errorOut) {
    *errorOut = loc("Backup.CollectionFileMissing")
                    .arg(collectionName);
  }
  return {};
}

namespace {

/// Etat du parcours, passe par reference pour ne pas multiplier les parametres.
struct WalkState {
  QVector<CollectionPackager::MediaEntry> media;
  /** Types de source rencontres, dans l'ordre, dedupliques. */
  QStringList sourceTypes;
  QVector<CollectionPackager::SkippedEntry> skipped;
  QHash<QString, QString> byHash;   ///< sha256 -> chemin dans le paquet (dedup)
  QHash<QString, QString> resolved; ///< chemin d'origine -> chemin dans le paquet
  qint64 mediaBytes = 0;
  bool full = false;  ///< plafond d'entrees atteint
};

void noteSkipped(WalkState& state, const QString& path, const QString& reason,
                 qint64 size = -1) {
  // Un meme fichier manquant est reference par dix sources : ne le lister
  // qu'une fois, sinon l'ecran du client affiche dix fois la meme ligne.
  for (const auto& entry : state.skipped) {
    if (entry.originalPath == path && entry.reason == reason) return;
  }
  state.skipped.append({path, reason, size});
}

/**
 * Prend en charge UN chemin, et rend le chemin a ecrire dans le JSON empaquete
 * (vide si le chemin doit rester tel quel).
 */
QString adoptPath(WalkState& state, const QString& originalPath) {
  if (originalPath.isEmpty()) return {};

  // Deja vu : meme reponse, sans relire le disque.
  const auto known = state.resolved.constFind(originalPath);
  if (known != state.resolved.constEnd()) return known.value();

  const QFileInfo info(originalPath);
  // Une URL (`https://...`) n'est pas un fichier local : la laisser telle quelle
  // est le comportement correct, un habillage servi en ligne le restera.
  if (!info.isAbsolute()) return {};

  const QString extension = info.suffix().toLower();
  if (!CollectionPackager::allowedExtensions().contains(extension)) {
    noteSkipped(state, originalPath, QStringLiteral("rejected"));
    return {};
  }
  if (!info.exists() || !info.isFile()) {
    noteSkipped(state, originalPath, QStringLiteral("missing"));
    return {};
  }
  // Lien symbolique : on refuse. Le suivre embarquerait un fichier situe hors de
  // ce que le client croit designer, et c'est exactement le genre de detour qui
  // fait sortir un fichier prive dans un paquet partage.
  if (info.isSymLink()) {
    noteSkipped(state, originalPath, QStringLiteral("rejected"));
    return {};
  }

  if (state.media.size() >= CollectionPackager::kMaxEntries) {
    state.full = true;
    noteSkipped(state, originalPath, QStringLiteral("quota"), info.size());
    return {};
  }

  const QString hash = sha256OfFile(originalPath);
  if (hash.isEmpty()) {
    noteSkipped(state, originalPath, QStringLiteral("missing"));
    return {};
  }

  // Deduplication par CONTENU : la meme image posee dans douze scenes ne monte
  // qu'une fois, et ne consomme le quota qu'une fois.
  const auto sameContent = state.byHash.constFind(hash);
  if (sameContent != state.byHash.constEnd()) {
    state.resolved.insert(originalPath, sameContent.value());
    return sameContent.value();
  }

  // Le nom dans le paquet porte une empreinte du chemin d'origine : deux
  // fichiers de meme nom venant de deux dossiers ne s'ecrasent pas.
  const QString packaged = QStringLiteral("media/%1-%2")
                               .arg(shortHash(originalPath), info.fileName());

  CollectionPackager::MediaEntry entry;
  entry.originalPath = originalPath;
  entry.packagedPath = packaged;
  entry.sizeBytes = info.size();
  entry.sha256 = hash;
  state.media.append(entry);
  state.mediaBytes += entry.sizeBytes;
  state.byHash.insert(hash, packaged);
  state.resolved.insert(originalPath, packaged);
  return packaged;
}

/**
 * Parcours recursif, en REECRIVANT les chemins adoptes.
 *
 * Rend une VALEUR neuve au lieu de muter par reference : les references de
 * Qt sur un noeud JSON (`QJsonValueRef`) sont valides tant que le conteneur
 * parent ne bouge pas, et une ecriture pendant le parcours du meme conteneur est
 * exactement le cas ou cette garantie devient difficile a tenir. Recopier coute
 * une allocation par noeud sur un fichier de quelques centaines de kilooctets,
 * et supprime toute question d'invalidation.
 */
QJsonValue walkValue(WalkState& state, const QJsonValue& value);

QJsonObject walkObject(WalkState& state, const QJsonObject& input) {
  QJsonObject output = input;

  // Une source d'OBS porte son TYPE dans `id`, a cote de `name` et `settings`.
  // Le relever ici sert a reperer ce qui vient d'un plugin tiers : sur la
  // machine de destination, une source dont le type manque ne s'affichera pas,
  // meme avec tous ses medias. On ne refuse rien pour autant — voir
  // `Result::unknownSourceTypes`.
  if ((input.contains(QStringLiteral("id")) || input.contains(QStringLiteral("versioned_id"))) &&
      input.contains(QStringLiteral("settings"))) {
    const QString typeId = CollectionPackager::effectiveSourceType(input);
    if (!typeId.isEmpty() && !state.sourceTypes.contains(typeId)) {
      state.sourceTypes.append(typeId);
    }
  }

  for (const QString& key : input.keys()) {
    const QJsonValue value = input.value(key);

    if (pathKeys().contains(key) && value.isString()) {
      const QString packaged = adoptPath(state, value.toString());
      if (!packaged.isEmpty()) output[key] = packaged;
      continue;
    }

    // URL d'une source vers un service d'alertes tiers : secrets retires.
    if (key == QStringLiteral("url") && value.isString()) {
      const QString scrubbed = scrubThirdPartyUrl(value.toString());
      if (scrubbed != value.toString()) output[key] = scrubbed;
      continue;
    }

    if (pathListKeys().contains(key) && value.isArray()) {
      QJsonArray list = value.toArray();
      for (int i = 0; i < list.size(); ++i) {
        if (!list.at(i).isObject()) continue;
        QJsonObject item = list.at(i).toObject();
        const QString packaged = adoptPath(state, item.value(QStringLiteral("value")).toString());
        if (packaged.isEmpty()) continue;
        item[QStringLiteral("value")] = packaged;
        list[i] = item;
      }
      output[key] = list;
      continue;
    }

    // Tout le reste est parcouru : les filtres vivent dans `filters[]`, les
    // sources dans `sources[]`, et un groupe porte ses propres reglages.
    if (value.isObject() || value.isArray()) output[key] = walkValue(state, value);
  }
  return output;
}

QJsonValue walkValue(WalkState& state, const QJsonValue& value) {
  if (value.isObject()) return walkObject(state, value.toObject());
  if (value.isArray()) {
    QJsonArray input = value.toArray();
    QJsonArray output;
    for (const QJsonValue& item : input) {
      output.append(item.isObject() || item.isArray() ? walkValue(state, item) : item);
    }
    return output;
  }
  return value;
}

}  // namespace

QStringList CollectionPackager::availableCollections() {
  QStringList names;
  char** raw = obs_frontend_get_scene_collections();
  if (!raw) return names;
  for (char** cursor = raw; *cursor; ++cursor) names.append(QString::fromUtf8(*cursor));
  bfree(raw);
  names.removeDuplicates();
  return names;
}

QString CollectionPackager::currentCollectionName() {
  char* raw = obs_frontend_get_current_scene_collection();
  if (!raw) return {};
  const QString name = QString::fromUtf8(raw);
  bfree(raw);
  return name;
}

CollectionPackager::Result CollectionPackager::packageCurrentCollection() {
  const QString active = currentCollectionName();
  if (active.isEmpty()) {
    Result result;
    result.error = loc("Backup.NoActiveCollection");
    return result;
  }
  return packageCollection(active);
}

// Enumeration libobs, donc FIL PRINCIPAL : c'est pour ca qu'elle est faite dans
// `prepare` et transportee dans le `Job`, plutot que consultee pendant le
// travail lourd. L'import s'en sert aussi, pour dire ce qui manque ICI.
QStringList CollectionPackager::availableSourceTypes() {
  QStringList ids;
  const char* id = nullptr;
  for (size_t i = 0; obs_enum_source_types(i, &id); ++i) {
    if (id && *id) ids.append(QString::fromUtf8(id));
  }
  // Les entrees et les filtres sont enumeres separement selon les versions
  // d'OBS ; on prend aussi les types d'entree pour ne pas declarer inconnu ce
  // qui est parfaitement present.
  for (size_t i = 0; obs_enum_input_types(i, &id); ++i) {
    if (id && *id && !ids.contains(QString::fromUtf8(id))) ids.append(QString::fromUtf8(id));
  }
  return ids;
}

CollectionPackager::Job CollectionPackager::prepare(const QString& collectionName) {
  Job job;
  job.collectionName = collectionName;

  // LA garde de cette classe : un nom, verifie contre ce qu'OBS declare. Jamais
  // un chemin, jamais un nom pris au mot. Sans elle, une commande venue du pont
  // local ferait lire et monter n'importe quel fichier de la machine.
  if (collectionName.isEmpty() || !availableCollections().contains(collectionName)) {
    job.error = loc("Backup.UnknownCollectionNamed").arg(collectionName);
    return job;
  }

  job.obsVersion = QString::fromUtf8(obs_get_version_string());
  job.availableSourceTypes = availableSourceTypes();

  // OBS n'ecrit sa collection sur le disque qu'a certains moments (changement de
  // collection, fermeture, enregistrement automatique). Sans cet appel, on
  // sauvegarderait l'etat d'il y a peut-etre une heure — un ecart qu'on ne
  // decouvrirait qu'a la restauration.
  //
  // Uniquement pour la collection ACTIVE : `obs_frontend_save` ecrit celle qui
  // est chargee, et il n'existe aucune API pour forcer l'ecriture d'une autre.
  // Pour les autres, le fichier sur le disque est deja ce qu'OBS rechargerait —
  // il a ete ecrit quand l'operateur les a quittees.
  if (collectionName == currentCollectionName()) obs_frontend_save();

  QString error;
  job.collectionFile = collectionFileFor(collectionName, &error);
  if (job.collectionFile.isEmpty()) {
    job.error = error;
    return job;
  }

  job.ok = true;
  return job;
}

CollectionPackager::Result CollectionPackager::packageCollection(const QString& collectionName) {
  // Chemin synchrone, conserve pour les appels qui n'ont pas de fil de travail a
  // leur disposition. `BackupService` ne l'emploie PAS : il appelle `prepare`
  // puis `run` sur un autre fil, sans quoi OBS gele le temps de l'empaquetage.
  const Job job = prepare(collectionName);
  if (!job.ok) {
    Result result;
    result.collectionName = collectionName;
    result.error = job.error;
    return result;
  }
  return run(job);
}

CollectionPackager::Result CollectionPackager::run(const Job& job) {
  Result result;
  result.collectionName = job.collectionName;
  result.obsVersion = job.obsVersion;

  const QString collectionFile = job.collectionFile;
  QFile source(collectionFile);
  if (!source.open(QIODevice::ReadOnly)) {
    result.error = loc("Backup.CollectionUnreadableDetail").arg(source.errorString());
    return result;
  }
  QJsonParseError parseError{};
  const QJsonDocument doc = QJsonDocument::fromJson(source.readAll(), &parseError);
  source.close();
  if (!doc.isObject()) {
    result.error = loc("Backup.CollectionUnreadable").arg(parseError.errorString());
    return result;
  }

  QJsonObject collection = doc.object();

  // Les scripts sortent du paquet AVANT le parcours : leurs reglages portent
  // parfois des chemins de fichiers, qu'on n'a aucune raison d'embarquer pour un
  // script qui, lui, ne sera pas embarque.
  WalkState state;
  QJsonObject modules = collection.value(QStringLiteral("modules")).toObject();
  if (modules.contains(QStringLiteral("scripts-tool"))) {
    const QJsonArray scripts = modules.value(QStringLiteral("scripts-tool")).toArray();
    for (const QJsonValue& item : scripts) {
      const QString path = item.toObject().value(QStringLiteral("path")).toString();
      if (!path.isEmpty()) noteSkipped(state, path, QStringLiteral("script"));
    }
    modules.remove(QStringLiteral("scripts-tool"));
    collection[QStringLiteral("modules")] = modules;
  }

  // Secrets retires des l'EXPORT : le paquet part sur le compte et peut se
  // redistribuer. Sans ce passage, il emportait les destinations / cles de
  // diffusion des modules Semirprod (sortie verticale, multistream) et les
  // identifiants des modules de plugins tiers.
  //
  // Les modules tiers sont GARDES (c'est la sauvegarde de l'operateur : les
  // retirer perdait leur configuration a la restauration), sans leurs clefs
  // d'allure secrete. C'est a l'IMPORT qu'ils sont ecartes, car un paquet peut
  // venir d'un autre membre (filterImportedModules).
  {
    QJsonObject exportes = collection.value(QStringLiteral("modules")).toObject();
    int secrets = 0;
    for (const QString& cle : exportes.keys()) {
      QJsonValue module = exportes.value(cle);
      secrets += cle.startsWith(QStringLiteral("semirprod")) ? stripStreamSecrets(module)
                                                             : stripSecretLookingKeys(module);
      exportes[cle] = module;
    }
    if (collection.contains(QStringLiteral("modules"))) {
      collection[QStringLiteral("modules")] = exportes;
    }
    if (secrets > 0) {
      blog(LOG_INFO, "[semirprod-backup] export : %d reglage(s) d'allure secrete retire(s)",
           secrets);
    }
  }

  collection = walkObject(state, collection);

  if (state.full) {
    result.error = loc("Backup.TooManyFiles")
                       .arg(kMaxEntries);
    return result;
  }

  // Le paquet est ecrit dans le dossier temporaire du systeme, jamais a cote de
  // la configuration d'OBS : un fichier oublie la serait relu par OBS au
  // demarrage suivant.
  const QString tempPath =
      QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
          .absoluteFilePath(QStringLiteral("semirprod-collection-%1.zip")
                                .arg(QUuid::createUuid().toString(QUuid::Id128)));

  ZipWriter zip(tempPath);
  if (!zip.open()) {
    result.error = zip.lastError();
    return result;
  }

  const QByteArray collectionJson = QJsonDocument(collection).toJson(QJsonDocument::Compact);
  if (!zip.addBytes(QStringLiteral("collection.json"), collectionJson)) {
    result.error = zip.lastError();
    return result;
  }

  result.media = state.media;
  result.skipped = state.skipped;
  result.sourceTypes = state.sourceTypes;
  for (const QString& typeId : state.sourceTypes) {
    // Ce que ce poste ne sait pas rendre viendra d'un plugin tiers, ou d'un
    // plugin retire. On l'AFFICHE, on ne refuse pas : exiger que la machine de
    // sauvegarde soit deja celle de destination n'aurait aucun sens.
    if (!job.availableSourceTypes.contains(typeId)) result.unknownSourceTypes.append(typeId);
  }
  result.mediaBytes = state.mediaBytes;

  const QByteArray manifestJson = QJsonDocument(result.manifest()).toJson(QJsonDocument::Compact);
  if (!zip.addBytes(QStringLiteral("manifest.json"), manifestJson)) {
    result.error = zip.lastError();
    return result;
  }

  for (const MediaEntry& entry : result.media) {
    if (!zip.addFile(entry.packagedPath, entry.originalPath)) {
      result.error = zip.lastError();
      return result;
    }
  }

  if (!zip.finish()) {
    result.error = zip.lastError();
    return result;
  }

  const QFileInfo packageInfo(tempPath);
  result.packagePath = tempPath;
  result.sizeBytes = packageInfo.size();
  result.sha256 = sha256OfFile(tempPath);
  if (result.sha256.isEmpty()) {
    result.error = loc("Backup.HashFailed");
    return result;
  }

  result.ok = true;
  blog(LOG_INFO,
       "[semirprod-backup] paquet prêt : %s, %lld octets, %d médias, %d écartés",
       result.collectionName.toUtf8().constData(),
       static_cast<long long>(result.sizeBytes), static_cast<int>(result.media.size()),
       static_cast<int>(result.skipped.size()));
  return result;
}

}  // namespace semirprod
