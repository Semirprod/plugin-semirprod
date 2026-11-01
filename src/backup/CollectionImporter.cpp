#include "CollectionImporter.hpp"

#include "CollectionPackager.hpp"
#include "ZipReader.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/config-file.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QUrl>
#include <QScopeGuard>
#include <QSet>
#include <QUuid>
#include "../Config.hpp"
#include "../Edition.hpp"
#include "../Locale.hpp"

namespace semirprod {
namespace {

/** Le JSON de collection est petit ; au-dela, le paquet n'est pas ce qu'il dit. */
constexpr qint64 kMaxCollectionJson = 32 * 1024 * 1024;

/**
 * Nom de fichier qu'OBS donnerait a cette collection.
 *
 * Volontairement plus severe qu'OBS : lettres, chiffres, tiret, souligne. Le nom
 * vient du reseau et devient un nom de fichier ; on ne cherche pas a reproduire
 * exactement la regle d'OBS, seulement a ne jamais ecrire ailleurs que prevu.
 * OBS retrouvera la collection par le champ `name` du fichier, pas par son nom.
 */
QString fichierPourCollection(const QString& nom) {
  QString base;
  for (const QChar& c : nom) {
    if (c.isLetterOrNumber() || c == QLatin1Char('-') || c == QLatin1Char('_')) {
      base.append(c);
    } else if (c.isSpace() && !base.endsWith(QLatin1Char('_'))) {
      base.append(QLatin1Char('_'));
    }
  }
  base = base.left(80);
  return base.isEmpty() ? QStringLiteral("collection_importee") : base;
}

/** Au plus tant de sources nommees dans le Journal, chacune coupee a tant de caracteres. */
constexpr int kMaxListedSources = 50;
constexpr int kMaxListedNameLength = 80;

/**
 * Ce que le CONTENU d'un media extrait ferait faire au lecteur qui l'ouvre,
 * releve une fois a l'extraction (voir scanMediaMarkers). Le verdict depend de
 * la clef qui designe le fichier, donc il est rendu plus tard, a la reecriture
 * (mediaAllowedForKey).
 */
struct MediaMarkers {
  /** Marqueurs qu'un demultiplexeur ffmpeg suit ou qu'il sonde partout : DASH
      (`<MPD`), IMF (`<CompositionPlaylist`), film de reference MOV (`rmra`,
      `rmda`). */
  bool ffmpeg = false;
  /** Les memes, plus une ligne qui commence par une URL ou un chemin UNC :
      ce que le module m3u de VLC prend pour une liste de lecture. */
  bool vlc = false;
};

/**
 * Etat de la reecriture des chemins de medias : UN seul parcours du document.
 *
 * Le type et le nom de l'objet instanciable en cours (source, filtre,
 * transition) sont transmis en descendant : ils valent pour tout ce qu'il
 * contient, jusqu'au prochain objet instanciable. C'est ce qui permet de
 * nommer, sans second parcours, chaque source dont une reference media a ete
 * videe, et de savoir quel lecteur ouvrira un fichier (`file` d'une source
 * image n'est pas `file` d'une source texte).
 */
struct MediaRewrite {
  QDir mediaDir;
  /** Fichiers extraits ET valides par CET import, et leurs marqueurs. */
  QHash<QString, MediaMarkers> valid;
  int rewritten = 0;
  int rejected = 0;
  /** Sources (et filtres) restees sans fichier, bornees a kMaxListedSources. */
  QStringList emptiedSources;
  int emptiedSourcesMore = 0;
  QString currentType;
  QString currentName;
  bool currentRecorded = false;
};

/**
 * Parcours recursif : remplace les chemins `media/...` par leur chemin local,
 * sous les SEULES clefs de media (voir `isMediaKey`).
 */
QJsonValue reecrire(const QJsonValue& valeur, MediaRewrite& w);

bool estInstanciable(const QJsonObject& objet);

/**
 * Vrai si `cle` est une clef qui designe un MEDIA dans une source native d'OBS.
 *
 * Liste FERMEE, la meme que celle de l'empaqueteur (`settingsPathKeys`) : image
 * (`file`), video locale (`local_file` de ffmpeg_source), texte lu depuis un
 * fichier (`text_file`), masque ou LUT (`image_path`), stinger (`path`). Un
 * `media/...` sous une autre clef n'est JAMAIS ramene vers le dossier des
 * medias importes : sinon `plugin_path` d'un filtre VST pointerait sur un
 * fichier du paquet, et le module serait charge (LoadLibraryW et dlopen
 * ignorent l'extension).
 */
bool isMediaKey(const QString& cle) {
  return CollectionPackager::settingsPathKeys().contains(cle);
}

/**
 * Vrai si les premiers octets de `chemin` sont ceux d'un executable ou d'une
 * bibliotheque : PE (`MZ`), ELF (`\x7fELF`), Mach-O 32/64 bits et binaire
 * universel, dans les deux ordres d'octets.
 *
 * L'extension d'un media ne prouve rien : un module renomme en `x.png` reste
 * chargeable. Un fichier illisible est traite comme suspect.
 */
bool startsWithExecutableMagic(const QString& chemin) {
  QFile fichier(chemin);
  if (!fichier.open(QIODevice::ReadOnly)) return true;
  const QByteArray tete = fichier.read(4);
  if (tete.startsWith("MZ")) return true;
  if (tete.size() < 4) return false;
  if (tete == QByteArray("\x7f" "ELF", 4)) return true;
  static const char* const kMachO[] = {
      "\xFE\xED\xFA\xCE", "\xFE\xED\xFA\xCF", "\xCA\xFE\xBA\xBE",  // gros-boutiste
      "\xCE\xFA\xED\xFE", "\xCF\xFA\xED\xFE", "\xBE\xBA\xFE\xCA",  // petit-boutiste
  };
  for (const char* magie : kMachO) {
    if (tete == QByteArray(magie, 4)) return true;
  }
  return false;
}

/**
 * Chemin local d'un media embarque, ou chaine VIDE si `relatif` pourrait
 * designer autre chose qu'un fichier SOUS le dossier des medias importes.
 *
 * LA regle de confinement, et la seule : l'extraction des entrees et la
 * reecriture des chemins du JSON passent toutes deux par ici. Deux gardes :
 *   1. syntaxique (ZipReader::isSafeRelativePath) : ni barre de tete, ni `//`,
 *      ni antislash, ni deux-points, ni `..`. `QDir::absoluteFilePath` rend un
 *      chemin absolu TEL QUEL, donc `media/C:/...` ou `media//Users/...`
 *      ecrivaient n'importe ou ;
 *   2. de confinement : le chemin nettoye doit commencer par le dossier des
 *      medias suivi d'un separateur. Ceinture et bretelles, si la premiere
 *      regle laissait un jour passer une forme qu'on n'a pas prevue.
 */
QString confinedMediaPath(const QString& relatif, const QDir& mediaDir) {
  if (!ZipReader::isSafeRelativePath(relatif)) return {};
  const QString racine = QDir::cleanPath(mediaDir.absolutePath());
  const QString chemin = QDir::cleanPath(mediaDir.absoluteFilePath(relatif));
  if (!chemin.startsWith(racine + QLatin1Char('/'))) return {};
  return chemin;
}

/**
 * Lecteur qui ouvrira le fichier designe par la clef `cle` d'un objet de type
 * `type`. C'est lui, et pas l'extension, qui dit ce que le contenu peut faire :
 *   - Text : sources texte (`text_file` de text_ft2_source, `file` de
 *     text_gdiplus). Le fichier est lu comme du TEXTE et affiche, jamais
 *     interprete : une URL ou un `#EXTM3U` dedans n'est qu'un mot a l'ecran ;
 *   - Image : source image, diaporama, masque, LUT. libobs decode l'image avec
 *     ffmpeg (avformat_open_input SANS format impose, donc par sondage du
 *     contenu) : les demultiplexeurs qui suivent des references et qui sondent
 *     le contenu partout comptent (MediaMarkers::ffmpeg). HLS et ffconcat
 *     exigent leur en-tete en TETE de fichier, ce que la signature d'image
 *     verifiee a l'extraction exclut deja ;
 *   - Ffmpeg : source media, stinger et son cache de piste (media-playback,
 *     ffmpeg). Memes marqueurs que pour une image ;
 *   - Vlc : liste de lecture de vlc_source. VLC sonde en plus des listes M3U a
 *     une ligne qui commence par une URL (MediaMarkers::vlc).
 * Une URL au milieu d'une balise ID3, d'un bloc XMP ou d'un texte n'est donc
 * suivie par aucun de ces lecteurs : elle n'est plus un motif de refus.
 */
enum class MediaReader { None, Text, Image, Ffmpeg, Vlc };

MediaReader readerForKey(const QString& cle, const QString& type) {
  if (cle == QStringLiteral("text_file")) return MediaReader::Text;
  if (cle == QStringLiteral("file")) {
    // `file` d'une source texte GDI+ : du texte. Partout ailleurs (source
    // image) : une image, jamais du texte, qu'un sondage ffmpeg pourrait lire
    // comme une liste de lecture HLS ou ffconcat.
    return type.startsWith(QStringLiteral("text_gdiplus")) ? MediaReader::Text
                                                           : MediaReader::Image;
  }
  if (cle == QStringLiteral("image_path") || cle == QStringLiteral("files")) {
    return MediaReader::Image;
  }
  if (cle == QStringLiteral("playlist")) return MediaReader::Vlc;
  if (cle == QStringLiteral("local_file") || cle == QStringLiteral("path") ||
      cle == QStringLiteral("track_matte_path")) {
    return MediaReader::Ffmpeg;
  }
  return MediaReader::None;
}

/**
 * Vrai si le fichier `chemin` (deja extrait et valide) peut etre designe par
 * la clef `cle` d'un objet de type `type` : extension admise pour CE lecteur
 * (un texte n'atteint jamais ffmpeg ou VLC, une image n'est jamais une video),
 * et aucun marqueur que CE lecteur suivrait. Clef inconnue : rien.
 */
bool mediaAllowedForKey(const QString& cle, const QString& type, const QString& chemin,
                        const MediaMarkers& marqueurs) {
  static const QStringList kImages = {
      QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("gif"),
      QStringLiteral("bmp"), QStringLiteral("webp"), QStringLiteral("tga"), QStringLiteral("psd"),
  };
  static const QStringList kMedia = {
      QStringLiteral("mp4"), QStringLiteral("mov"), QStringLiteral("mkv"), QStringLiteral("webm"),
      QStringLiteral("avi"), QStringLiteral("flv"), QStringLiteral("ts"),  QStringLiteral("m4v"),
      QStringLiteral("mp3"), QStringLiteral("wav"), QStringLiteral("ogg"), QStringLiteral("flac"),
      QStringLiteral("aac"), QStringLiteral("m4a"), QStringLiteral("opus"),
  };
  static const QStringList kText = {QStringLiteral("txt"), QStringLiteral("json"),
                                    QStringLiteral("srt"), QStringLiteral("vtt")};
  const QString e = QFileInfo(chemin).suffix().toLower();
  switch (readerForKey(cle, type)) {
    case MediaReader::Text:
      return kText.contains(e);
    case MediaReader::Image:
      return kImages.contains(e) && !marqueurs.ffmpeg;
    case MediaReader::Ffmpeg:
      return kMedia.contains(e) && !marqueurs.ffmpeg;
    case MediaReader::Vlc:
      return kMedia.contains(e) && !marqueurs.vlc;
    case MediaReader::None:
      break;
  }
  return false;
}

/**
 * Vrai si `text` est un chemin `media/...` du paquet. `localPath` recoit alors
 * son chemin local, ou une chaine VIDE s'il sortirait du dossier des medias.
 * Seuls les fichiers EXTRAITS ET VALIDES par CET import sont designes, et
 * seulement s'ils conviennent au lecteur de la clef (mediaAllowedForKey) : un
 * media ecarte, absent du paquet, un dossier, ou un reste d'un import
 * precedent ne l'est jamais.
 */
bool packagedToLocal(const QString& text, const QString& cle, const MediaRewrite& w,
                     QString* localPath) {
  if (!text.startsWith(QStringLiteral("media/"))) return false;
  *localPath = confinedMediaPath(text.mid(6), w.mediaDir);
  if (localPath->isEmpty()) return true;
  const auto it = w.valid.constFind(*localPath);
  if (it == w.valid.constEnd() || !mediaAllowedForKey(cle, w.currentType, *localPath, it.value())) {
    localPath->clear();
  }
  return true;
}

/**
 * Une reference media videe : comptee, et la source qui la portait nommee
 * une fois dans le rapport (au plus kMaxListedSources noms de
 * kMaxListedNameLength caracteres ; le reste est compte). Le nom vient du
 * paquet tel quel, avant la coupe de assainirNoeud : il est donc coupe ici.
 */
void noteEmptiedMedia(MediaRewrite& w) {
  ++w.rejected;
  if (w.currentName.isEmpty() || w.currentRecorded) return;
  w.currentRecorded = true;
  const QString nom = w.currentName.left(kMaxListedNameLength);
  if (w.emptiedSources.contains(nom)) return;
  if (w.emptiedSources.size() < kMaxListedSources) {
    w.emptiedSources.append(nom);
  } else {
    ++w.emptiedSourcesMore;
  }
}

bool isAsciiLetter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

/** Insensible a la casse ASCII : `aiguille` doit etre en minuscules. */
bool containsNoCase(const QByteArray& botte, const char* aiguille) {
  const qsizetype taille = static_cast<qsizetype>(qstrlen(aiguille));
  if (taille == 0 || botte.size() < taille) return false;
  for (qsizetype i = 0; i + taille <= botte.size(); ++i) {
    qsizetype j = 0;
    while (j < taille && (botte.at(i + j) | 0x20) == aiguille[j]) ++j;
    if (j == taille) return true;
  }
  return false;
}

/**
 * Marqueurs suivis par ffmpeg OU sondes par lui n'importe ou dans le fichier :
 * `<mpd` (DASH, demultiplexeur actif dans les ffmpeg de Linux), `<compositionplaylist`
 * (IMF), boites `rmra` / `rmda` (un MP4 / MOV qui renvoie vers d'autres films).
 * Recherche dans les 64 premiers Kio : le sondage d'ffmpeg s'arrete des qu'un
 * format sort nettement du lot, ce que fait vite un fichier a signature valide.
 */
bool ffmpegFollowsMarkers(const QByteArray& tete) {
  if (containsNoCase(tete, "<mpd") || containsNoCase(tete, "<compositionplaylist")) return true;
  const qsizetype n = tete.size();
  for (qsizetype i = 0; i + 3 < n; ++i) {
    if (tete.at(i) == 'r' && tete.at(i + 1) == 'm' &&
        (tete.at(i + 2) == 'r' || tete.at(i + 2) == 'd') && tete.at(i + 3) == 'a') {
      return true;
    }
  }
  return false;
}

/**
 * Ce que le module m3u de VLC prend pour une liste de lecture dans un fichier
 * qui n'en porte pas l'extension : une LIGNE qui commence par `<schema>://`
 * (ContainsURL). S'y ajoute une ligne qui commence par un chemin UNC
 * (`\\hote\`), qui ne sert qu'a une telle liste. Blancs de tete toleres. Une
 * URL au milieu d'une ligne (balise ID3, bloc XMP) n'en fait pas une liste.
 * Parcours lineaire : chaque position n'est examinee que comme debut de ligne.
 */
bool vlcPlaylistMarkers(const QByteArray& tete) {
  const auto schema = [](char c) {
    return isAsciiLetter(c) || (c >= '0' && c <= '9') || c == '+' || c == '.' || c == '-';
  };
  const qsizetype n = tete.size();
  for (qsizetype i = 0; i < n; ++i) {
    if (i > 0 && tete.at(i - 1) != '\n' && tete.at(i - 1) != '\r') continue;
    qsizetype j = i;
    while (j < n && (tete.at(j) == ' ' || tete.at(j) == '\t')) ++j;
    if (j + 2 < n && tete.at(j) == '\\' && tete.at(j + 1) == '\\') return true;
    if (j < n && isAsciiLetter(tete.at(j))) {
      qsizetype k = j + 1;
      while (k < n && k - j < 32 && schema(tete.at(k))) ++k;
      if (k + 2 < n && tete.at(k) == ':' && tete.at(k + 1) == '/' && tete.at(k + 2) == '/') {
        return true;
      }
    }
  }
  return false;
}

/** Releve les marqueurs d'un fichier extrait. Illisible : tout est suppose. */
MediaMarkers scanMediaMarkers(const QString& chemin) {
  MediaMarkers m;
  QFile fichier(chemin);
  if (!fichier.open(QIODevice::ReadOnly)) {
    m.ffmpeg = true;
    m.vlc = true;
    return m;
  }
  const QByteArray tete = fichier.read(64 * 1024);
  m.ffmpeg = ffmpegFollowsMarkers(tete);
  m.vlc = m.ffmpeg || vlcPlaylistMarkers(tete);
  return m;
}

/**
 * Vrai si le CONTENU de `chemin` correspond a son extension (signature des
 * premiers octets). L'extension seule ne prouve rien : une liste de lecture
 * (M3U, XSPF, ffconcat, HLS) renommee en `.mp4` ferait ouvrir par VLC ou ffmpeg
 * les fichiers ou URL qu'elle cite, et ces formats exigent leur en-tete en
 * tete de fichier, la ou la signature est verifiee. Les marqueurs qu'un
 * lecteur sonde AILLEURS dans le fichier sont juges a la reecriture, selon le
 * lecteur de la clef (MediaMarkers, mediaAllowedForKey). Un texte est refuse
 * s'il contient un octet nul ; il n'atteint jamais ffmpeg ni VLC (readerForKey).
 */
bool contentMatchesExtension(const QString& chemin, const QString& extension) {
  QFile fichier(chemin);
  if (!fichier.open(QIODevice::ReadOnly)) return false;
  const QByteArray t = fichier.read(64);
  const auto at = [&](int offset, const char* magie, int taille) {
    return t.size() >= offset + taille && t.mid(offset, taille) == QByteArray(magie, taille);
  };
  const auto octet = [&](int i) { return i < t.size() ? static_cast<quint8>(t.at(i)) : 0x100; };
  const QString e = extension.toLower();

  if (e == QStringLiteral("png")) return at(0, "\x89PNG", 4);
  if (e == QStringLiteral("jpg") || e == QStringLiteral("jpeg")) return at(0, "\xFF\xD8\xFF", 3);
  if (e == QStringLiteral("gif")) return at(0, "GIF8", 4);
  if (e == QStringLiteral("webp")) return at(0, "RIFF", 4) && at(8, "WEBP", 4);
  if (e == QStringLiteral("bmp")) return at(0, "BM", 2);
  if (e == QStringLiteral("psd")) return at(0, "8BPS", 4);
  if (e == QStringLiteral("tga")) {
    // Pas de signature : l'en-tete doit au moins etre plausible (type de
    // palette 0/1, type d'image 1-3 ou 9-11).
    const int palette = octet(1);
    const int type = octet(2);
    return t.size() >= 18 && (palette == 0 || palette == 1) &&
           ((type >= 1 && type <= 3) || (type >= 9 && type <= 11));
  }
  if (e == QStringLiteral("mp4") || e == QStringLiteral("mov") || e == QStringLiteral("m4v") ||
      e == QStringLiteral("m4a")) {
    // Premiere boite ISO-BMFF / QuickTime.
    for (const char* boite : {"ftyp", "moov", "mdat", "wide", "free", "skip", "pnot"}) {
      if (at(4, boite, 4)) return true;
    }
    return false;
  }
  if (e == QStringLiteral("mkv") || e == QStringLiteral("webm")) return at(0, "\x1A\x45\xDF\xA3", 4);
  if (e == QStringLiteral("avi")) return at(0, "RIFF", 4) && at(8, "AVI ", 4);
  if (e == QStringLiteral("wav")) return at(0, "RIFF", 4) && at(8, "WAVE", 4);
  if (e == QStringLiteral("flv")) return at(0, "FLV", 3);
  if (e == QStringLiteral("ts")) {
    // Synchro MPEG-TS tous les 188 octets.
    if (octet(0) != 0x47) return false;
    QByteArray suite;
    if (fichier.seek(188)) suite = fichier.read(1);
    return suite.isEmpty() || static_cast<quint8>(suite.at(0)) == 0x47;
  }
  if (e == QStringLiteral("mp3")) {
    return at(0, "ID3", 3) || (octet(0) == 0xFF && (octet(1) & 0xE0) == 0xE0);
  }
  if (e == QStringLiteral("aac")) {
    return at(0, "ID3", 3) || at(0, "ADIF", 4) || (octet(0) == 0xFF && (octet(1) & 0xF6) == 0xF0);
  }
  if (e == QStringLiteral("ogg") || e == QStringLiteral("opus")) return at(0, "OggS", 4);
  if (e == QStringLiteral("flac")) return at(0, "fLaC", 4) || at(0, "ID3", 3);
  if (e == QStringLiteral("ttf") || e == QStringLiteral("otf")) {
    return at(0, "\x00\x01\x00\x00", 4) || at(0, "OTTO", 4) || at(0, "true", 4) ||
           at(0, "ttcf", 4);
  }
  if (e == QStringLiteral("woff")) return at(0, "wOFF", 4);
  if (e == QStringLiteral("woff2")) return at(0, "wOF2", 4);

  if (e == QStringLiteral("txt") || e == QStringLiteral("json") || e == QStringLiteral("srt") ||
      e == QStringLiteral("vtt")) {
    // Lu comme du texte par une source texte, et par elle seule : une URL ou
    // un `#EXTM3U` n'y est qu'un mot affiche. Seul un octet nul (binaire
    // deguise) est refuse.
    fichier.seek(0);
    const QByteArray contenu = fichier.read(1024 * 1024);
    return !contenu.contains('\0');
  }
  // Extension autorisee mais sans regle ici : refusee (liste fermee).
  return false;
}

/**
 * Vrai si `text` designe un emplacement du systeme de fichiers hors du paquet :
 * URL `file:`, chemin absolu, UNC, chemin de peripherique, lettre de lecteur
 * (`C:\x` comme `C:x`, relatif au lecteur) ou dossier personnel (`~`).
 *
 * Le pire des cas d'un reglage importe : ouvrir `\\hote\partage\x` fait
 * envoyer par Windows l'empreinte NTLM de la session a cet hote, avant meme de
 * lire un octet ; et une source qui charge un module (`plugin_path` d'un filtre
 * VST) executerait ce qu'il y trouve.
 *
 * Les separateurs sont NORMALISES avant le test : Win32 lit comme UNC toute
 * paire de separateurs en tete, quel que soit leur sens (`/\hote`, `\/hote`),
 * et `\??\UNC\hote`, `\\?\UNC\hote`, `\\.\` sont d'autres portes vers le meme
 * endroit. Tous commencent par un separateur une fois normalises.
 */
bool isOutsidePathOrFileUrl(const QString& text) {
  if (text.startsWith(QStringLiteral("file:"), Qt::CaseInsensitive)) return true;
  QString n = text.trimmed();
  n.replace(QLatin1Char('/'), QLatin1Char('\\'));
  if (n.startsWith(QLatin1Char('\\'))) return true;  // racine, UNC, \??\, \\?\, \\.\ .
  if (n.size() >= 2 && n.at(1) == QLatin1Char(':') && n.at(0).isLetter()) return true;
  if (n.startsWith(QLatin1Char('~'))) return true;
  return false;
}

/** Clef de reglage qui, a son nom, porte un chemin (`plugin_path`, `track_matte_path`…). */
bool looksLikePathKey(const QString& cle) {
  if (CollectionPackager::settingsPathKeys().contains(cle)) return true;
  const QString c = cle.toLower();
  return c.contains(QStringLiteral("file")) || c.contains(QStringLiteral("path")) ||
         c.contains(QStringLiteral("dir")) || c.contains(QStringLiteral("folder"));
}

// ─── ZERO CONFIANCE ENVERS LE PAQUET ────────────────────────────────────────
//
// Le paquet ne vient pas forcement de notre empaqueteur : n'importe quel porteur
// d'un jeton de l'organisation peut deposer un ZIP fabrique a la main, et un
// autre membre l'importe ensuite chez lui. Tout champ de chemin qui ne designe
// pas un media EMBARQUE est donc vide : sans ca, une collection importee pourrait
// faire lire a OBS n'importe quel fichier du poste qui l'importe (une source
// navigateur `local_file` pointee sur un document, un fichier texte, etc.).
//
// Un `media/...` n'est ramene vers le dossier des medias importes que sous une
// clef de media (`isMediaKey`) ; ailleurs, la chaine reste telle quelle et ne
// designe rien d'utilisable.
//
// Au-dela de la liste explicite (`settingsPathKeys`), toute clef qui porte un
// chemin A SON NOM est videe quand sa valeur designe un emplacement hors du
// paquet : absolu, UNC, lecteur ou URL `file:` (`isOutsidePathOrFileUrl`). Un
// chemin relatif (`luma_wipe/...`, resolu par OBS dans ses propres donnees)
// reste. Le `plugin_path` d'un filtre VST est vide dans tous les cas, plus bas
// (`filtrerReglagesParType`).
QJsonObject reecrireObjet(const QJsonObject& entree, MediaRewrite& w) {
  const QStringList& pathKeys = CollectionPackager::settingsPathKeys();
  const QStringList& pathListKeys = CollectionPackager::settingsPathListKeys();
  // Objet instanciable : son type et son nom valent pour tout son contenu
  // (voir MediaRewrite). Ceux du parent sont rendus en sortant.
  const bool instanciable = estInstanciable(entree);
  const QString typeParent = w.currentType;
  const QString nomParent = w.currentName;
  const bool noteParent = w.currentRecorded;
  if (instanciable) {
    w.currentType = entree.value(QStringLiteral("id")).toString();
    w.currentName = entree.value(QStringLiteral("name")).toString();
    w.currentRecorded = false;
  }
  QJsonObject sortie = entree;
  for (const QString& cle : entree.keys()) {
    const QJsonValue valeur = entree.value(cle);

    if (valeur.isString()) {
      const QString texte = valeur.toString();
      QString localPath;
      if (cle == QStringLiteral("luma_image")) {
        // Transition « balayage luma » : un nom d'image RELATIF au dossier de
        // donnees d'OBS (`linear-h.png`). Rien d'autre : ni chemin hors paquet,
        // ni remontee, ni antislash.
        if (!texte.isEmpty() &&
            (isOutsidePathOrFileUrl(texte) || texte.contains(QLatin1Char('\\')) ||
             texte.split(QLatin1Char('/')).contains(QStringLiteral("..")))) {
          sortie[cle] = QString();
          ++w.rejected;
        }
        continue;
      }
      if (isMediaKey(cle) && packagedToLocal(texte, cle, w, &localPath)) {
        sortie[cle] = localPath;
        if (localPath.isEmpty()) {
          noteEmptiedMedia(w);
        } else {
          ++w.rewritten;
        }
      } else if (pathKeys.contains(cle) && !texte.isEmpty()) {
        sortie[cle] = QString();
        noteEmptiedMedia(w);
      } else if (looksLikePathKey(cle) && isOutsidePathOrFileUrl(texte)) {
        sortie[cle] = QString();
        ++w.rejected;
      }
      continue;
    }
    if (valeur.isArray() && pathListKeys.contains(cle)) {
      QJsonArray items;
      for (const QJsonValue& item : valeur.toArray()) {
        QJsonObject itemObject = item.toObject();
        QString localPath;
        if (packagedToLocal(itemObject.value(QStringLiteral("value")).toString(), cle, w,
                            &localPath) &&
            !localPath.isEmpty()) {
          itemObject[QStringLiteral("value")] = localPath;
          items.append(itemObject);
          ++w.rewritten;
        } else {
          noteEmptiedMedia(w);
        }
      }
      sortie[cle] = items;
      continue;
    }
    if (valeur.isObject() || valeur.isArray()) {
      sortie[cle] = reecrire(valeur, w);
    }
  }
  if (instanciable) {
    w.currentType = typeParent;
    w.currentName = nomParent;
    w.currentRecorded = noteParent;
  }
  return sortie;
}

QJsonValue reecrire(const QJsonValue& valeur, MediaRewrite& w) {
  if (valeur.isObject()) return reecrireObjet(valeur.toObject(), w);
  if (valeur.isArray()) {
    QJsonArray sortie;
    for (const QJsonValue& item : valeur.toArray()) {
      sortie.append(item.isObject() || item.isArray() || item.isString() ? reecrire(item, w)
                                                                         : item);
    }
    return sortie;
  }
  // Une chaine nue dans un tableau n'est sous aucune clef de media :
  // l'empaqueteur n'en produit pas, elle reste telle quelle.
  return valeur;
}

// ─── REGLAGES PAR TYPE DE SOURCE ────────────────────────────────────────────
//
// La reecriture ci-dessus ne connait que des clefs. Certaines sources d'OBS
// lisent pourtant un emplacement dans un champ qui n'a rien d'un chemin : une
// source media lit `input` (n'importe quelle URL que ffmpeg comprend, donc
// aussi `file:`, `concat:` ou un partage reseau), une source navigateur charge
// `url` et peut recevoir le droit de piloter OBS (`webpage_control_level`),
// un filtre VST charge un module. Ces types ont donc leur propre regle, en
// LISTE BLANCHE ; les types que nous ne connaissons pas (plugins tiers)
// perdent TOUS leurs reglages.

/** Types de source, filtre et transition fournis par OBS lui-meme. */
const QSet<QString>& nativeSourceTypes() {
  static const QSet<QString> types = {
      // Sources
      QStringLiteral("scene"), QStringLiteral("group"), QStringLiteral("image_source"),
      QStringLiteral("color_source"), QStringLiteral("color_source_v2"),
      QStringLiteral("color_source_v3"), QStringLiteral("slideshow"),
      QStringLiteral("slideshow_v2"), QStringLiteral("ffmpeg_source"),
      QStringLiteral("vlc_source"), QStringLiteral("browser_source"),
      QStringLiteral("text_gdiplus"), QStringLiteral("text_gdiplus_v2"),
      QStringLiteral("text_gdiplus_v3"), QStringLiteral("text_ft2_source"),
      QStringLiteral("text_ft2_source_v2"), QStringLiteral("audio_line"),
      QStringLiteral("wasapi_input_capture"), QStringLiteral("wasapi_output_capture"),
      QStringLiteral("wasapi_process_output_capture"), QStringLiteral("dshow_input"),
      QStringLiteral("monitor_capture"), QStringLiteral("window_capture"),
      QStringLiteral("game_capture"), QStringLiteral("coreaudio_input_capture"),
      QStringLiteral("coreaudio_output_capture"), QStringLiteral("av_capture_input"),
      QStringLiteral("av_capture_input_v2"), QStringLiteral("macos-avcapture"),
      QStringLiteral("macos-avcapture-fast"), QStringLiteral("screen_capture"),
      QStringLiteral("display_capture"), QStringLiteral("pulse_input_capture"),
      QStringLiteral("pulse_output_capture"), QStringLiteral("alsa_input_capture"),
      QStringLiteral("jack_output_capture"), QStringLiteral("v4l2_input"),
      QStringLiteral("xshm_input"), QStringLiteral("xshm_input_v2"),
      QStringLiteral("xcomposite_input"), QStringLiteral("pipewire-desktop-capture-source"),
      QStringLiteral("pipewire-window-capture-source"),
      QStringLiteral("pipewire-screen-capture-source"), QStringLiteral("decklink-input"),
      QStringLiteral("aja_source"),
      // La notre : sa regle (liste blanche) est dans filtrerReglagesParType.
      QStringLiteral("semirprod_live_source"),
      // Filtres
      QStringLiteral("mask_filter"), QStringLiteral("mask_filter_v2"),
      QStringLiteral("crop_filter"), QStringLiteral("color_filter"),
      QStringLiteral("color_filter_v2"), QStringLiteral("color_key_filter"),
      QStringLiteral("color_key_filter_v2"), QStringLiteral("chroma_key_filter"),
      QStringLiteral("chroma_key_filter_v2"), QStringLiteral("luma_key_filter"),
      QStringLiteral("luma_key_filter_v2"), QStringLiteral("scroll_filter"),
      QStringLiteral("sharpness_filter"), QStringLiteral("sharpness_filter_v2"),
      QStringLiteral("gain_filter"), QStringLiteral("noise_gate_filter"),
      QStringLiteral("noise_suppress_filter"), QStringLiteral("noise_suppress_filter_v2"),
      QStringLiteral("compressor_filter"), QStringLiteral("expander_filter"),
      QStringLiteral("upward_compressor_filter"), QStringLiteral("limiter_filter"),
      QStringLiteral("invert_polarity_filter"), QStringLiteral("async_delay_filter"),
      QStringLiteral("gpu_delay"), QStringLiteral("scale_filter"),
      QStringLiteral("clut_filter"), QStringLiteral("hdr_tonemap_filter"),
      QStringLiteral("basic_eq_filter"), QStringLiteral("vst_filter"),
      QStringLiteral("nvidia_greenscreen_filter"), QStringLiteral("nvidia_denoiser_filter"),
      // Transitions
      QStringLiteral("cut_transition"), QStringLiteral("fade_transition"),
      QStringLiteral("swipe_transition"), QStringLiteral("slide_transition"),
      QStringLiteral("obs_stinger_transition"), QStringLiteral("fade_to_color_transition"),
      QStringLiteral("wipe_transition"), QStringLiteral("luma_wipe_transition"),
  };
  return types;
}

/**
 * Garde, dans `reglages`, les seules clefs de `permises`. Rend le nombre de
 * clefs retirees. LISTE BLANCHE : une clef ajoutee par une version future du
 * type (ou par un paquet fabrique) part par defaut.
 */
/** Ce que l'assainissement retire, pour le journal et le rapport d'import. */
struct Rapport {
  int rejected = 0;
  /** URL de sources navigateur retirees : « nom de la source (hote) ». */
  QStringList urlsRetirees;
  /** Sauvegarde de l'operateur lui-meme, selon le serveur : raccourcis clavier
      et CSS des sources navigateur gardes seulement dans ce cas. */
  bool keepHotkeys = false;
};

int garderSeulement(QJsonObject& reglages, const QStringList& permises) {
  int retirees = 0;
  for (const QString& cle : reglages.keys()) {
    if (!permises.contains(cle)) {
      reglages.remove(cle);
      ++retirees;
    }
  }
  return retirees;
}

/**
 * Regle d'un type sur UN objet de reglages (`settings`, ou `transition` d'une
 * transition d'apparition / de disparition d'element de scene).
 */
void filtrerReglagesParType(const QString& type, const QString& nom, QJsonObject& reglages,
                            Rapport& r) {
  int retires = 0;
  if (type == QStringLiteral("ffmpeg_source")) {
    // LISTE BLANCHE, fichier local seulement. `input`, `input_format` et
    // `ffmpeg_options` partent : ce dernier passe des options avformat
    // (`protocol_whitelist`, `safe`…) qui font lire a ffmpeg un fichier
    // quelconque du poste, ou un hote du reseau local, meme avec une entree
    // d'apparence anodine. `local_file` a deja ete confine au dossier des
    // medias importes (reecrireObjet) ou vide.
    static const QStringList kPermises = {
        QStringLiteral("local_file"),          QStringLiteral("looping"),
        QStringLiteral("restart_on_activate"), QStringLiteral("clear_on_media_end"),
        QStringLiteral("close_when_inactive"), QStringLiteral("speed_percent"),
        QStringLiteral("color_range"),         QStringLiteral("linear_alpha"),
        QStringLiteral("hw_decode"),           QStringLiteral("buffering_mb"),
    };
    retires += garderSeulement(reglages, kPermises);
    reglages[QStringLiteral("is_local_file")] = true;
  } else if (type == QStringLiteral("browser_source")) {
    // LISTE BLANCHE, plus `url` sous condition (voir plus bas).
    // Fichier local : jamais (obs-browser sert les fichiers locaux sous une
    // origine unique, d'ou une page lirait tout autre fichier du poste).
    static const QStringList kPermises = {
        QStringLiteral("width"),    QStringLiteral("height"),
        QStringLiteral("fps"),      QStringLiteral("fps_custom"),
        QStringLiteral("css"),      QStringLiteral("shutdown"),
        QStringLiteral("restart_when_active"), QStringLiteral("reroute_audio"),
    };
    // `url` est gardee si c'est une page HTTPS DISTANTE d'un FOURNISSEUR
    // D'OVERLAYS CONNU (Config::isKnownOverlayHost) : sans elle, chaque
    // streamer perdait toutes ses URL d'overlays en restaurant sa propre
    // sauvegarde. Une telle page est acceptable ici parce qu'elle n'a AUCUN
    // controle sur OBS (niveau force a 0 plus bas) et ne tourne pas sous
    // l'origine des fichiers locaux (`http://absolute/`, fichier local retire).
    // Refusees : tout autre schema, identifiants dans l'URL, hote local ou
    // prive (isPublicNetworkUrl), `absolute`, nom sans point, et tout hote hors
    // de la liste. Une URL retiree est signalee a l'operateur (nom de la
    // source + hote) pour qu'il la ressaisisse.
    const QString url = reglages.value(QStringLiteral("url")).toString();
    retires += garderSeulement(reglages, kPermises);
    if (!url.isEmpty()) {
      const QUrl parsed(url);
      const QString host = parsed.host().toLower();
      const bool acceptee =
          parsed.scheme() == QStringLiteral("https") &&
          Config::isPublicNetworkUrl(url, {QStringLiteral("https")}) &&
          parsed.userInfo().isEmpty() && host != QStringLiteral("absolute") &&
          host.contains(QLatin1Char('.')) && !host.endsWith(QLatin1Char('.')) &&
          Config::isKnownOverlayHost(host);
      if (acceptee) {
        reglages[QStringLiteral("url")] = url;
        --retires;  // gardee : ne compte pas comme retiree
      } else if (r.urlsRetirees.size() < 50) {
        r.urlsRetirees.append(QStringLiteral("%1 (%2)").arg(
            nom.left(80), host.isEmpty() ? QStringLiteral("?") : host.left(80)));
      }
    }
    // CSS injectee dans la page : jamais de `@import` (feuille chargee
    // ailleurs, choisie par l'auteur du paquet). Sauvegarde d'un autre compte :
    // la CSS part en entier.
    if (reglages.contains(QStringLiteral("css"))) {
      if (!r.keepHotkeys) {
        reglages.remove(QStringLiteral("css"));
        ++retires;
      } else {
        static const QRegularExpression kImport(QStringLiteral("@import[^;\n]*;?"),
                                                QRegularExpression::CaseInsensitiveOption);
        QString css = reglages.value(QStringLiteral("css")).toString();
        if (css.contains(kImport)) {
          css.remove(kImport);
          reglages[QStringLiteral("css")] = css;
          ++retires;
        }
      }
    }
    reglages[QStringLiteral("is_local_file")] = false;
    // Niveau de controle de la page sur OBS : le minimum (aucun acces).
    reglages[QStringLiteral("webpage_control_level")] = 0;
  } else if (type == QStringLiteral("semirprod_live_source")) {
    // Notre source : seul le flux choisi est un reglage de l'operateur. Son
    // adresse de lecture est resolue par le plugin a partir du compte, jamais
    // lue dans la collection.
    static const QStringList kPermises = {QStringLiteral("stream_id")};
    retires += garderSeulement(reglages, kPermises);
    if (reglages.value(QStringLiteral("stream_id")).toString().size() > 128) {
      reglages.remove(QStringLiteral("stream_id"));
      ++retires;
    }
  } else if (type == QStringLiteral("vst_filter")) {
    // `plugin_path` est un module charge par LoadLibraryW / dlopen, et l'etat
    // du module (`chunk_*`) est donne tel quel a ce module : jamais repris.
    for (const QString& cle : reglages.keys()) {
      if (cle == QStringLiteral("plugin_path") || cle.contains(QStringLiteral("chunk"))) {
        reglages.remove(cle);
        ++retires;
      }
    }
  } else if (!nativeSourceTypes().contains(type)) {
    // Type inconnu (plugin tiers), ou non classable (type vide) : ce que ses
    // reglages declenchent n'est pas connu (certains lancent des commandes,
    // ouvrent des fichiers, des URL). Ils sont vides en entier ; le nom et le
    // type restent, pour que l'operateur voie quoi reconfigurer.
    if (!reglages.isEmpty()) {
      reglages = QJsonObject();
      ++retires;
    }
  }
  r.rejected += retires;
}

/**
 * Vrai si OBS peut instancier cet objet : il porte un `id` ou un
 * `versioned_id` TEXTE. (L'`id` d'un element de scene est un nombre, celui
 * d'une transition rapide aussi : ils ne sont pas concernes.)
 */
bool estInstanciable(const QJsonObject& objet) {
  return objet.value(QStringLiteral("id")).isString() ||
         objet.value(QStringLiteral("versioned_id")).isString();
}

/**
 * Applique a un objet instanciable la regle de son type. Rend faux si l'objet
 * doit etre RETIRE de son parent.
 *
 * OBS cree l'objet sous `versioned_id` s'il est renseigne, `id` sinon
 * (CollectionPackager::effectiveSourceType). Filtrer sur `id` seul laissait
 * passer `"id":"color_source","versioned_id":"browser_source"`. Couple
 * incoherent : objet retire, seul un paquet fabrique peut le porter. Couple
 * coherent = `versioned_id` vaut `id` ou `id_v<N>` : l'objet sera une VERSION
 * de `id`, c'est donc la regle de `id` qui s'applique.
 *
 * Les reglages vivent sous `settings`, ou sous `transition` pour les
 * transitions d'apparition / de disparition d'un element de scene
 * (obs_source_create_private(id, nom, transition)) : les deux sont filtres.
 * `private_settings` d'un type inconnu est vide aussi.
 */
bool filtrerObjetInstanciable(QJsonObject& objet, Rapport& r) {
  bool coherent = true;
  CollectionPackager::effectiveSourceType(objet, &coherent);
  if (!coherent) {
    ++r.rejected;
    return false;
  }
  const QString type = objet.value(QStringLiteral("id")).toString();
  for (const QString cle : {QStringLiteral("settings"), QStringLiteral("transition")}) {
    if (!objet.contains(cle)) continue;
    const QJsonValue valeur = objet.value(cle);
    if (!valeur.isObject()) {
      // Forme inattendue : on ne sait pas ce qu'OBS en ferait, elle part.
      objet[cle] = QJsonObject();
      ++r.rejected;
      continue;
    }
    QJsonObject reglages = valeur.toObject();
    filtrerReglagesParType(type, objet.value(QStringLiteral("name")).toString(), reglages, r);
    objet[cle] = reglages;
  }
  if (!nativeSourceTypes().contains(type) && objet.contains(QStringLiteral("private_settings")) &&
      !objet.value(QStringLiteral("private_settings")).toObject().isEmpty()) {
    objet[QStringLiteral("private_settings")] = QJsonObject();
    ++r.rejected;
  }
  return true;
}

constexpr int kMaxNameLength = 256;

bool assainirNoeud(QJsonObject& objet, Rapport& r);

QJsonArray assainirTableau(const QJsonArray& tableau, Rapport& r) {
  QJsonArray gardes;
  for (const QJsonValue& item : tableau) {
    if (item.isObject()) {
      QJsonObject objet = item.toObject();
      // Un objet retire laisse au plus un element de scene orphelin, qu'OBS
      // ignore au chargement (source introuvable).
      if (assainirNoeud(objet, r)) gardes.append(objet);
    } else if (item.isArray()) {
      gardes.append(assainirTableau(item.toArray(), r));
    } else {
      gardes.append(item);
    }
  }
  return gardes;
}

/**
 * Parcours de TOUT le document. On n'enumere plus les endroits ou OBS cree des
 * sources (`sources`, `groups`, `transitions`, `filters`…) : la liste etait
 * incomplete (peripheriques audio de la racine `DesktopAudioDevice1/2`,
 * `AuxAudioDevice1..4`, transitions d'apparition des elements de scene,
 * filtres de filtres, sources d'un canvas) et chaque oubli contournait toutes
 * les regles. Tout objet instanciable, ou qu'il soit, passe par
 * filtrerObjetInstanciable ; puis ses enfants, y compris ses reglages, sont
 * parcourus a leur tour.
 */
bool assainirNoeud(QJsonObject& objet, Rapport& r) {
  if (estInstanciable(objet) && !filtrerObjetInstanciable(objet, r)) return false;
  for (const QString& cle : objet.keys()) {
    const QJsonValue valeur = objet.value(cle);
    // Noms de scene / source / element bornes a kMaxNameLength : ils partent
    // dans le journal et dans l'interface, et un nom de plusieurs megaoctets
    // les ferait ramer. Coupe partout de la meme facon, les references par nom
    // (elements de scene) restent donc coherentes.
    if (cle == QStringLiteral("name") && valeur.isString() &&
        valeur.toString().size() > kMaxNameLength) {
      objet[cle] = valeur.toString().left(kMaxNameLength);
      continue;
    }
    // Raccourcis clavier (de source, de scene, de transition rapide…) : gardes
    // pour la propre sauvegarde de l'operateur SEULEMENT. Un paquet d'un autre
    // compte pourrait sinon poser « demarrer le direct » ou « basculer de
    // scene » sur une touche que le streamer frappe sans y penser.
    if (cle == QStringLiteral("hotkeys") && !r.keepHotkeys) {
      objet.remove(cle);
      continue;
    }
    if (valeur.isObject()) {
      QJsonObject enfant = valeur.toObject();
      if (assainirNoeud(enfant, r)) {
        objet[cle] = enfant;
      } else {
        objet.remove(cle);
      }
    } else if (valeur.isArray()) {
      objet[cle] = assainirTableau(valeur.toArray(), r);
    }
  }
  return true;
}

/** Passe toute la collection par assainirNoeud (la racine n'est jamais retiree). */
void filtrerReglagesSources(QJsonObject& collection, Rapport& r) {
  assainirNoeud(collection, r);
}

/** Types de source employes par la collection, pour dire ce qui manque ICI. */
QStringList typesEmployes(const QJsonValue& valeur) {
  QStringList types;
  if (valeur.isObject()) {
    const QJsonObject objet = valeur.toObject();
    if ((objet.contains(QStringLiteral("id")) || objet.contains(QStringLiteral("versioned_id"))) &&
        objet.contains(QStringLiteral("settings"))) {
      // Le type qu'OBS emploiera, comme dans filtrerObjetInstanciable.
      const QString type = CollectionPackager::effectiveSourceType(objet);
      if (!type.isEmpty()) types.append(type);
    }
    for (const QString& cle : objet.keys()) types += typesEmployes(objet.value(cle));
  } else if (valeur.isArray()) {
    for (const QJsonValue& item : valeur.toArray()) types += typesEmployes(item);
  }
  types.removeDuplicates();
  return types;
}

}  // namespace

CollectionImporter::Job CollectionImporter::prepare(const QString& packagePath,
                                                    const QString& wantedName) {
  Job job;
  job.packagePath = packagePath;
  job.wantedName = wantedName;

  if (obs_frontend_streaming_active() || obs_frontend_recording_active()) {
    // Ecrire des centaines de megaoctets sur le disque qui enregistre le direct
    // produit des images sautees. Refus explicite, pas de file d'attente.
    job.error = loc("Import.DuringStream");
    return job;
  }

  const QStringList existantes = CollectionPackager::availableCollections();

  // On n'ECRASE JAMAIS une collection existante : un import est justement le
  // geste qu'on fait quand on n'est pas sûr, et il prendrait la seule copie d'un
  // habillage en cours de travail.
  QString nom = wantedName.isEmpty() ? loc("Import.DefaultName") : wantedName;
  if (existantes.contains(nom)) {
    nom = loc("Import.NameCopy").arg(wantedName);
    for (int suffixe = 2; existantes.contains(nom) && suffixe < 100; ++suffixe) {
      nom = loc("Import.NameCopyN").arg(wantedName).arg(suffixe);
    }
  }
  job.finalName = nom;

  // Le dossier des collections vient d'OBS lui-meme (chemin du profil courant),
  // donc juste aussi en mode portable. Meme resolution que la sauvegarde.
  job.scenesDir = CollectionPackager::scenesDirectoryPublic();
  if (job.scenesDir.isEmpty()) {
    job.error = loc("Backup.FolderMissing");
    return job;
  }

  // Les medias importes vivent A COTE des collections, dans un dossier qui nous
  // appartient : jamais melanges aux fichiers de l'operateur, et effacables sans
  // rien casser d'autre.
  // Dossier NEUF a chaque import (suffixe aleatoire) : reutiliser
  // `semirprod-medias/<nom>` gardait les fichiers d'un import precedent (meme
  // nom, ou deux noms nettoyes pareil : « A B » / « A_B »), que la collection
  // aurait designes sans qu'ils soient passes par les controles de celui-ci.
  job.mediaDir = QDir(job.scenesDir)
                     .absoluteFilePath(QStringLiteral("semirprod-medias/%1-%2")
                                           .arg(fichierPourCollection(job.finalName),
                                                QUuid::createUuid()
                                                    .toString(QUuid::Id128)
                                                    .left(8)));

  job.ok = true;
  return job;
}

CollectionImporter::Result CollectionImporter::run(const Job& job,
                                                    const QStringList& availableSourceTypes) {
  Result result;
  result.collectionName = job.finalName;

  ZipReader zip(job.packagePath);
  if (!zip.open()) {
    result.error = zip.lastError();
    return result;
  }

  const QByteArray brut = zip.read(QStringLiteral("collection.json"), kMaxCollectionJson);
  if (brut.isEmpty()) {
    result.error = zip.lastError();
    return result;
  }
  QJsonParseError parseError{};
  const QJsonDocument doc = QJsonDocument::fromJson(brut, &parseError);
  if (!doc.isObject()) {
    result.error =
        loc("Import.UnreadablePackage")
            .arg(parseError.errorString());
    return result;
  }

  // Nom de fichier deja pris : refus AVANT d'ecrire quoi que ce soit sur le
  // disque (il etait verifie apres l'extraction des medias).
  const QString fichier =
      QDir(job.scenesDir)
          .absoluteFilePath(QStringLiteral("%1.json").arg(fichierPourCollection(job.finalName)));
  if (QFile::exists(fichier)) {
    result.error = loc("Import.NameTaken");
    return result;
  }

  // Les medias d'abord : si l'un manque, autant le savoir avant d'avoir ecrit une
  // collection qui les reference.
  //
  // REGLE (01/11/2026) : les fichiers medias ne sont importes que depuis la
  // PROPRE sauvegarde de l'operateur (provenance donnee par le serveur, jamais
  // par le paquet). Une sauvegarde d'un autre membre de l'espace donne les
  // scenes et leur disposition, sans aucun fichier : rien n'est ecrit sur le
  // disque, et chaque reglage `media/...` est vide plus bas (`w.valid` reste
  // vide). Le controle du contenu des medias reste en place pour la propre
  // sauvegarde, mais ce n'est plus lui qui protege un poste d'un paquet tiers.
  MediaRewrite w;
  w.mediaDir = QDir(job.mediaDir);
  const QDir& mediaDir = w.mediaDir;
  if (job.ownBackup && !QDir().mkpath(job.mediaDir)) {
    result.error = loc("Import.MediaFolderFailed");
    return result;
  }
  // Import en echec : son dossier de medias (neuf, a lui seul) part avec lui.
  const auto nettoyage = qScopeGuard([&result, &job]() {
    if (!result.ok) QDir(job.mediaDir).removeRecursively();
  });
  // Fichiers extraits ET valides par cet import : les seuls que la collection
  // pourra designer (packagedToLocal, via w.valid).
  int rejectedMedia = 0;
  for (const ZipReader::Entry& entree : zip.entries()) {
    if (!entree.name.startsWith(QStringLiteral("media/"))) continue;
    if (!job.ownBackup) {
      // Une entree de dossier (`media/x/`) n'est pas un fichier non importe.
      if (!entree.name.endsWith(QLatin1Char('/'))) ++result.mediaDroppedCount;
      continue;
    }
    // La liste d'extensions de l'empaqueteur est REAPPLIQUEE ici : un paquet
    // fabrique a la main pourrait sinon deposer un executable ou un script sur
    // le disque de celui qui importe.
    if (!CollectionPackager::allowedExtensions().contains(
            QFileInfo(entree.name).suffix().toLower())) {
      ++rejectedMedia;
      continue;
    }
    // Meme regle de confinement que pour les chemins du JSON : une entree qui
    // sortirait du dossier des medias n'est pas ecrite.
    const QString destination = confinedMediaPath(entree.name.mid(6), mediaDir);
    if (destination.isEmpty()) {
      ++rejectedMedia;
      continue;
    }
    if (!zip.extractTo(entree.name, destination)) {
      result.error = zip.lastError();
      return result;
    }
    // L'extension a ete verifiee, pas le contenu : un executable renomme en
    // image est retire du disque avant que quoi que ce soit le designe.
    if (startsWithExecutableMagic(destination) ||
        !contentMatchesExtension(destination, QFileInfo(entree.name).suffix())) {
      // Un fichier refuse qu'on ne peut pas effacer (antivirus, verrou) resterait
      // sur le disque sous un nom attendu : l'import s'arrete.
      if (!QFile::remove(destination) && QFile::exists(destination)) {
        result.error = loc("Import.RejectedFileStuck");
        return result;
      }
      ++rejectedMedia;
      continue;
    }
    // Ce que le contenu ferait faire a ffmpeg ou VLC : juge a la reecriture,
    // selon le lecteur de la clef qui designe le fichier (mediaAllowedForKey).
    w.valid.insert(destination, scanMediaMarkers(destination));
    ++result.mediaCount;
    result.mediaBytes += entree.size;
  }

  QJsonObject collection = doc.object();
  // Le nom PORTE dans le fichier est celui qu'OBS affichera : il doit etre le nom
  // libre calcule, sinon deux collections porteraient le meme nom dans la liste.
  collection[QStringLiteral("name")] = job.finalName;

  // Donnees de modules : seuls ceux d'OBS et les notres entrent. Les scripts
  // Lua/Python (`scripts-tool`) en premier lieu : OBS les executerait a
  // l'ouverture de la collection. Mais aussi ceux des plugins tiers, dont
  // certains executent des actions (Advanced Scene Switcher lance des
  // programmes, ecrit des fichiers). Voir CollectionPackager::filterImportedModules.
  //
  // Les modules de plugins TIERS ne sont donc pas restaures, meme depuis la
  // propre sauvegarde de l'operateur (l'export les garde, sans leurs secrets) :
  // un paquet peut venir d'un autre membre de l'espace, et ces modules
  // declenchent des actions. Leur configuration est a refaire apres import
  // (dit aussi dans le README). Les modules natifs d'OBS sont gardes mais
  // NEUTRALISES (filterImportedModules : rien ne demarre seul).
  int rejected = CollectionPackager::filterImportedModules(collection, /*keepNativeObs=*/true);
  // Projecteurs sauvegardes : rouverts par OBS au chargement, en plein ecran
  // sur un moniteur choisi par l'auteur du paquet. Repris pour la propre
  // sauvegarde de l'operateur seulement (meme regle que les raccourcis).
  if (!job.ownBackup && collection.contains(QStringLiteral("saved_projectors"))) {
    collection.remove(QStringLiteral("saved_projectors"));
    ++rejected;
  }

  collection = reecrireObjet(collection, w);
  const int reecrits = w.rewritten;
  rejected += w.rejected;
  result.mediaRejectedCount = rejectedMedia;
  result.emptiedMediaSources = w.emptiedSources;
  result.emptiedMediaSourcesMore = w.emptiedSourcesMore;
  // Puis la regle propre a chaque type de source (entree reseau d'une source
  // media, URL et droits d'une source navigateur, chemins des plugins tiers).
  Rapport rapport;
  rapport.keepHotkeys = job.ownBackup;
  filtrerReglagesSources(collection, rapport);
  rejected += rapport.rejected;
  result.droppedBrowserUrls = rapport.urlsRetirees;
  result.hotkeysDropped = !job.ownBackup;

  // Ce qui manque ICI, sur cette machine — pas ce qui manquait sur celle
  // d'origine. C'est la seule version utile de l'information au moment d'importer.
  for (const QString& type : typesEmployes(collection)) {
    if (!availableSourceTypes.contains(type)) result.missingSourceTypes.append(type);
  }

  // Cloud : ne garde que ce qui peut tourner sur l'instance (canvas, sources
  // et modules de plugins absents). Public : rien. Voir
  // src/cloud/CloudImportFilter.cpp.
  edition::filterImportedCollection(collection, availableSourceTypes);

  if (QFile::exists(fichier)) {
    // Le nom de collection etait libre mais le nom de FICHIER est deja pris
    // (deux noms differents peuvent donner le meme fichier apres nettoyage). On
    // refuse plutot que d'ecraser le fichier d'une autre collection.
    result.error = loc("Import.NameTaken");
    return result;
  }

  QFile sortie(fichier);
  if (!sortie.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    result.error = loc("Import.WriteFailed")
                       .arg(sortie.errorString());
    return result;
  }
  const QByteArray json = QJsonDocument(collection).toJson(QJsonDocument::Compact);
  if (sortie.write(json) != json.size()) {
    sortie.close();
    QFile::remove(fichier);
    result.error = loc("Import.WriteInterrupted");
    return result;
  }
  sortie.flush();
  sortie.close();

  result.collectionFile = fichier;
  result.ok = true;
  blog(LOG_INFO,
       "[semirprod-import] « %s » importée : %d médias, %d chemins réécrits, %d type(s) absent(s), "
       "%d média(s) et %d réglage(s) écartés",
       job.finalName.toUtf8().constData(), result.mediaCount, reecrits,
       static_cast<int>(result.missingSourceTypes.size()), rejectedMedia, rejected);
  return result;
}

bool CollectionImporter::publishToObs(const QString& collectionName,
                                      const QString& importedFile,
                                      const QString& scenesDir,
                                      QString* error) {
  const auto echec = [error](const QString& message) {
    if (error) *error = message;
    return false;
  };

  if (collectionName.isEmpty() || importedFile.isEmpty() || scenesDir.isEmpty()) {
    return echec(loc("Import.Incomplete"));
  }

  // La collection courante, pour y revenir. Elle DOIT etre relevee avant la
  // creation : apres, c'est deja la nouvelle qui est courante.
  char* courantBrut = obs_frontend_get_current_scene_collection();
  const QString courant = QString::fromUtf8(courantBrut ? courantBrut : "");
  bfree(courantBrut);

  // 1. C'est OBS qui cree la collection. Sans ce passage, rien de ce qu'on
  //    ecrit sur le disque n'entre dans sa liste ni dans son menu.
  if (!obs_frontend_add_scene_collection(collectionName.toUtf8().constData())) {
    return echec(loc("Import.ObsRefused")
                     .arg(collectionName));
  }

  // 2. Le nom de fichier vient d'OBS, jamais de nous. Il applique sa propre
  //    regle de nommage et deduplique ; former le nom de notre cote reviendrait
  //    a ecrire a cote et a laisser une collection vide a l'ecran.
  config_t* userConfig = obs_frontend_get_user_config();
  if (!userConfig) return echec(loc("Import.ConfigMissing"));
  QString fichierObs =
      QString::fromUtf8(config_get_string(userConfig, "Basic", "SceneCollectionFile"));
  if (fichierObs.isEmpty()) {
    return echec(loc("Import.NotNamed"));
  }
  // Selon les versions, la valeur porte l'extension ou non.
  if (fichierObs.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive)) {
    fichierObs.chop(5);
  }
  const QString cible =
      QDir(scenesDir).absoluteFilePath(QStringLiteral("%1.json").arg(fichierObs));

