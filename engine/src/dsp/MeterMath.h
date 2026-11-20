// MeterMath.h — dB conversion and meter ballistics for DSP Studio (no Qt, unit-tested).
#pragma once

#include <algorithm>
#include <cmath>

namespace vac3dsp {

constexpr float kMeterFloorDb = -90.0f;

inline float PeakToDb(float linear)
{
  return linear <= 1e-5f ? kMeterFloorDb : std::max(kMeterFloorDb, 20.0f * std::log10(linear));
}

// Level bar with instant attack and a steady fall, plus a peak marker that is held for a moment.
class MeterBallistics
{
public:
  static constexpr float kFallDbPerSec = 24.0f;
  static constexpr float kPeakHoldSec = 1.5f;
  static constexpr float kPeakFallDbPerSec = 18.0f;

  void Reset()
  {
    level_ = peak_ = kMeterFloorDb;
    hold_ = 0.0f;
  }

  // Call once per UI tick with the newest measured level (use kMeterFloorDb when there is no new data).
  void Update(float measuredDb, float dtSec)
  {
    level_ = std::max(measuredDb, level_ - kFallDbPerSec * dtSec);
    if (measuredDb >= peak_)
    {
      peak_ = measuredDb;
      hold_ = kPeakHoldSec;
    }
    else if (hold_ > 0.0f)
      hold_ -= dtSec;
    else
      peak_ = std::max(level_, peak_ - kPeakFallDbPerSec * dtSec);
    level_ = std::max(level_, kMeterFloorDb);
    peak_ = std::max(peak_, kMeterFloorDb);
  }

  float level() const { return level_; }
  float peak() const { return peak_; }

private:
  float level_ = kMeterFloorDb;
  float peak_ = kMeterFloorDb;
  float hold_ = 0.0f;
};

}  // namespace vac3dsp
