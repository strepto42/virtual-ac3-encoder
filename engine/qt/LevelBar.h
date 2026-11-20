// LevelBar.h — thin vertical level meter for the control panel's faders: -60..0 dBFS, green / amber / red,
// faint scale marks and a peak marker. Values come from the engine's level block (see dsp/MeterShm.h).
#pragma once

#include <QWidget>

class LevelBar : public QWidget
{
public:
  explicit LevelBar(QWidget* parent = nullptr);
  void setValues(float levelDb, float peakDb);
  QSize sizeHint() const override { return QSize(12, 160); }
  QSize minimumSizeHint() const override { return QSize(8, 60); }

protected:
  void paintEvent(QPaintEvent* event) override;

private:
  float level_ = -90.0f;
  float peak_ = -90.0f;
};
