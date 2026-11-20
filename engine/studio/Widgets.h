// Widgets.h — small custom widgets of DSP Studio: band fader, level meter, response curve with
// draggable band points, and a labelled parameter slider. Colours come from the active palette, so
// every look (Modern / Qt, dark / light) is handled without any per-widget code.
#pragma once

#include <QSlider>
#include <QString>
#include <QWidget>

#include <vector>

#include "dsp/Spectrum.h"

class QLabel;
class QMouseEvent;
class QPaintEvent;
class QWheelEvent;

// Vertical fader for one EQ band: range -120..120 (tenths of a dB); double click = back to 0 dB.
class BandSlider : public QSlider
{
  Q_OBJECT
public:
  explicit BandSlider(QWidget* parent = nullptr);

protected:
  void mouseDoubleClickEvent(QMouseEvent* event) override;
};

// Vertical bar. Level mode: fills from the bottom, -60..0 dBFS, green/amber/red, with a peak marker.
// Reduction mode: fills from the top, 0..24 dB of gain reduction.
class MeterWidget : public QWidget
{
  Q_OBJECT
public:
  enum class Mode { Level, Reduction };
  explicit MeterWidget(Mode mode, QWidget* parent = nullptr);

  void setValues(float levelDb, float peakDb);  // Level: dBFS; Reduction: dB of reduction (>= 0)
  QSize sizeHint() const override { return QSize(14, 110); }
  QSize minimumSizeHint() const override { return QSize(10, 60); }

protected:
  void paintEvent(QPaintEvent* event) override;

private:
  Mode mode_;
  float level_;
  float peak_;
};

// What the response curve needs to know about one band.
struct CurveBand
{
  float db = 0.0f;       // gain (only meaningful for bell / shelves)
  float freq = 1000.0f;  // centre / corner frequency
  int type = 0;          // vac3dsp::BandType
};

// Frequency response (20 Hz .. 20 kHz, +-15 dB) of the shown EQ curve. Every band has a point on it:
//   click / drag up-down   = gain of the nearest band (bell and shelves)
//   Shift + drag sideways  = move the band's frequency (notch and cut filters: plain drag sideways)
//   mouse wheel            = width of the nearest band
//   double click           = gain back to 0
class CurveWidget : public QWidget
{
  Q_OBJECT
public:
  explicit CurveWidget(QWidget* parent = nullptr);

  void setCurve(const std::vector<double>& responseDb);    // vac3dsp::kCurvePoints values
  // Spectrum behind the curve: vac3dsp::kSpecBands dBFS values before (grey) and after (orange) the DSP chain.
  void setSpectrum(const std::vector<float>& inDb, const std::vector<float>& outDb);
  void setSpectrumVisible(bool on);
  void setBands(const std::vector<CurveBand>& bands, int selected);   // 30 bands
  void setEditable(bool editable) { editable_ = editable; }
  QSize sizeHint() const override { return QSize(640, 200); }
  QSize minimumSizeHint() const override { return QSize(320, 90); }

signals:
  void bandSelected(int band);
  void bandEdited(int band, float db);
  void bandFreqEdited(int band, float hz);
  void bandWheel(int band, int steps);   // +1 = wider, -1 = narrower
  void bandReset(int band);

protected:
  void paintEvent(QPaintEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void mouseDoubleClickEvent(QMouseEvent* event) override;
  void wheelEvent(QWheelEvent* event) override;

private:
  QRectF plotRect() const;
  double xForFreq(double hz) const;
  double freqForX(double x) const;
  double yForDb(double db) const;
  float dbForY(double y) const;
  int nearestBand(double x) const;

  std::vector<double> curve_;
  std::vector<float> specIn_, specOut_;
  bool specVisible_ = false;
  std::vector<CurveBand> bands_;
  int selected_ = 0;
  bool editable_ = true;
  int dragBand_ = -1;
  bool dragFreq_ = false;
};

// Six small spectra (FL FR / FC LFE / SL SR), before the DSP chain in grey and after it in orange: shows at a
// glance what the upmix put into the centre and the surrounds.
class SpectrumGrid : public QWidget
{
  Q_OBJECT
public:
  explicit SpectrumGrid(QWidget* parent = nullptr);
  void setSpectra(const float (*in)[vac3dsp::kSpecBands], const float (*out)[vac3dsp::kSpecBands]);
  QSize sizeHint() const override { return QSize(300, 300); }
  QSize minimumSizeHint() const override { return QSize(200, 120); }

protected:
  void paintEvent(QPaintEvent* event) override;

private:
  std::vector<float> in_, out_;   // 6 * kSpecBands each
};

// The small indicator under a band fader: shape tag ("B", "LS", "HS", "N", "HP", "LP") on top and a thin
// bar below it whose length shows the band's width. The selected band is outlined in the accent colour.
class BandBadge : public QWidget
{
  Q_OBJECT
public:
  explicit BandBadge(QWidget* parent = nullptr);
  void setShape(const QString& tag, float widthFraction, bool selected);
  QSize sizeHint() const override { return QSize(32, 22); }
  QSize minimumSizeHint() const override { return QSize(22, 20); }

signals:
  void clicked();

protected:
  void paintEvent(QPaintEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;

private:
  QString tag_;
  float width_ = 0.5f;
  bool selected_ = false;
};

// "Name   [=====o------]   -18.0 dB": a labelled slider with a value readout; linear or logarithmic.
class ParamSlider : public QWidget
{
  Q_OBJECT
public:
  ParamSlider(const QString& name, double minValue, double maxValue, const QString& unit, int decimals,
              bool logScale, QWidget* parent = nullptr);

  double value() const;
  void setValue(double value);   // does not emit valueChanged

signals:
  void valueChanged(double value);   // user moved the slider

private:
  double fromPos(int pos) const;
  int toPos(double value) const;
  void updateReadout();

  QLabel* name_;
  QLabel* readout_;
  QSlider* slider_;
  double min_, max_;
  QString unit_;
  int decimals_;
  bool log_;
};