  // 3. Retour a la collection de depart. C'est CE changement qui fait ecrire a
  //    OBS la collection vide qu'il vient de creer : ecraser avant, c'est se
  //    faire effacer juste apres.
  if (!courant.isEmpty() && courant != collectionName) {
    obs_frontend_set_current_scene_collection(courant.toUtf8().constData());
  }

  // 4. Notre contenu prend la place du fichier vide.
  if (QFileInfo(importedFile).absoluteFilePath() != QFileInfo(cible).absoluteFilePath()) {
    QFile::remove(cible);
    if (!QFile::rename(importedFile, cible)) {
      // Sur certains montages, renommer echoue la ou copier passe.
      if (!QFile::copy(importedFile, cible)) {
        return echec(loc("Import.InstallFailed"));
      }
      QFile::remove(importedFile);
    }
  }

  // 5. On NE BASCULE PAS sur la collection importee : elle apparait dans la
  //    liste, et l'operateur l'ouvre quand il le decide (bouton « Ouvrir » du
  //    Studio, ou menu d'OBS). Basculer d'office chargerait sans un geste un
  //    contenu venu du reseau, et emporterait la scene a l'antenne.
  //
  //    Seule exception : sans collection de depart (cas degenere), OBS est
  //    reste sur la collection vide qu'il vient de creer. Il faut alors la
  //    recharger depuis le disque, sinon son prochain enregistrement
  //    ecraserait le contenu importe par une collection vide.
  if (courant.isEmpty()) {
    obs_frontend_set_current_scene_collection(collectionName.toUtf8().constData());
  }

  blog(LOG_INFO,
       "[semirprod-import] « %s » publiée dans OBS (fichier « %s.json »), sans redémarrage",
       collectionName.toUtf8().constData(), fichierObs.toUtf8().constData());
  return true;
}

}  // namespace semirprod
