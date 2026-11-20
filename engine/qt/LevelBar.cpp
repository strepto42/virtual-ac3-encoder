#include "LevelBar.h"

#include <QPainter>
#include <QPalette>

#include <algorithm>
#include <cmath>

LevelBar::LevelBar(QWidget* parent) : QWidget(parent)
{
  setToolTip(QStringLiteral("Уровень канала на выходе (после регулятора, запаса и лимитера)"));
}

void LevelBar::setValues(float levelDb, float peakDb)
{
  if (std::fabs(levelDb - level_) < 0.05f && std::fabs(peakDb - peak_) < 0.05f)
    return;
  level_ = levelDb;
  peak_ = peakDb;
  update();
}

void LevelBar::paintEvent(QPaintEvent*)
{
  QPainter p(this);
  const QPalette& pal = palette();
  const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
  p.setPen(QPen(pal.color(QPalette::Mid), 1.0));
  p.setBrush(pal.color(QPalette::Base));
  p.drawRoundedRect(r, 2.0, 2.0);
  const QRectF in = r.adjusted(1.5, 1.5, -1.5, -1.5);

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
  QColor tick = pal.color(QPalette::Text);
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
