#include "Widgets.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPolygonF>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

#include "dsp/DspParams.h"
#include "dsp/Spectrum.h"
#include "dsp/StudioModel.h"

// ------------------------------------------------------------------------------ BandSlider
BandSlider::BandSlider(QWidget* parent) : QSlider(Qt::Vertical, parent)
{
  setRange(-120, 120);
  setSingleStep(5);
  setPageStep(10);
  setValue(0);
  setMinimumHeight(90);
}

void BandSlider::mouseDoubleClickEvent(QMouseEvent* event)
{
  setValue(0);
  event->accept();
}

// ------------------------------------------------------------------------------ MeterWidget
MeterWidget::MeterWidget(Mode mode, QWidget* parent)
    : QWidget(parent), mode_(mode), level_(mode == Mode::Level ? -90.0f : 0.0f),
      peak_(mode == Mode::Level ? -90.0f : 0.0f)
{
}

void MeterWidget::setValues(float levelDb, float peakDb)
{
  if (std::fabs(levelDb - level_) < 0.05f && std::fabs(peakDb - peak_) < 0.05f)
    return;
  level_ = levelDb;
  peak_ = peakDb;
  update();
}

void MeterWidget::paintEvent(QPaintEvent*)
{
  QPainter p(this);
  const QPalette& pal = palette();
  const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
  p.setPen(QPen(pal.color(QPalette::Mid), 1.0));
  p.setBrush(pal.color(QPalette::Base));
  p.drawRoundedRect(r, 2.0, 2.0);
  const QRectF in = r.adjusted(1.5, 1.5, -1.5, -1.5);

  if (mode_ == Mode::Level)
  {
    const double lo = -60.0, hi = 0.0;
    auto yFor = [&](double db) {
      const double f = (std::min(hi, std::max(lo, db)) - lo) / (hi - lo);
      return in.bottom() - f * in.height();
    };
    struct Seg { double from, to; QColor color; };
    const Seg segs[] = {{-60.0, -18.0, QColor(46, 190, 100)},
                        {-18.0, -6.0, QColor(235, 190, 50)},
                        {-6.0, 0.0, QColor(230, 70, 70)}};
    for (const Seg& s : segs)
    {
      const double top = std::min<double>(s.to, level_);
      if (top <= s.from)
        continue;
      p.fillRect(QRectF(in.left(), yFor(top), in.width(), yFor(s.from) - yFor(top)), s.color);
    }
    QColor tick = pal.color(QPalette::Text);   // faint scale marks: -6, -12, -18, -30, -45 dB
    tick.setAlpha(55);
    p.setPen(QPen(tick, 1.0));
    for (double db : {-6.0, -12.0, -18.0, -30.0, -45.0})
      p.drawLine(QPointF(in.left(), yFor(db)), QPointF(in.right(), yFor(db)));
    if (peak_ > lo)
    {
      p.setPen(QPen(peak_ > -6.0f ? QColor(255, 120, 120) : pal.color(QPalette::Text), 2.0));
      const double y = yFor(peak_);
      p.drawLine(QPointF(in.left(), y), QPointF(in.right(), y));
    }
  }
  else
  {
    const double depth = std::min(24.0, std::max(0.0, static_cast<double>(level_)));
    const double h = depth / 24.0 * in.height();
    p.fillRect(QRectF(in.left(), in.top(), in.width(), h), QColor(255, 150, 40));
    QColor tick = pal.color(QPalette::Text);   // faint scale marks: 6, 12, 18 dB of reduction
    tick.setAlpha(55);
    p.setPen(QPen(tick, 1.0));
    for (double db : {6.0, 12.0, 18.0})
      p.drawLine(QPointF(in.left(), in.top() + db / 24.0 * in.height()), QPointF(in.right(), in.top() + db / 24.0 * in.height()));
    const double pk = std::min(24.0, std::max(0.0, static_cast<double>(peak_)));
    if (pk > 0.1)
    {
      p.setPen(QPen(pal.color(QPalette::Text), 2.0));
      const double y = in.top() + pk / 24.0 * in.height();
      p.drawLine(QPointF(in.left(), y), QPointF(in.right(), y));
    }
  }
}

// ------------------------------------------------------------------------------ CurveWidget
namespace {
constexpr double kRangeDb = 15.0;
constexpr double kLeftMargin = 36.0, kRightMargin = 10.0, kTopMargin = 8.0, kBottomMargin = 20.0;
}  // namespace

CurveWidget::CurveWidget(QWidget* parent) : QWidget(parent)
{
  setMouseTracking(false);
  setMinimumHeight(90);
}

