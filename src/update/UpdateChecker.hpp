#pragma once

#include <QObject>
#include <QString>

class QJsonDocument;
class QTimer;

namespace semirprod {

class HttpClient;
struct HttpResponse;

/**
 * Update manifest checker.
 *
 * Security model (v2) — signature ASYMÉTRIQUE :
 *   Le backend signe chaque manifeste en ECDSA P-256 / SHA-256 avec une clé
 *   privée qui ne quitte jamais son infrastructure. Le plugin ne porte que la
 *   clé PUBLIQUE (Config::kUpdateSignerPubKeyB64) et vérifie via CNG
 *   (bcrypt.dll) — aucune bibliothèque de crypto embarquée.
 *
 *   La v1 (HMAC-SHA256, secret partagé) est RETIRÉE, pas dépréciée : le plugin
 *   étant distribué et payant, un secret symétrique compilé dedans était
 *   détenu par chaque client, extractible de `.rdata`, et donc suffisant pour
 *   forger un manifeste. Laisser la v1 en repli aurait offert une
 *   rétrogradation triviale, d'où l'égalité STRICTE sur le numéro de format.
 *
 *   Message canonique (séparé par des `\n`, dans cet ordre, `\n` final inclus).
 *   Le premier champ est le numéro de format : séparation de domaine, pour
 *   qu'une signature d'un format ne puisse pas être rejouée sur un autre.
 *     2\n<version>\n<downloadUrl>\n<notes>\n<severity>\n<sha256>\n<sizeBytes>\n
 *
 *   Manifest JSON format:
 *     {
 *       "version":    "0.1.6",
 *       "downloadUrl":"https://app.semirprod.fr/downloads/plugin-0.1.6.zip",
 *       "notes":      "...",
 *       "severity":   "info" | "recommended" | "critical",
 *       "sha256":     "<64 hex minuscules, artefact designe par downloadUrl>",
 *       "sizeBytes":  2582122,
 *       "signature":  {
 *         "alg": "ECDSA-P256-SHA256",
 *         "v":   2,
 *         "sig": "<base64 de 64 octets : r||s, IEEE P-1363, PAS du DER>"
 *       }
 *     }
 *
 *   `sha256` et `sizeBytes` sont DANS le message signé. En v1, `sizeBytes`
 *   vivait à côté de la signature sans être couvert par elle : réécrivable à
 *   volonté, donc sans valeur comme contrôle d'intégrité. Le plugin ne
 *   télécharge pas lui-même (il ouvre l'URL dans le navigateur), il ne peut
 *   donc pas vérifier le fichier ; l'empreinte sert de référence authentifiée
 *   pour le client et le support — ce qui compte pour un binaire distribué
 *   sans signature Authenticode.
 *
 *   Le format signe est decrit ci-dessus (canonicalMessage dans
 *   UpdateChecker.cpp).
 *
 * The downloadUrl MUST be HTTPS and on the Semirprod allowlist even when
 * the signature is valid — a signed-but-hostile URL would still let an
 * attacker compromise the user via a malicious payload, so the host
 * check is the first line of defense.
 */
class UpdateChecker : public QObject {
  Q_OBJECT

public:
  explicit UpdateChecker(HttpClient* http, QObject* parent = nullptr);

  // Premiere verification tout de suite, puis environ toutes les 6 h. Apres un
  // echec, reprise plus rapprochee et bornee : une panne au demarrage n'attend
  // pas 6 h. Une seule requete a la fois, jamais de reentrance.
  void start();
  // Fin definitive : plus aucune requete ni reponse traitee (sortie d'OBS).
  void stop();
  // Redemande le manifeste tout de suite, hors du rythme du timer (version
  // bloquee, manifeste pas encore arrive). Sans effet si une requete est en vol.
  void check();

signals:
  // severity: "info" | "recommended" | "critical"
  // critical = breaking change → modal bloquante côté UI.
  void updateAvailable(const QString& latestVersion,
                       const QString& downloadUrl,
                       const QString& notes,
                       const QString& severity);
  // Manifeste SIGNE et valide dont la derniere version n'est pas plus recente que
  // celle qui tourne. Seul signal qui autorise a retirer une offre : une panne, une
  // signature fausse ou un contenu illisible n'emettent rien, l'etat ne bouge pas.
  void noUpdateAvailable();

private:
  enum class CheckOutcome {
    Verified,       // manifeste signe, valide : la reprise repart de zero
    Failed,         // reseau, statut, signature, contenu : nouvel essai bientot
    FailedNoRetry,  // reessayer ne changerait rien : cycle normal
  };

  CheckOutcome handleManifest(const HttpResponse& res, const QJsonDocument& doc);
  void scheduleNext(CheckOutcome outcome);
  static bool isNewer(const QString& candidate, const QString& current);

  HttpClient* http_;
  QTimer* timer_;
  bool inFlight_ = false;
  bool stopped_ = false;
  int failures_ = 0;
};

} // namespace semirprod
