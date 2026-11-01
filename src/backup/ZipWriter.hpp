#pragma once

#include <QByteArray>
#include <QFile>
#include <QString>
#include <QVector>

namespace semirprod {

/**
 * Ecrivain ZIP minimal, en mode STORE (aucune compression).
 *
 * ─── Pourquoi ecrire un ZIP a la main plutot qu'ajouter une bibliotheque ────
 *
 * Ce plugin ne depend que de Qt, libobs et l'API frontend d'OBS. Qt sait LIRE
 * une archive (QZipReader) mais son ecrivain est prive, donc indisponible pour
 * un greffon. Ajouter miniz ou libzip pour ecrire un conteneur dont le format
 * tient en deux en-tetes ferait entrer une dependance de plus dans une chaine de
 * compilation manuelle, sur trois plateformes, pour un gain nul.
 *
 * ─── Pourquoi STORE, et non deflate ────────────────────────────────────────
 *
 * Le contenu d'un paquet de collection est deja compresse : PNG, JPEG, MP4, WEBM,
 * OGG. Les recompresser coute du temps de calcul PENDANT une session OBS — donc
 * potentiellement pendant un direct — et ne gagne presque rien. Seul le JSON de
 * collection se compresserait bien, et il pese quelques centaines de kilooctets
 * dans un paquet qui en pese des centaines de milliers.
 *
 * Consequence a connaitre : le paquet fait la taille de la somme de ses
 * fichiers, et c'est cette taille qui est reservee sur le quota.
 *
 * ─── Limite assumee : pas de ZIP64 ─────────────────────────────────────────
 *
 * Les champs de taille du format d'origine tiennent sur 32 bits. Un paquet est
 * donc refuse au-dela de `kMaxTotalBytes`, avec un message explicite, plutot que
 * d'ecrire une archive silencieusement corrompue qu'aucun outil ne relira. Le
 * plafond commercial le plus haut (10 Gio) est au-dessus de cette limite : c'est
 * un plafond de STOCKAGE cumule, pas de paquet unique, et une collection de
 * scenes de 4 Gio n'a jamais ete observee.
 */
class ZipWriter {
public:
  /** Au-dela, on refuse : le format sans ZIP64 ne sait plus decrire les tailles. */
  static constexpr qint64 kMaxTotalBytes = 3500LL * 1024 * 1024;

  explicit ZipWriter(const QString& filePath);
  ~ZipWriter();

  ZipWriter(const ZipWriter&) = delete;
  ZipWriter& operator=(const ZipWriter&) = delete;

  bool open();

  /**
   * Ajoute un fichier du disque sous le nom `entryName` (toujours relatif, avec
   * des `/`). Lu par blocs : un media de 800 Mio ne passe jamais par la memoire.
   */
  bool addFile(const QString& entryName, const QString& sourcePath);

  /** Ajoute un contenu deja en memoire (le JSON de collection, le manifeste). */
  bool addBytes(const QString& entryName, const QByteArray& content);

  /** Ecrit le catalogue central et ferme. Sans lui, l'archive est illisible. */
  bool finish();

  const QString& lastError() const { return lastError_; }
  qint64 bytesWritten() const { return written_; }

private:
  struct Entry {
    QString name;
    quint32 crc32 = 0;
    quint32 size = 0;
    quint32 offset = 0;
  };

  bool writeEntry(const QString& entryName, QIODevice& source, qint64 size,
                  quint32 crc);
  bool writeRaw(const char* data, qint64 length);

  QFile file_;
  QVector<Entry> entries_;
  qint64 written_ = 0;
  QString lastError_;
  bool finished_ = false;
};

}  // namespace semirprod