void CurveWidget::setCurve(const std::vector<double>& responseDb)
{
  curve_ = responseDb;
  update();
}

void CurveWidget::setSpectrum(const std::vector<float>& inDb, const std::vector<float>& outDb)
{
  specIn_ = inDb;
  specOut_ = outDb;
  if (specVisible_)
    update();
}

void CurveWidget::setSpectrumVisible(bool on)
{
  if (on == specVisible_)
    return;
  specVisible_ = on;
  update();
}

void CurveWidget::setBands(const std::vector<CurveBand>& bands, int selected)
{
  bands_ = bands;
  selected_ = selected;
  update();
}

QRectF CurveWidget::plotRect() const
{
  return QRectF(kLeftMargin, kTopMargin, width() - kLeftMargin - kRightMargin, height() - kTopMargin - kBottomMargin);
}

double CurveWidget::xForFreq(double hz) const
{
  const QRectF r = plotRect();
  return r.left() + std::log10(hz / 20.0) / 3.0 * r.width();
}

double CurveWidget::freqForX(double x) const
{
  const QRectF r = plotRect();
  const double t = std::min(1.0, std::max(0.0, (x - r.left()) / r.width()));
  return 20.0 * std::pow(1000.0, t);
}

double CurveWidget::yForDb(double db) const
{
  const QRectF r = plotRect();
  return r.center().y() - std::min(kRangeDb, std::max(-kRangeDb, db)) / kRangeDb * (r.height() / 2.0);
}

float CurveWidget::dbForY(double y) const
{
  const QRectF r = plotRect();
  const double db = (r.center().y() - y) / (r.height() / 2.0) * kRangeDb;
  return static_cast<float>(std::min(vac3dsp::kEqBandMaxDb * 1.0, std::max(-vac3dsp::kEqBandMaxDb * 1.0, db)));
}

int CurveWidget::nearestBand(double x) const
{
  int best = 0;
  double bestDist = 1e18;
  for (int b = 0; b < static_cast<int>(bands_.size()); ++b)
  {
    const double d = std::fabs(x - xForFreq(static_cast<double>(bands_[static_cast<size_t>(b)].freq)));
    if (d < bestDist)
    {
      bestDist = d;
      best = b;
    }
  }
  return best;
}

