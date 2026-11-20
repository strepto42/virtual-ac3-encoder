#include "MixerWindow.h"

#include <QCheckBox>
#include <QTimer>
#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>

#include <algorithm>

#include "LevelBar.h"

MixerWindow::MixerWindow(bool startHidden) : PanelWindow(startHidden)
{
  init();
  resize(680, 520);
  setMinimumSize(640, 480);

  meterTimer_ = new QTimer(this);
  meterTimer_->setInterval(33);
  connect(meterTimer_, &QTimer::timeout, this, [this]() { pollMeters(); });
  sinceTick_.start();
  meterTimer_->start();
}

void MixerWindow::pollMeters()
{
  const float dt = std::min(0.25f, static_cast<float>(sinceTick_.restart()) / 1000.0f);
  if (!shm_.IsOpen() && (++openTick_ % 15) == 0)
    shm_.Open();   // the engine creates the block when it starts; try about twice a second
  vac3dsp::DspMeterBlock b;
  bool fresh = false;
  if (shm_.IsOpen() && shm_.Read(b) && b.updates != lastUpdates_)
  {
    lastUpdates_ = b.updates;
    fresh = true;
  }
  for (int c = 0; c < 6; ++c)
  {
    ball_[c].Update(fresh ? vac3dsp::PeakToDb(b.finalPeak[c]) : vac3dsp::kMeterFloorDb, dt);
    bars_[c]->setValues(ball_[c].level(), ball_[c].peak());
  }
}

void MixerWindow::buildLayout(QWidget* central)
{
  auto* root = new QVBoxLayout(central);
  root->setContentsMargins(14, 12, 14, 12);
  root->setSpacing(10);

  // Header: state, mode, Apply, Start/Stop
  auto* head = new QHBoxLayout;
  head->setSpacing(8);
  head->addWidget(stateDot_);
  head->addWidget(stateText_);
  head->addWidget(modeCombo_, 1);
  head->addWidget(applyBtn_);
  head->addWidget(startStopBtn_);
  root->addLayout(head);

  // Mixer: six vertical faders
  auto* mixer = new QFrame;
  mixer->setObjectName(QStringLiteral("mixer"));
  auto* strips = new QHBoxLayout(mixer);
  strips->setContentsMargins(8, 12, 8, 10);
  strips->setSpacing(4);
  for (int i = 0; i < 6; ++i)
  {
    auto* strip = new QVBoxLayout;
    strip->setSpacing(6);
    values_[i]->setAlignment(Qt::AlignHCenter);
    strip->addWidget(values_[i], 0, Qt::AlignHCenter);
    bars_[i] = new LevelBar;
    auto* pair = new QHBoxLayout;   // fader and its level bar side by side
    pair->setSpacing(5);
    pair->addStretch(1);
    pair->addWidget(sliders_[i]);
    pair->addWidget(bars_[i]);
    pair->addStretch(1);
    strip->addLayout(pair, 1);
    auto* name = new QLabel(QString::fromLatin1(kChannelNames[i]));
    name->setProperty("role", "title");
    strip->addWidget(name, 0, Qt::AlignHCenter);
    strip->addWidget(mutes_[i], 0, Qt::AlignHCenter);
    strips->addLayout(strip, 1);
  }
  root->addWidget(mixer, 1);

  // Options row
  auto* options = new QHBoxLayout;
  options->setSpacing(8);
  options->addWidget(stereoLabel_);
  options->addWidget(stereoCombo_);
  options->addSpacing(10);
  options->addWidget(delayLabel_);
  options->addWidget(delayCombo_);
  options->addStretch(1);
  options->addWidget(resetBtn_);
  root->addLayout(options);

  // Devices (collapsible) + autostart + theme
  auto* devices = new QFrame;
  devices->setObjectName(QStringLiteral("card"));
  auto* deviceGrid = new QHBoxLayout(devices);
  deviceGrid->setContentsMargins(10, 8, 10, 8);
  deviceGrid->setSpacing(8);
  auto* inLabel = new QLabel(QStringLiteral("Вход:"));
  auto* outLabel = new QLabel(QStringLiteral("Выход:"));
  deviceGrid->addWidget(inLabel);
  deviceGrid->addWidget(inputCombo_, 1);
  deviceGrid->addWidget(outLabel);
  deviceGrid->addWidget(outputCombo_, 1);
  devices->setVisible(false);

  auto* footer = new QHBoxLayout;
  footer->setSpacing(8);
  auto* devicesBtn = new QPushButton(QStringLiteral("Устройства…"));
  devicesBtn->setCheckable(true);
  connect(devicesBtn, &QPushButton::toggled, this, [devices](bool on) { devices->setVisible(on); });
  footer->addWidget(devicesBtn);
  footer->addWidget(autostartCheck_);
  footer->addStretch(1);
  footer->addWidget(dspBtn_);
  footer->addWidget(themeBtn_);
  root->addWidget(devices);
  root->addLayout(footer);

  root->addWidget(statusLabel_);
}
