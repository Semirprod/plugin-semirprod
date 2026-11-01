#include "ZipWriter.hpp"

#include <QBuffer>
#include <QDateTime>
#include <QFileInfo>

#include <array>
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

void appendLE16(QByteArray& out, quint16 value) {
  out.append(static_cast<char>(value & 0xFF));
  out.append(static_cast<char>((value >> 8) & 0xFF));
}

void appendLE32(QByteArray& out, quint32 value) {
  out.append(static_cast<char>(value & 0xFF));
  out.append(static_cast<char>((value >> 8) & 0xFF));
  out.append(static_cast<char>((value >> 16) & 0xFF));
  out.append(static_cast<char>((value >> 24) & 0xFF));
}

/// Horodatage MS-DOS. Fige a une valeur STABLE et non a l'heure courante :
/// deux paquets du meme contenu doivent avoir la meme empreinte, sinon le
/// serveur ne peut pas reconnaitre un doublon et le client paie deux fois son
/// quota pour un habillage identique.
constexpr quint16 kDosTime = 0;   // 00:00:00
constexpr quint16 kDosDate = 33;  // 1980-01-01

/// Lecture par blocs. 256 Kio : assez grand pour que le disque travaille par
/// paquets, assez petit pour ne pas faire respirer la memoire d'OBS.
constexpr qint64 kChunk = 256 * 1024;

}  // namespace

ZipWriter::ZipWriter(const QString& filePath) : file_(filePath) {}

ZipWriter::~ZipWriter() {
  // Une archive jamais terminee est un fichier tronque, sans catalogue central :
  // aucun outil ne la relira. On l'efface plutot que de laisser un paquet mort
  // dans le dossier temporaire du client.
  if (file_.isOpen()) {
    file_.close();
    if (!finished_) file_.remove();
  }
}

bool ZipWriter::open() {
  if (!file_.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    lastError_ = loc("Zip.CreateFailed").arg(file_.errorString());
    return false;
  }
  return true;
}

bool ZipWriter::writeRaw(const char* data, qint64 length) {
  qint64 done = 0;
  while (done < length) {
    const qint64 n = file_.write(data + done, length - done);
    if (n <= 0) {
      lastError_ = loc("Zip.ArchiveWriteInterrupted").arg(file_.errorString());
      return false;
    }
    done += n;
  }
  written_ += length;
  return true;
}

bool ZipWriter::writeEntry(const QString& entryName, QIODevice& source, qint64 size,
                           quint32 crc) {
  if (size < 0 || size > 0xFFFFFFFFLL) {
    lastError_ = loc("Zip.FileTooLarge").arg(entryName);
    return false;
  }
  if (written_ + size > kMaxTotalBytes) {
    lastError_ = loc("Zip.PackageTooLarge")
                     .arg(kMaxTotalBytes / (1024.0 * 1024 * 1024), 0, 'f', 1);
    return false;
  }

  const QByteArray name = entryName.toUtf8();
  Entry entry;
  entry.name = entryName;
  entry.crc32 = crc;
  entry.size = static_cast<quint32>(size);
  entry.offset = static_cast<quint32>(written_);

  QByteArray header;
  appendLE32(header, 0x04034b50);  // signature d'en-tete local
  appendLE16(header, 20);          // version minimale (2.0)
  appendLE16(header, 0x0800);      // drapeau : nom de fichier en UTF-8
  appendLE16(header, 0);           // methode 0 = STORE
  appendLE16(header, kDosTime);
  appendLE16(header, kDosDate);
  appendLE32(header, crc);
  appendLE32(header, entry.size);  // taille compressee = taille reelle en STORE
  appendLE32(header, entry.size);
  appendLE16(header, static_cast<quint16>(name.size()));
  appendLE16(header, 0);  // pas de champ extra
  if (!writeRaw(header.constData(), header.size())) return false;
  if (!writeRaw(name.constData(), name.size())) return false;

  char buffer[kChunk];
  qint64 remaining = size;
  while (remaining > 0) {
    const qint64 read = source.read(buffer, qMin(remaining, kChunk));
    if (read <= 0) {
      lastError_ = loc("Zip.ReadInterrupted").arg(entryName);
      return false;
    }
    if (!writeRaw(buffer, read)) return false;
    remaining -= read;
  }

  entries_.append(entry);
  return true;
}

