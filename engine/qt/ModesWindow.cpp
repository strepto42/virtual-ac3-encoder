#include "ModesWindow.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QVBoxLayout>

ModesWindow::ModesWindow(bool startHidden) : PanelWindow(startHidden)
{
  init();
  resize(820, 540);
  setMinimumSize(780, 500);
}

void ModesWindow::buildLayout(QWidget* central)
{
  auto* root = new QHBoxLayout(central);
  root->setContentsMargins(0, 0, 0, 0);
  root->setSpacing(0);

  // ---- left: modes
  auto* side = new QFrame;
  side->setObjectName(QStringLiteral("side"));
  side->setFixedWidth(250);
  auto* sideLayout = new QVBoxLayout(side);
  sideLayout->setContentsMargins(8, 12, 8, 12);
  sideLayout->setSpacing(6);
  auto* modeTitle = new QLabel(QStringLiteral("РЕЖИМ"));
  modeTitle->setProperty("role", "dim");
  sideLayout->addWidget(modeTitle);
  modeList_ = new QListWidget;
  for (const ModeInfo& mode : kModes)
    modeList_->addItem(QString::fromUtf8(mode.title) + QStringLiteral("\n") + QString::fromUtf8(mode.subtitle) +
                       QStringLiteral("  [") + QString::fromLatin1(mode.badge) + QStringLiteral("]"));
  connect(modeList_, &QListWidget::currentRowChanged, this, [this](int row) {
    if (row >= 0) modeCombo_->setCurrentIndex(row);  // the combo holds the real selection (hidden in B)
  });
  sideLayout->addWidget(modeList_, 1);
  root->addWidget(side);

  // The combo is the model; this variant shows the list instead.
  modeCombo_->hide();
  for (QPushButton* mute : mutes_) mute->hide();

  // ---- right
  auto* right = new QVBoxLayout;
  right->setContentsMargins(16, 14, 16, 14);
  right->setSpacing(12);

  auto* head = new QHBoxLayout;
  head->setSpacing(8);
  head->addWidget(stateDot_);
  head->addWidget(stateText_);
  head->addStretch(1);
  head->addWidget(dspBtn_);
  head->addWidget(themeBtn_);
  right->addLayout(head);

  auto* devices = new QGridLayout;
  devices->setHorizontalSpacing(16);
  devices->setVerticalSpacing(4);
  auto* inLabel = new QLabel(QStringLiteral("Вход"));
  inLabel->setProperty("role", "dim");
  auto* outLabel = new QLabel(QStringLiteral("Выход"));
  outLabel->setProperty("role", "dim");
  devices->addWidget(inLabel, 0, 0);
  devices->addWidget(outLabel, 0, 1);
  devices->addWidget(inputCombo_, 1, 0);
  devices->addWidget(outputCombo_, 1, 1);
  right->addLayout(devices);

  auto* levels = new QFrame;
  levels->setObjectName(QStringLiteral("card"));
  auto* grid = new QGridLayout(levels);
  grid->setContentsMargins(14, 12, 14, 12);
  grid->setHorizontalSpacing(20);
  grid->setVerticalSpacing(10);
  for (int i = 0; i < 6; ++i)
  {
    const int row = i / 2;
    const int column = i % 2;
    auto* cell = new QHBoxLayout;
    cell->setSpacing(8);
    auto* name = new QLabel(QString::fromLatin1(kChannelNames[i]));
    name->setProperty("role", "title");
    name->setMinimumWidth(30);
    values_[i]->setMinimumWidth(52);
    values_[i]->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    cell->addWidget(name);
    cell->addWidget(sliders_[i], 1);
    cell->addWidget(values_[i]);
    grid->addLayout(cell, row, column);
  }
  right->addWidget(levels);

  auto* options = new QGridLayout;
  options->setHorizontalSpacing(16);
  options->setVerticalSpacing(4);
  stereoLabel_->setProperty("role", "dim");
  delayLabel_->setProperty("role", "dim");
  options->addWidget(stereoLabel_, 0, 0);
  options->addWidget(delayLabel_, 0, 1);
  options->addWidget(stereoCombo_, 1, 0);
  options->addWidget(delayCombo_, 1, 1);
  right->addLayout(options);

  auto* extra = new QHBoxLayout;
  extra->addWidget(autostartCheck_);
  extra->addStretch(1);
  extra->addWidget(resetBtn_);
  right->addLayout(extra);

  right->addStretch(1);
  right->addWidget(statusLabel_);

  auto* foot = new QHBoxLayout;
  foot->setSpacing(10);
  foot->addStretch(1);
  foot->addWidget(startStopBtn_);
  foot->addWidget(applyBtn_);
  right->addLayout(foot);

  root->addLayout(right, 1);
}

void ModesWindow::modeChangedHook(int mode)
{
  if (!modeList_ || mode < 0) return;
  QSignalBlocker blocker(modeList_);
  modeList_->setCurrentRow(mode);
}
