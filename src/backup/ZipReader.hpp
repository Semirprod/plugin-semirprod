#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace semirprod {

/**
 * Lecteur ZIP minimal, en miroir de `ZipWriter`.
 *
 * ─── Pourquoi un lecteur ecrit ici ─────────────────────────────────────────
 *
 * Qt sait lire une archive (`QZipReader`) mais cette classe est PRIVEE : elle
 * n'est pas exportee, donc indisponible pour un greffon. Ajouter miniz ou libzip
 * pour relire un conteneur qu'on ecrit soi-meme ferait entrer une dependance de
 * plus dans une chaine de compilation manuelle, sur trois plateformes.
 *
 * ─── Ce qu'il accepte, et ce qu'il refuse ─────────────────────────────────
 *
 * Il lit ce que `ZipWriter` produit : entrees STOCKEES (methode 0), sans ZIP64,
 * sans chiffrement. Une methode inconnue est REFUSEE avec un message, jamais
 * ignoree en silence : une entree sautee donnerait une collection restauree a
 * laquelle il manque un media, sans que personne ne sache lequel.
 *
 * ─── Les gardes de chemin, et pourquoi elles sont ICI ─────────────────────
 *
 * Un paquet est un fichier qui vient du reseau, et il decide de noms de fichiers
 * a ecrire sur le disque de l'operateur. Le lecteur refuse donc, pour chaque
 * entree : un chemin absolu, un `..`, un antislash (separateur Windows dans une
 * archive = tentative de sortir du dossier sur un systeme, chemin valide sur
 * l'autre), un nom vide, et tout ce qui n'est pas sous les prefixes attendus.
 *
 * La verification vit dans le lecteur et non chez l'appelant : c'est le seul
 * endroit par lequel TOUTES les entrees passent, donc le seul ou l'oubli est
 * impossible.
 */
class ZipReader {
public:
  struct Entry {
    QString name;
    quint32 size = 0;
    quint32 crc32 = 0;
    /** Position des octets dans le fichier, en-tete local deja franchi. */
    qint64 dataOffset = 0;
  };

  explicit ZipReader(const QString& filePath);

  /**
   * Lit le catalogue central et valide chaque nom.
   *
   * `false` = l'archive est refusee, et `lastError()` dit pourquoi en francais :
   * ce message finit devant l'operateur.
   */
  bool open();

  const QList<Entry>& entries() const { return entries_; }
  bool contains(const QString& name) const;

  /**
   * Rend le contenu d'une entree, verifie par son CRC.
   *
   * `maxBytes` borne ce qu'on accepte de charger en memoire : reserve aux petits
   * fichiers (le JSON de collection, le manifeste). Pour un media, voir
   * `extractTo`.
   */
  QByteArray read(const QString& name, qint64 maxBytes);

  /**
   * Ecrit une entree dans un fichier, par blocs, sans la charger en memoire.
   *
   * Verifie le CRC en cours de route et supprime le fichier si l'archive mentait
   * — un media a moitie ecrit vaut moins qu'un media absent, parce que le second
   *  se voit.
   */
  bool extractTo(const QString& name, const QString& destPath);

  const QString& lastError() const { return lastError_; }

  /**
   * Vrai si `path` est un chemin RELATIF qui ne peut designer qu'un fichier
   * sous le dossier ou on le pose : ni barre de tete, ni `//`, ni antislash,
   * ni deux-points (lecteur Windows, flux NTFS), ni segment vide, `.` ou `..`.
   *
   * Purement syntaxique : c'est la meme regle pour le nom d'une entree sous
   * `media/` et pour un chemin `media/...` lu dans le JSON de collection
   * (CollectionImporter, qui y ajoute la verification de confinement).
   */
  static bool isSafeRelativePath(const QString& path);

private:
  /** Vrai si ce nom d'entree est acceptable. Voir l'en-tete de la classe. */
  bool nameIsSafe(const QString& name) const;

  QString filePath_;
  QList<Entry> entries_;
  QString lastError_;
};

}  // namespace semirprod