bool ZipWriter::addFile(const QString& entryName, const QString& sourcePath) {
  QFile source(sourcePath);
  if (!source.open(QIODevice::ReadOnly)) {
    lastError_ = loc("Zip.FileUnreadable").arg(sourcePath);
    return false;
  }
  const qint64 size = source.size();

  // Deux passes sur le fichier : une pour le CRC, une pour l'ecriture. Le format
  // veut le CRC dans l'en-tete, AVANT les octets, et l'alternative (le descripteur
  // de donnees, drapeau 0x08) est mal relue par plusieurs outils Windows.
  // Le cout est une relecture disque, pas une occupation memoire.
  quint32 crc = 0;
  char buffer[kChunk];
  while (true) {
    const qint64 read = source.read(buffer, kChunk);
    if (read < 0) {
      lastError_ = loc("Zip.ReadInterrupted").arg(sourcePath);
      return false;
    }
    if (read == 0) break;
    crc = crcUpdate(crc, buffer, read);
  }
  if (!source.seek(0)) {
    lastError_ = loc("Zip.NotRepositionable").arg(sourcePath);
    return false;
  }

  return writeEntry(entryName, source, size, crc);
}

bool ZipWriter::addBytes(const QString& entryName, const QByteArray& content) {
  QBuffer buffer;
  buffer.setData(content);
  if (!buffer.open(QIODevice::ReadOnly)) {
    lastError_ = loc("Zip.BufferUnreadable").arg(entryName);
    return false;
  }
  return writeEntry(entryName, buffer, content.size(), crcUpdate(0, content.constData(), content.size()));
}

bool ZipWriter::finish() {
  if (finished_) return true;

  const quint32 centralOffset = static_cast<quint32>(written_);
  QByteArray central;
  for (const Entry& entry : entries_) {
    const QByteArray name = entry.name.toUtf8();
    appendLE32(central, 0x02014b50);  // en-tete de catalogue
    appendLE16(central, 20);          // version d'ecriture
    appendLE16(central, 20);          // version minimale de lecture
    appendLE16(central, 0x0800);      // nom en UTF-8
    appendLE16(central, 0);           // STORE
    appendLE16(central, kDosTime);
    appendLE16(central, kDosDate);
    appendLE32(central, entry.crc32);
    appendLE32(central, entry.size);
    appendLE32(central, entry.size);
    appendLE16(central, static_cast<quint16>(name.size()));
    appendLE16(central, 0);  // extra
    appendLE16(central, 0);  // commentaire
    appendLE16(central, 0);  // disque
    appendLE16(central, 0);  // attributs internes
    appendLE32(central, 0);  // attributs externes
    appendLE32(central, entry.offset);
    central.append(name);
  }
  if (!writeRaw(central.constData(), central.size())) return false;

  QByteArray eocd;
  appendLE32(eocd, 0x06054b50);
  appendLE16(eocd, 0);  // disque courant
  appendLE16(eocd, 0);  // disque du catalogue
  appendLE16(eocd, static_cast<quint16>(entries_.size()));
  appendLE16(eocd, static_cast<quint16>(entries_.size()));
  appendLE32(eocd, static_cast<quint32>(central.size()));
  appendLE32(eocd, centralOffset);
  appendLE16(eocd, 0);  // pas de commentaire d'archive
  if (!writeRaw(eocd.constData(), eocd.size())) return false;

  if (!file_.flush()) {
    lastError_ = loc("Zip.NotFlushed").arg(file_.errorString());
    return false;
  }
  file_.close();
  finished_ = true;
  return true;
}

}  // namespace semirprod
