#pragma once

#include <QDateTime>
#include <QString>
#include <QWidget>

class QCheckBox;
class QLabel;
class QListWidget;

namespace semirprod {

class SemirprodDock;

/**
 * Dock OBS « Journal » — fil des evenements metier du plugin : demarrage et
 * arret de diffusion, bascules de scene, incidents reseau, reconnexions,
 * ajustements de debit.
 *
 * Volontairement distinct du log WS de la fenetre Studio, qui affiche les
 * trames brutes : ici on veut ce qu'un operateur peut relire apres coup pour
 * comprendre ce qui s'est passe pendant le direct, pas du protocole.
 *
 * Vue pure alimentee par SemirprodDock::eventLogged.
 */
class EventLogDock : public QWidget {
  Q_OBJECT

 public:
  explicit EventLogDock(SemirprodDock* dock, QWidget* parent = nullptr);
  ~EventLogDock() override;

 private:
  void appendEvent(const QDateTime& at, const QString& level,
                   const QString& message);
  void applyFilter();

  QListWidget* list_;
  QCheckBox* onlyIncidents_;
  QLabel* emptyLabel_;
};

} // namespace semirprod
