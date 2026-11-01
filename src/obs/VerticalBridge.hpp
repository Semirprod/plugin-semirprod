#pragma once

#include <QObject>
#include <QString>

// Type C de libobs (callback/calldata.h), declare hors du namespace.
struct calldata;

namespace semirprod {

// Pilote le plugin Semirprod Vertical, s'il est installe, par le proc handler
// global de libobs (contrat : src/VerticalApi.hpp du depot
// semirprod-obs-vertical).
//
// Aucune dependance de compilation entre les deux plugins : si Vertical est
// absent, proc_handler_call rend false et `available` reste faux. Chacun marche
// seul ; le Studio ne fait que relayer ce que le site demande.
//
// Actif sur l'OBS cloud seulement (edition::bridgesEnabled, Edition.hpp).
// Ailleurs, le pont repond comme si Vertical etait absent : `available` faux,
// pas de capacite annoncee.
class VerticalBridge : public QObject {
  Q_OBJECT

 public:
  struct State {
    bool available = false;   // Vertical installe et charge
    int api = 0;              // version du contrat (2 = apercu WebRTC)
    bool streaming = false;
    bool configured = false;  // destination posee
    bool followStream = false;
    QString server;           // jamais la cle
    QString lastError;        // dernier echec du direct vertical, vide sinon
  };

  explicit VerticalBridge(QObject* parent = nullptr);
  ~VerticalBridge() override;

  State state() const;

  // La cle n'est ni journalisee ni conservee ici : elle part directement chez
  // Vertical, qui la range dans le profil OBS comme le fait OBS pour sa
  // propre destination.
  bool setDestination(const QString& server, const QString& key,
                      int bitrateKbps, bool followStream, QString* error);
  bool startStream(QString* error);
  void stopStream();

  // Apercu WebRTC du canvas vertical (contrat v2). L'URL a deja ete verifiee
  // contre la liste d'hotes autorises : Vertical ne la controle plus que sur
  // son schema. La sortie vit chez Vertical, qui la coupe lui-meme quand son
  // canvas disparait ; ici on ne tient que le bail.
  bool startPreview(const QString& whipUrl, const QString& bearerToken,
                    int width, int height, int fps, int bitrateKbps,
                    QString* error);
  void stopPreview();

  // Vertical charge ET assez recent pour l'apercu : condition de l'annonce
  // `vertical_preview_whip`.
  static bool previewSupported();

 signals:
  // Fil UI. Emis quand Vertical signale un demarrage, un arret ou un echec.
  void stateChanged();

 private:
  static void onVerticalState(void* data, ::calldata* cd);

  QString lastError_;
  bool connected_ = false;
};

}  // namespace semirprod
