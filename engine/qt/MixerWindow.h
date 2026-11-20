// MixerWindow.h — variant A: compact "mixer" layout, six vertical faders.
#pragma once

#include "PanelWindow.h"

#include <QElapsedTimer>

#include "dsp/MeterMath.h"
#include "dsp/MeterShm.h"

class LevelBar;
class QTimer;

class MixerWindow : public PanelWindow
{
public:
  explicit MixerWindow(bool startHidden);

protected:
  void buildLayout(QWidget* central) override;
  Qt::Orientation sliderOrientation() const override { return Qt::Vertical; }
  QString variantName() const override { return QStringLiteral("A"); }

private:
  void pollMeters();   // level bars next to the faders: what the engine really sends to the encoder

  LevelBar* bars_[6] = {};
  vac3dsp::MeterShmClient shm_;
  vac3dsp::MeterBallistics ball_[6];
  QTimer* meterTimer_ = nullptr;
  QElapsedTimer sinceTick_;
  uint32_t lastUpdates_ = 0;
  int openTick_ = 0;
};
