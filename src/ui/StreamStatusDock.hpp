#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QWidget>

class QLabel;
class QTimer;

namespace semirprod {

class LiveClient;
class SemirprodDock;

/**
 * Dock OBS « Direct » — ce que l'operateur doit voir sans quitter son layout :
 * une alerte quand la diffusion se degrade, l'etat des trois liaisons, et les
 * chiffres du direct en cours.
 *
 * Vue pure : aucune logique metier ici. L'etat arrive par le signal
 * SemirprodDock::stateChanged (meme instantane que la WebUI), les etats de
 * liaison par les signaux de LiveClient, et la reception SRT est relue au
 * rythme du timer local puisqu'elle vient de libobs, pas d'un signal.
 */
class StreamStatusDock : public QWidget {
  Q_OBJECT

 public:
  explicit StreamStatusDock(SemirprodDock* dock, LiveClient* live,
                            QWidget* parent = nullptr);
  ~StreamStatusDock() override;

 private:
  void refresh();
  void onConnectionChanged(bool connected);
  void onPublishStateChanged(const QString& streamId, bool publishing);
  void onStateChanged(const QJsonObject& state);
  void applyHealthBanner();
  void applyStats();

  LiveClient* live_;

  QWidget* healthBanner_;
  QLabel* healthTitle_;
  QLabel* healthDetail_;

  QLabel* connDot_;
  QLabel* connText_;

  QLabel* publisherDot_;
  QLabel* publisherText_;

  QLabel* sourceDot_;
  QLabel* sourceText_;
  QLabel* dimensionsLabel_;

  QLabel* bitrateValue_;
  QLabel* droppedValue_;
  QLabel* congestionValue_;
  QLabel* uptimeValue_;
  QLabel* statsIdleLabel_;
  QWidget* statsGrid_;

  QTimer* timer_;

  bool wsConnected_ = false;
  bool publishing_ = false;

  // Dernier instantane pousse par le dock principal.
  bool healthAlert_ = false;
  QString healthReason_;
  bool streaming_ = false;
  bool statsActive_ = false;
  int statsKbps_ = 0;
  int droppedFrames_ = 0;
  int totalFrames_ = 0;
  double congestion_ = 0.0;
  QDateTime streamingStartedAt_;
};

} // namespace semirprod
