#include "ZipReader.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <QSet>
#include <QVector>

#include <algorithm>
#include <array>
#include <vector>
#include "../Locale.hpp"

namespace semirprod {
namespace {

/// Table CRC-32 (polynome 0xEDB88320), celle du format ZIP et de zlib.
const std::array<quint32, 256>& crcTable() {
  static const std::array<quint32, 256> table = [] {
    std::array<quint32, 256> t{};
    for (quint32 i = 0; i < 256; ++i) {
      quint32 c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      t[i] = c;
    }
    return t;
  }();
  return table;
}

quint32 crcUpdate(quint32 crc, const char* data, qint64 length) {
  const auto& table = crcTable();
  quint32 c = crc ^ 0xFFFFFFFFu;
  for (qint64 i = 0; i < length; ++i) {
    c = table[(c ^ static_cast<unsigned char>(data[i])) & 0xFF] ^ (c >> 8);
  }
  return c ^ 0xFFFFFFFFu;
}

quint16 le16(const QByteArray& buffer, int offset) {
  return static_cast<quint16>(static_cast<unsigned char>(buffer[offset])) |
         (static_cast<quint16>(static_cast<unsigned char>(buffer[offset + 1])) << 8);
}

quint32 le32(const QByteArray& buffer, int offset) {
  return static_cast<quint32>(static_cast<unsigned char>(buffer[offset])) |
         (static_cast<quint32>(static_cast<unsigned char>(buffer[offset + 1])) << 8) |
         (static_cast<quint32>(static_cast<unsigned char>(buffer[offset + 2])) << 16) |
         (static_cast<quint32>(static_cast<unsigned char>(buffer[offset + 3])) << 24);
}

constexpr qint64 kChunk = 256 * 1024;

/**
 * Nombre maximal d'entrees acceptees dans un paquet.
 *
 * Le plafond de l'empaqueteur (`CollectionPackager::kMaxEntries`, 2000 medias),
 * plus le JSON de collection et le manifeste. Au-dela, le paquet n'a pas ete
 * produit par nous : un catalogue de 65 535 entrees minuscules suffit sinon a
 * remplir le dossier des medias de l'operateur.
 */
constexpr int kMaxEntries = 2000 + 2;

/**
 * Nom de peripherique reserve par Windows (CON, PRN, AUX, NUL, COM1-9, LPT1-9,
 * avec ou sans extension, casse indifferente). `media/x/AUX.png` ne cree pas un
 * fichier sous Windows : il ouvre le port serie, et l'ecriture part ailleurs
 * que dans le dossier d'import. L'empaqueteur prefixe chaque media d'une
 * empreinte (`<hash>-nom`), donc un tel segment ne vient jamais d'un paquet a
 * nous : refuse sur toutes les plateformes, le paquet doit rester importable
 * partout.
 */
bool isWindowsReservedName(const QString& segment) {
  // Windows ignore l'extension et les espaces de fin du nom de base.
  const QString base = segment.section(QLatin1Char('.'), 0, 0).trimmed().toUpper();
  static const QStringList kFixed = {QStringLiteral("CON"), QStringLiteral("PRN"),
                                     QStringLiteral("AUX"), QStringLiteral("NUL"),
                                     QStringLiteral("CONIN$"), QStringLiteral("CONOUT$")};
  if (kFixed.contains(base)) return true;
  if (base.size() == 4 &&
      (base.startsWith(QStringLiteral("COM")) || base.startsWith(QStringLiteral("LPT")))) {
    const QChar d = base.at(3);
    // Chiffres 1-9, et les exposants 1, 2, 3 que Windows traite de meme.
    if ((d >= QLatin1Char('1') && d <= QLatin1Char('9')) || d == QChar(0x00B9) ||
        d == QChar(0x00B2) || d == QChar(0x00B3)) {
      return true;
    }
  }
  return false;
}

}  // namespace

ZipReader::ZipReader(const QString& filePath) : filePath_(filePath) {}

bool ZipReader::isSafeRelativePath(const QString& path) {
  if (path.isEmpty() || path.size() > 1024) return false;
  // Absolu POSIX (`/Users/...`), UNC (`//hote/partage`) et `media//...` : une
  // barre de tete ou un segment vide suffit a sortir du dossier, parce que
  // `QDir::absoluteFilePath` rend un chemin absolu tel quel.
  if (path.startsWith(QLatin1Char('/'))) return false;
  if (path.contains(QStringLiteral("//"))) return false;
  // Antislash : separateur sous Windows, donc `media/..\\..\\x` y remonte.
  if (path.contains(QLatin1Char('\\'))) return false;
  // Deux-points : lettre de lecteur (`C:/...`, `C:x`) ou flux NTFS
  // (`image.png:flux`). Aucun nom produit par l'empaqueteur n'en contient.
  if (path.contains(QLatin1Char(':'))) return false;
  for (const QChar c : path) {
    if (c.unicode() < 0x20 || c.unicode() == 0x7F) return false;
  }
  const QStringList segments = path.split(QLatin1Char('/'));
  for (const QString& segment : segments) {
    if (segment.isEmpty() || segment == QStringLiteral(".") ||
        segment == QStringLiteral("..")) {
      return false;
    }
    if (isWindowsReservedName(segment)) return false;
    // Windows retire l'espace et le point finaux : `a.png.` et `a.png` sont le
    // meme fichier, et un nom qui change en route n'est plus celui verifie.
    if (segment.endsWith(QLatin1Char(' ')) || segment.endsWith(QLatin1Char('.'))) return false;
  }
  return true;
}

/**
 * Un nom d'entree est acceptable s'il ne peut designer QUE l'endroit prevu.
 *
 * Rappel de ce qui est refuse, et du symptome que chaque refus evite :
 *   - chemin absolu (`/etc/...`, `C:\\...`)  -> ecriture hors du dossier d'import ;
 *   - `..` n'importe ou                      -> remontee arborescente ;
 *   - antislash                              -> separateur Windows dans une archive,
 *                                               inoffensif sur un systeme, chemin
 *                                               valide sur l'autre ;
 *   - hors des prefixes attendus             -> le paquet decide de fichiers dont
 *                                               personne n'a besoin.
 *
 * Sous `media/`, c'est la SUITE du nom qui est verifiee (`isSafeRelativePath`) :
 * `media/C:/...` ou `media//Users/...` passaient les gardes appliquees au nom
 * entier, puis devenaient un chemin absolu une fois le prefixe retire.
 */
bool ZipReader::nameIsSafe(const QString& name) const {
  if (name.isEmpty() || name.size() > 1024) return false;
  if (name.contains(QLatin1Char('\\'))) return false;
  if (name.startsWith(QLatin1Char('/'))) return false;
  if (name.contains(QStringLiteral(".."))) return false;
  // Un nom Windows du type `C:quelquechose` n'a pas de barre oblique de tete mais
  // reste un chemin absolu pour l'API du systeme.
  if (name.size() > 1 && name.at(1) == QLatin1Char(':')) return false;
  if (name.endsWith(QLatin1Char('/'))) return false;  // dossier, rien a extraire

  if (name == QStringLiteral("collection.json") || name == QStringLiteral("manifest.json")) {
    return true;
  }
  return name.startsWith(QStringLiteral("media/")) && isSafeRelativePath(name.mid(6));
}

bool ZipReader::open() {
  QFile file(filePath_);
  if (!file.open(QIODevice::ReadOnly)) {
    lastError_ = loc("Zip.Unreadable").arg(file.errorString());
    return false;
  }

  const qint64 taille = file.size();
  if (taille < 22) {
    lastError_ = loc("Zip.TooShort");
    return false;
  }

  // Le catalogue central se trouve par la fin : on cherche la signature de
  // l'enregistrement de fin (EOCD) dans les derniers 64 Kio, ce que la
  // specification autorise (taille maximale du commentaire d'archive).
  const qint64 fenetre = qMin<qint64>(taille, 65557);
  if (!file.seek(taille - fenetre)) {
    lastError_ = loc("Zip.NotSeekable");
    return false;
  }
  const QByteArray queue = file.read(fenetre);

  int eocd = -1;
  for (int i = queue.size() - 22; i >= 0; --i) {
    if (le32(queue, i) == 0x06054b50) {
      eocd = i;
      break;
    }
  }
  if (eocd < 0) {
    lastError_ = loc("Zip.NotZip");
    return false;
  }

  const quint16 nombre = le16(queue, eocd + 10);
  if (nombre > kMaxEntries) {
    lastError_ = loc("Zip.TooManyEntries").arg(kMaxEntries);
    return false;
  }
  const quint32 tailleCatalogue = le32(queue, eocd + 12);
  const quint32 debutCatalogue = le32(queue, eocd + 16);

  if (debutCatalogue == 0xFFFFFFFFu || tailleCatalogue == 0xFFFFFFFFu) {
    // ZIP64 : `ZipWriter` n'en produit pas, et un paquet qui en contient vient
    // d'ailleurs. Refus explicite plutot qu'une lecture qui partirait de travers.
    lastError_ = loc("Zip.Zip64");
    return false;
  }
  if (!file.seek(debutCatalogue)) {
    lastError_ = loc("Zip.DirMissing");
    return false;
  }
  const QByteArray catalogue = file.read(tailleCatalogue);
  if (static_cast<quint64>(debutCatalogue) + tailleCatalogue > static_cast<quint64>(taille) ||
      catalogue.size() != static_cast<int>(tailleCatalogue)) {
    lastError_ = loc("Zip.DirInconsistent");
    return false;
  }

  // Plages occupees par chaque entree (en-tete local + donnees), pour refuser
  // les entrees qui se recouvrent : c'est la forme de la « bombe » sans
  // compression, ou mille entrees designent les memes octets et multiplient
  // d'autant ce qui s'ecrit sur le disque.
  struct Plage {
    qint64 debut;
    qint64 fin;
    QString nom;
  };
  QVector<Plage> plages;
  QSet<QString> nomsVus;
  quint64 totalDonnees = 0;

  int position = 0;
  for (quint16 index = 0; index < nombre; ++index) {
    if (position + 46 > catalogue.size() || le32(catalogue, position) != 0x02014b50) {
      lastError_ = loc("Zip.DirInconsistent");
      return false;
    }
    const quint16 methode = le16(catalogue, position + 10);
    const quint32 crc = le32(catalogue, position + 16);
    const quint32 tailleCompressee = le32(catalogue, position + 20);
    const quint32 tailleReelle = le32(catalogue, position + 24);
    const quint16 longueurNom = le16(catalogue, position + 28);
    const quint16 longueurExtra = le16(catalogue, position + 30);
    const quint16 longueurCommentaire = le16(catalogue, position + 32);
    const quint32 offsetLocal = le32(catalogue, position + 42);
    // Le nom est lu dans le catalogue : une longueur qui deborde ferait lire
    // au-dela du tampon.
    if (position + 46 + longueurNom + longueurExtra + longueurCommentaire > catalogue.size()) {
      lastError_ = loc("Zip.DirInconsistent");
      return false;
    }
    const QString nom =
        QString::fromUtf8(catalogue.constData() + position + 46, longueurNom);

    if (methode != 0) {
      // Une entree sautee en silence donnerait une collection restauree a
      // laquelle il manque un media, sans que personne ne sache lequel.
      lastError_ =
          loc("Zip.UnsupportedEntry").arg(nom);
      return false;
    }
    if (tailleCompressee != tailleReelle) {
      lastError_ = loc("Zip.BadSizes").arg(nom);
      return false;
    }
    if (!nameIsSafe(nom)) {
      lastError_ = loc("Zip.BadPath").arg(nom);
      return false;
    }
    // Doublon, casse comprise : sous Windows et macOS, `media/A.png` et
    // `media/a.png` designent le MEME fichier, le second ecraserait le premier.
    const QString cleNom = nom.toLower();
    if (nomsVus.contains(cleNom)) {
      lastError_ = loc("Zip.Duplicate").arg(nom);
      return false;
    }
    nomsVus.insert(cleNom);

    // L'en-tete local reprend le nom et l'extra : les donnees commencent apres.
    if (!file.seek(offsetLocal)) {
      lastError_ = loc("Zip.EntryMissing").arg(nom);
      return false;
    }
    const QByteArray enTeteLocal = file.read(30);
    if (enTeteLocal.size() < 30 || le32(enTeteLocal, 0) != 0x04034b50) {
      lastError_ = loc("Zip.BadHeader").arg(nom);
      return false;
    }
    const quint16 nomLocal = le16(enTeteLocal, 26);
    const quint16 extraLocal = le16(enTeteLocal, 28);

    Entry entry;
    entry.name = nom;
    entry.size = tailleReelle;
    entry.crc32 = crc;
    entry.dataOffset = static_cast<qint64>(offsetLocal) + 30 + nomLocal + extraLocal;

    // Les donnees doivent tenir AVANT le catalogue central : au-dela, elles
    // seraient lues dans le catalogue lui-meme, ou hors du fichier.
    const qint64 finDonnees = entry.dataOffset + static_cast<qint64>(entry.size);
    if (finDonnees > static_cast<qint64>(debutCatalogue)) {
      lastError_ = loc("Zip.BadSizes").arg(nom);
      return false;
    }
    plages.append({static_cast<qint64>(offsetLocal), finDonnees, nom});
    totalDonnees += entry.size;
    entries_.append(entry);

    position += 46 + longueurNom + longueurExtra + longueurCommentaire;
  }

  if (entries_.isEmpty()) {
    lastError_ = loc("Zip.Empty");
    return false;
  }

  // Entrees STOCKEES seulement (methode 0, verifiee plus haut) : ce qui sera
  // ecrit ne peut donc pas depasser la taille de l'archive. Une somme superieure
  // veut dire que des entrees partagent leurs octets.
  if (totalDonnees > static_cast<quint64>(taille)) {
    lastError_ = loc("Zip.DirInconsistent");
    entries_.clear();
    return false;
  }
  std::sort(plages.begin(), plages.end(),
            [](const Plage& a, const Plage& b) { return a.debut < b.debut; });
  for (int i = 1; i < plages.size(); ++i) {
    if (plages.at(i).debut < plages.at(i - 1).fin) {
      lastError_ = loc("Zip.Overlap").arg(plages.at(i).nom);
      entries_.clear();
      return false;
    }
  }
  return true;
}

bool ZipReader::contains(const QString& name) const {
  for (const Entry& entry : entries_) {
    if (entry.name == name) return true;
  }
  return false;
}

QByteArray ZipReader::read(const QString& name, qint64 maxBytes) {
  for (const Entry& entry : entries_) {
    if (entry.name != name) continue;
    if (entry.size > maxBytes) {
      lastError_ = loc("Zip.TooBig").arg(name);
      return {};
    }
    QFile file(filePath_);
    if (!file.open(QIODevice::ReadOnly) || !file.seek(entry.dataOffset)) {
      lastError_ = loc("Zip.ReadFailed").arg(name);
      return {};
    }
    const QByteArray data = file.read(entry.size);
    if (data.size() != static_cast<int>(entry.size)) {
      lastError_ = loc("Zip.ReadIncomplete").arg(name);
      return {};
    }
    if (crcUpdate(0, data.constData(), data.size()) != entry.crc32) {
      lastError_ = loc("Zip.Corrupted").arg(name);
      return {};
    }
    return data;
  }
  lastError_ = loc("Zip.Absent").arg(name);
  return {};
}

bool ZipReader::extractTo(const QString& name, const QString& destPath) {
  for (const Entry& entry : entries_) {
    if (entry.name != name) continue;

    QFile source(filePath_);
    if (!source.open(QIODevice::ReadOnly) || !source.seek(entry.dataOffset)) {
      lastError_ = loc("Zip.ReadFailed").arg(name);
      return false;
    }
    QDir().mkpath(QFileInfo(destPath).absolutePath());
    QFile dest(destPath);
    if (!dest.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
      lastError_ = loc("Zip.WriteFailed").arg(dest.errorString());
      return false;
    }

    // Sur le tas : 256 Ko sur la pile d'un fil de travail (dont la pile peut
    // etre petite, notamment sous macOS) est un debordement qui attend.
    std::vector<char> tampon(static_cast<size_t>(kChunk));
    char* const buffer = tampon.data();
    qint64 restant = entry.size;
    quint32 crc = 0;
    while (restant > 0) {
      const qint64 lu = source.read(buffer, qMin(restant, kChunk));
      if (lu <= 0) {
        lastError_ = loc("Zip.ReadInterruptedOn").arg(name);
        dest.close();
        QFile::remove(destPath);
        return false;
      }
      crc = crcUpdate(crc, buffer, lu);
      if (dest.write(buffer, lu) != lu) {
        lastError_ = loc("Zip.WriteInterruptedOn").arg(name);
        dest.close();
        QFile::remove(destPath);
        return false;
      }
      restant -= lu;
    }
    dest.flush();
    dest.close();

    if (crc != entry.crc32) {
      // Un media a moitie ecrit vaut moins qu'un media absent : le second se
      // voit dans OBS, le premier se decouvre a l'antenne.
      QFile::remove(destPath);
      lastError_ = loc("Zip.Corrupted").arg(name);
      return false;
    }
    return true;
  }
  lastError_ = loc("Zip.Absent").arg(name);
  return false;
}

}  // namespace semirprod
