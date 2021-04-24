#pragma once
#include <map>

#include <QFrame>
#include <QPushButton>
#include <QLabel>
#include <QVBoxLayout>

#include "common/params.h"
#include "widgets/scrollview.hpp"

class OffroadAlert : public QFrame {
  Q_OBJECT

public:
  explicit OffroadAlert(QWidget *parent = 0);
  int alertCount = 0;
  bool updateAvailable;

private:
  void updateAlerts();

  Params params;
  std::map<std::string, QLabel*> alerts;

  QLabel releaseNotes;
  QPushButton rebootBtn;
  ScrollView *alertsScroll;
  ScrollView *releaseNotesScroll;
  QVBoxLayout *alerts_layout;

signals:
  void closeAlerts();

public slots:
  void refresh();
};