void CurveWidget::paintEvent(QPaintEvent*)
{
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing, true);
  const QPalette& pal = palette();
  const QRectF r = plotRect();
  const QColor text = pal.color(QPalette::Text);
  QColor grid = text;
  grid.setAlpha(40);
  QColor gridStrong = text;
  gridStrong.setAlpha(110);

  p.fillRect(r, pal.color(QPalette::Base));
  QFont smallFont = font();
  smallFont.setPointSizeF(7.0);
  p.setFont(smallFont);

  // spectrum (behind the grid and the curve): -90 .. -10 dBFS over the full height of the plot
  if (specVisible_ && static_cast<int>(specIn_.size()) == vac3dsp::kSpecBands &&
      static_cast<int>(specOut_.size()) == vac3dsp::kSpecBands)
  {
    auto drawSpectrum = [&](const std::vector<float>& v, const QColor& fill, const QColor& line) {
      const int count = vac3dsp::kSpecBands;
      QPolygonF outline;
      outline.reserve(count);
      for (int i = 0; i < count; ++i)
      {
        const float a = v[static_cast<size_t>(std::max(i - 1, 0))];
        const float b = v[static_cast<size_t>(i)];
        const float c = v[static_cast<size_t>(std::min(i + 1, count - 1))];
        const double db = std::min(-10.0, std::max(-90.0, static_cast<double>((a + 2.0f * b + c) / 4.0f)));
        outline << QPointF(xForFreq(vac3dsp::SpectrumBandHz(i)), r.bottom() - (db + 90.0) / 80.0 * r.height());
      }
      QPolygonF area = outline;
      area << QPointF(outline.last().x(), r.bottom()) << QPointF(outline.first().x(), r.bottom());
      p.setPen(Qt::NoPen);
      p.setBrush(fill);
      p.drawPolygon(area);
      p.setBrush(Qt::NoBrush);
      p.setPen(QPen(line, 1.2));
      p.drawPolyline(outline);
    };
    QColor greyFill = pal.color(QPalette::Text);
    greyFill.setAlpha(38);
    QColor greyLine = pal.color(QPalette::Text);
    greyLine.setAlpha(110);
    drawSpectrum(specIn_, greyFill, greyLine);
    drawSpectrum(specOut_, QColor(255, 150, 40, 70), QColor(255, 150, 40, 210));
    QColor legend = pal.color(QPalette::Text);
    legend.setAlpha(150);
    p.setPen(legend);
    p.drawText(QRectF(r.left() + 6, r.top() + 2, 420, 14), Qt::AlignLeft | Qt::AlignTop,
               QStringLiteral("Спектр: серый — вход, оранжевый — выход (−90…−10 дБFS)"));
  }

  // horizontal grid: dB
  for (int db = -12; db <= 12; db += 6)
  {
    const double y = yForDb(db);
    p.setPen(QPen(db == 0 ? gridStrong : grid, 1.0));
    p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
    p.setPen(text);
    p.drawText(QRectF(0, y - 7, kLeftMargin - 4, 14), Qt::AlignRight | Qt::AlignVCenter,
               db > 0 ? QStringLiteral("+%1").arg(db) : QString::number(db));
  }
  // vertical grid: frequency
  const double freqs[] = {20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000};
  for (double f : freqs)
  {
    const double x = xForFreq(f);
    p.setPen(QPen(grid, 1.0));
    p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
    p.setPen(text);
    const QString label = f >= 1000 ? QString::number(f / 1000.0) + QStringLiteral("k") : QString::number(f);
    p.drawText(QRectF(x - 20, r.bottom() + 2, 40, 14), Qt::AlignHCenter | Qt::AlignTop, label);
  }

  // marker line of the selected band
  if (selected_ >= 0 && selected_ < static_cast<int>(bands_.size()))
  {
    const double x = xForFreq(static_cast<double>(bands_[static_cast<size_t>(selected_)].freq));
    QColor sel = pal.color(QPalette::Highlight);
    sel.setAlpha(120);
    p.setPen(QPen(sel, 1.0, Qt::DashLine));
    p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
  }

  // response curve
  const int n = static_cast<int>(curve_.size());
  if (n > 1)
  {
    QPolygonF line;
    line.reserve(n);
    for (int i = 0; i < n; ++i)
      line << QPointF(xForFreq(vac3dsp::CurveFrequency(i)), yForDb(curve_[static_cast<size_t>(i)]));
    QPolygonF area = line;
    area << QPointF(line.last().x(), yForDb(0.0)) << QPointF(line.first().x(), yForDb(0.0));
    QColor fill = pal.color(QPalette::Highlight);
    fill.setAlpha(55);
    p.setPen(Qt::NoPen);
    p.setBrush(fill);
    p.drawPolygon(area);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(pal.color(QPalette::Highlight), 2.0));
    p.drawPolyline(line);
  }

  // band points: circle = gain band (bell, shelves), diamond = notch / cut filter (no gain, sits on 0 dB)
  for (int b = 0; b < static_cast<int>(bands_.size()); ++b)
  {
    const CurveBand& band = bands_[static_cast<size_t>(b)];
    const bool gain = vac3dsp::BandTypeUsesGain(band.type);
    const QPointF c(xForFreq(static_cast<double>(band.freq)), yForDb(gain ? static_cast<double>(band.db) : 0.0));
    const bool hot = (b == selected_) || (b == dragBand_);
    const double rad = hot ? 5.0 : 3.5;
    p.setPen(QPen(pal.color(QPalette::Highlight), 1.5));
    p.setBrush(hot ? pal.color(QPalette::Highlight) : pal.color(QPalette::Base));
    if (gain)
      p.drawEllipse(c, rad, rad);
    else
    {
      QPolygonF d;
      d << QPointF(c.x(), c.y() - rad - 1) << QPointF(c.x() + rad + 1, c.y()) << QPointF(c.x(), c.y() + rad + 1)
        << QPointF(c.x() - rad - 1, c.y());
      p.drawPolygon(d);
    }
  }
  p.setPen(QPen(pal.color(QPalette::Mid), 1.0));
  p.setBrush(Qt::NoBrush);
  p.drawRect(r);
}

void CurveWidget::mousePressEvent(QMouseEvent* event)
{
  if (!editable_ || event->button() != Qt::LeftButton || bands_.empty())
    return;
  dragBand_ = nearestBand(event->position().x());
  emit bandSelected(dragBand_);
  const bool gainBand = vac3dsp::BandTypeUsesGain(bands_[static_cast<size_t>(dragBand_)].type);
  dragFreq_ = (event->modifiers() & Qt::ShiftModifier) || !gainBand;
  if (!dragFreq_)
    emit bandEdited(dragBand_, dbForY(event->position().y()));
  update();
}

