#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QStackedLayout>
#include <QVBoxLayout>

#include "api.hpp"
#include "common/params.h"
#include "drive_stats.hpp"

const double MILE_TO_KM = 1.60934;

void clearLayouts(QLayout* layout) {
  while (QLayoutItem* item = layout->takeAt(0)) {
    if (QWidget* widget = item->widget()) {
      widget->deleteLater();
    }
    if (QLayout* childLayout = item->layout()) {
      clearLayouts(childLayout);
    }
    delete item;
  }
}

QLayout* build_stat(QString name, int stat) {
  QVBoxLayout* layout = new QVBoxLayout;
  layout->setMargin(0);

  QLabel* metric = new QLabel(QString("%1").arg(stat));
  metric->setStyleSheet(R"(
    font-size: 80px;
    font-weight: 600;
  )");
  layout->addWidget(metric, 0, Qt::AlignLeft);

  QLabel* label = new QLabel(name);
  label->setStyleSheet(R"(
    font-size: 45px;
    font-weight: 500;
  )");
  layout->addWidget(label, 0, Qt::AlignLeft);

  return layout;
}

// void DriveStats::parseError(QString response) {
//   clearLayouts(vlayout);
//   vlayout->addWidget(new QLabel("No Internet connection"), 0, Qt::AlignCenter);
// }

void DriveStats::parseResponse(QString response, bool save) {
  static QString prev_response;

  response = response.trimmed();
  if (prev_response == response) {
    return;
  }

  prev_response = response;

  clearLayouts(vlayout);
  QJsonDocument doc = QJsonDocument::fromJson(response.toUtf8());
  if (doc.isNull()) {
    qDebug() << "JSON Parse failed on getting past drives statistics";
    return;
  }
  if (save) {
    Params().write_db_value("DriveStats", response.toStdString());
  }

  bool metric = Params().read_db_bool("IsMetric");

  QJsonObject json = doc.object();
  auto all = json["all"].toObject();
  auto week = json["week"].toObject();

  QGridLayout* gl = new QGridLayout();
  gl->setMargin(0);

  int all_distance = all["distance"].toDouble() * (metric ? MILE_TO_KM : 1);
  gl->addWidget(new QLabel("ALL TIME"), 0, 0, 1, 3);
  gl->addLayout(build_stat("DRIVES", all["routes"].toDouble()), 1, 0, 3, 1);
  gl->addLayout(build_stat(metric ? "KM" : "MILES", all_distance), 1, 1, 3, 1);
  gl->addLayout(build_stat("HOURS", all["minutes"].toDouble() / 60), 1, 2, 3, 1);

  int week_distance = week["distance"].toDouble() * (metric ? MILE_TO_KM : 1);
  gl->addWidget(new QLabel("PAST WEEK"), 6, 0, 1, 3);
  gl->addLayout(build_stat("DRIVES", week["routes"].toDouble()), 7, 0, 3, 1);
  gl->addLayout(build_stat(metric ? "KM" : "MILES", week_distance), 7, 1, 3, 1);
  gl->addLayout(build_stat("HOURS", week["minutes"].toDouble() / 60), 7, 2, 3, 1);

  QWidget* q = new QWidget;
  q->setLayout(gl);
  vlayout->addWidget(q);
}

DriveStats::DriveStats(QWidget* parent) : QWidget(parent) {
  vlayout = new QVBoxLayout(this);
  vlayout->setMargin(0);
  setLayout(vlayout);
  setStyleSheet(R"(
    QLabel {
      font-size: 48px;
      font-weight: 500;
    }
  )");

  QString cached_stat = QString::fromStdString(Params().get("DriveStats"));
  if (cached_stat.isEmpty()) {
    cached_stat = R"({"all":{"distance":0.0,"minutes":0,"routes":0},"week":{"distance":0.0,"minutes":0,"routes":0}})";
  }
  parseResponse(cached_stat, false);

  // TODO: do we really need to update this frequently?
  QString dongleId = QString::fromStdString(Params().get("DongleId"));
  QString url = "https://api.commadotai.com/v1.1/devices/" + dongleId + "/stats";
  RequestRepeater* repeater = new RequestRepeater(this, url, 13);
  QObject::connect(repeater, SIGNAL(receivedResponse(QString)), this, SLOT(parseResponse(QString)));
  // QObject::connect(repeater, SIGNAL(failedResponse(QString)), this, SLOT(parseError(QString)));
}