void CurveWidget::mouseMoveEvent(QMouseEvent* event)
{
  if (!editable_ || dragBand_ < 0)
    return;
  if (dragFreq_)
    emit bandFreqEdited(dragBand_, static_cast<float>(freqForX(event->position().x())));
  else
    emit bandEdited(dragBand_, dbForY(event->position().y()));
}

void CurveWidget::mouseReleaseEvent(QMouseEvent*)
{
  dragBand_ = -1;
  dragFreq_ = false;
  update();
}

void CurveWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
  if (!editable_ || bands_.empty())
    return;
  const int b = nearestBand(event->position().x());
  emit bandSelected(b);
  emit bandReset(b);
  event->accept();
}

void CurveWidget::wheelEvent(QWheelEvent* event)
{
  if (!editable_ || bands_.empty())
    return;
  const int dy = event->angleDelta().y();
  if (dy == 0)
    return;
  const int b = nearestBand(event->position().x());
  emit bandSelected(b);
  emit bandWheel(b, dy > 0 ? 1 : -1);
  event->accept();
}

// ------------------------------------------------------------------------------ SpectrumGrid
SpectrumGrid::SpectrumGrid(QWidget* parent)
    : QWidget(parent), in_(static_cast<size_t>(vac3dsp::kMaxChannels * vac3dsp::kSpecBands), vac3dsp::kSpecFloorDb),
      out_(static_cast<size_t>(vac3dsp::kMaxChannels * vac3dsp::kSpecBands), vac3dsp::kSpecFloorDb)
{
}

void SpectrumGrid::setSpectra(const float (*in)[vac3dsp::kSpecBands], const float (*out)[vac3dsp::kSpecBands])
{
  for (int c = 0; c < vac3dsp::kMaxChannels; ++c)
    for (int b = 0; b < vac3dsp::kSpecBands; ++b)
    {
      in_[static_cast<size_t>(c * vac3dsp::kSpecBands + b)] = in[c][b];
      out_[static_cast<size_t>(c * vac3dsp::kSpecBands + b)] = out[c][b];
    }
  update();
}

void SpectrumGrid::paintEvent(QPaintEvent*)
{
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing, true);
  const QPalette& pal = palette();
  static const char* const names[vac3dsp::kMaxChannels] = {"FL", "FR", "FC", "LFE", "SL", "SR"};
  const int cols = 2, rows = 3;
  const double gap = 6.0;
  const double cw = (width() - gap * (cols - 1)) / cols;
  const double ch = (height() - gap * (rows - 1)) / rows;
  QFont f = font();
  f.setPointSizeF(8.0);
  f.setBold(true);
  p.setFont(f);

  for (int c = 0; c < vac3dsp::kMaxChannels; ++c)
  {
    const QRectF cell(static_cast<double>(c % cols) * (cw + gap), static_cast<double>(c / cols) * (ch + gap), cw, ch);
    p.setPen(QPen(pal.color(QPalette::Mid), 1.0));
    p.setBrush(pal.color(QPalette::Base));
    p.drawRoundedRect(cell, 4.0, 4.0);
    const QRectF plot = cell.adjusted(3, 3, -3, -3);
    auto draw = [&](const std::vector<float>& v, const QColor& fill, const QColor& line) {
      QPolygonF outline;
      for (int i = 0; i < vac3dsp::kSpecBands; ++i)
      {
        const size_t base = static_cast<size_t>(c * vac3dsp::kSpecBands);
        const float a = v[base + static_cast<size_t>(std::max(i - 1, 0))];
        const float b = v[base + static_cast<size_t>(i)];
        const float d = v[base + static_cast<size_t>(std::min(i + 1, vac3dsp::kSpecBands - 1))];
        const double db = std::min(-10.0, std::max(-90.0, static_cast<double>((a + 2.0f * b + d) / 4.0f)));
        outline << QPointF(plot.left() + plot.width() * static_cast<double>(i) / (vac3dsp::kSpecBands - 1),
                           plot.bottom() - (db + 90.0) / 80.0 * plot.height());
      }
      QPolygonF area = outline;
      area << QPointF(outline.last().x(), plot.bottom()) << QPointF(outline.first().x(), plot.bottom());
      p.setPen(Qt::NoPen);
      p.setBrush(fill);
      p.drawPolygon(area);
      p.setBrush(Qt::NoBrush);
      p.setPen(QPen(line, 1.1));
      p.drawPolyline(outline);
    };
    QColor greyFill = pal.color(QPalette::Text);
    greyFill.setAlpha(36);
    QColor greyLine = pal.color(QPalette::Text);
    greyLine.setAlpha(110);
    draw(in_, greyFill, greyLine);
    draw(out_, QColor(255, 150, 40, 70), QColor(255, 150, 40, 210));
    p.setPen(pal.color(QPalette::Text));
    p.drawText(QRectF(cell.left() + 6, cell.top() + 3, 60, 14), Qt::AlignLeft | Qt::AlignTop, QString::fromLatin1(names[c]));
  }
}

// ------------------------------------------------------------------------------ BandBadge
BandBadge::BandBadge(QWidget* parent) : QWidget(parent)
{
  setCursor(Qt::PointingHandCursor);
}

void BandBadge::setShape(const QString& tag, float widthFraction, bool selected)
{
  if (tag == tag_ && selected == selected_ && std::fabs(widthFraction - width_) < 0.004f)
    return;
  tag_ = tag;
  width_ = widthFraction;
  selected_ = selected;
  update();
}

void BandBadge::mousePressEvent(QMouseEvent* event)
{
  if (event->button() == Qt::LeftButton)
    emit clicked();
}

void BandBadge::paintEvent(QPaintEvent*)
{
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing, true);
  const QPalette& pal = palette();
  const QRectF r = QRectF(rect()).adjusted(1.5, 1.5, -1.5, -1.5);
  QColor fill = pal.color(QPalette::Highlight);
  fill.setAlpha(selected_ ? 60 : 0);
  p.setPen(QPen(selected_ ? pal.color(QPalette::Highlight) : pal.color(QPalette::Mid), selected_ ? 1.6 : 1.0));
  p.setBrush(fill);
  p.drawRoundedRect(r, 4.0, 4.0);

  QFont f = font();
  f.setPointSizeF(7.0);
  f.setBold(true);
  p.setFont(f);
  p.setPen(pal.color(QPalette::Text));
  p.drawText(QRectF(r.left(), r.top() + 1.0, r.width(), r.height() - 8.0), Qt::AlignHCenter | Qt::AlignVCenter, tag_);

  const double barLeft = r.left() + 4.0, barRight = r.right() - 4.0, barY = r.bottom() - 4.0;
  QColor track = pal.color(QPalette::Text);
  track.setAlpha(55);
  p.setPen(QPen(track, 2.0, Qt::SolidLine, Qt::RoundCap));
  p.drawLine(QPointF(barLeft, barY), QPointF(barRight, barY));
  p.setPen(QPen(pal.color(QPalette::Highlight), 2.0, Qt::SolidLine, Qt::RoundCap));
  p.drawLine(QPointF(barLeft, barY), QPointF(barLeft + (barRight - barLeft) * static_cast<double>(std::min(1.0f, std::max(0.04f, width_))), barY));
}

// ------------------------------------------------------------------------------ ParamSlider
ParamSlider::ParamSlider(const QString& name, double minValue, double maxValue, const QString& unit, int decimals,
                         bool logScale, QWidget* parent)
    : QWidget(parent), name_(new QLabel(name, this)), readout_(new QLabel(this)),
      slider_(new QSlider(Qt::Horizontal, this)), min_(minValue), max_(maxValue), unit_(unit), decimals_(decimals),
      log_(logScale)
{
  slider_->setRange(0, 1000);
  name_->setMinimumWidth(78);
  readout_->setMinimumWidth(70);
  readout_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  readout_->setProperty("role", "value");
  auto* row = new QHBoxLayout(this);
  row->setContentsMargins(0, 0, 0, 0);
  row->setSpacing(8);
  row->addWidget(name_);
  row->addWidget(slider_, 1);
  row->addWidget(readout_);
  connect(slider_, &QSlider::valueChanged, this, [this](int) {
    updateReadout();
    emit valueChanged(value());
  });
  updateReadout();
}

double ParamSlider::fromPos(int pos) const
{
  const double t = static_cast<double>(pos) / 1000.0;
  if (log_)
    return min_ * std::pow(max_ / min_, t);
  return min_ + t * (max_ - min_);
}

int ParamSlider::toPos(double value) const
{
  value = std::min(max_, std::max(min_, value));
  const double t = log_ ? std::log(value / min_) / std::log(max_ / min_) : (value - min_) / (max_ - min_);
  return static_cast<int>(std::lround(t * 1000.0));
}

double ParamSlider::value() const { return fromPos(slider_->value()); }

void ParamSlider::setValue(double v)
{
  const bool was = slider_->blockSignals(true);
  slider_->setValue(toPos(v));
  slider_->blockSignals(was);
  updateReadout();
}

void ParamSlider::updateReadout()
{
  readout_->setText(QString::number(value(), 'f', decimals_) + QStringLiteral(" ") + unit_);
}
