// Limiter.h — linked look-ahead peak limiter for planar float audio.
//
// Purpose: guarantee that the 5.1 signal handed to the AC3 / DTS encoder never exceeds
// the configured ceiling, so the encoder (and the float -> S32 conversion used for DTS)
// never clips. All channels are measured together ("linked"), so the stereo/surround image
// does not shift when the limiter engages.
//
// Algorithm (classic look-ahead limiter):
//   r[n]  = required gain for sample n         = min(1, ceiling / max_c |x_c[n]|)
//   m[n]  = min(r[n-L .. n])                   (sliding minimum over L+1 samples)
//   t[n]  = mean(m[n-L .. n])                  (box smoothing -> gain ramps down BEFORE the peak)
//   g[n]  = t[n] when falling, slow release when rising (so g[n] <= t[n] always)
//   y[n]  = x[n-L] * g[n]                      (audio is delayed by L samples)
// Because every m[k] in the averaging window is <= r[n-L], we get g[n] <= r[n-L], i.e.
// |y[n]| <= ceiling for any input. Latency is L samples (default 2 ms).
//
// Pure C++17, no dependencies; the audio thread never allocates (Configure() allocates).
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <vector>

class LookaheadLimiter
{
public:
  // channels: 1..N, sampleRate: Hz, ceilingDb: <= 0 dBFS, lookaheadMs / releaseMs: timing.
  void Configure(int channels, int sampleRate, float ceilingDb = -1.0f,
                 float lookaheadMs = 2.0f, float releaseMs = 150.0f)
  {
    channels_ = std::max(1, channels);
    look_ = std::max(1, static_cast<int>(std::lround(sampleRate * lookaheadMs / 1000.0f)));
    ceiling_ = std::pow(10.0f, std::min(0.0f, ceilingDb) / 20.0f);
    const float relSamples = std::max(1.0f, sampleRate * releaseMs / 1000.0f);
    releaseCoef_ = 1.0f - std::exp(-1.0f / relSamples);
    delay_.assign(static_cast<size_t>(channels_) * static_cast<size_t>(look_), 0.0f);
    req_.assign(static_cast<size_t>(look_) + 1, 1.0f);
    min_.assign(static_cast<size_t>(look_) + 1, 1.0f);
    Reset();
  }

  void Reset()
  {
    std::fill(delay_.begin(), delay_.end(), 0.0f);
    std::fill(req_.begin(), req_.end(), 1.0f);
    std::fill(min_.begin(), min_.end(), 1.0f);
    delayPos_ = 0;
    histPos_ = 0;
    minSum_ = static_cast<double>(min_.size());
    gain_ = 1.0f;
    deepestGain_.store(1.0f, std::memory_order_relaxed);
  }

  int LatencySamples() const { return look_; }

  // Process `frames` samples in place. planes[c] points to channel c (planar float).
  void Process(float* const* planes, int frames)
  {
    if (delay_.empty() || frames <= 0)
      return;
    const size_t histLen = req_.size();
    float blockDeepest = 1.0f;

    for (int n = 0; n < frames; ++n)
    {
      float peak = 0.0f;
      for (int c = 0; c < channels_; ++c)
        peak = std::max(peak, std::fabs(planes[c][n]));
      const float required = (peak > ceiling_) ? (ceiling_ / peak) : 1.0f;

      req_[histPos_] = required;
      float windowMin = 1.0f;
      for (size_t i = 0; i < histLen; ++i)
        windowMin = std::min(windowMin, req_[i]);

      minSum_ += static_cast<double>(windowMin) - static_cast<double>(min_[histPos_]);
      min_[histPos_] = windowMin;
      histPos_ = (histPos_ + 1) % histLen;

      const float target = static_cast<float>(minSum_ / static_cast<double>(histLen));
      gain_ = (target < gain_) ? target : gain_ + (target - gain_) * releaseCoef_;
      gain_ = std::min(gain_, 1.0f);
      blockDeepest = std::min(blockDeepest, gain_);

      for (int c = 0; c < channels_; ++c)
      {
        float& slot = delay_[static_cast<size_t>(c) * static_cast<size_t>(look_) + delayPos_];
        const float delayed = slot;
        slot = planes[c][n];
        planes[c][n] = delayed * gain_;
      }
      delayPos_ = (delayPos_ + 1) % static_cast<size_t>(look_);
    }

    if (blockDeepest < deepestGain_.load(std::memory_order_relaxed))
      deepestGain_.store(blockDeepest, std::memory_order_relaxed);
  }

  // Deepest gain reduction (in dB, >= 0) since the previous call; resets the tracker.
  // Safe to call from a different thread than Process().
  float TakeMaxReductionDb()
  {
    const float g = deepestGain_.exchange(1.0f, std::memory_order_relaxed);
    return g >= 1.0f ? 0.0f : -20.0f * std::log10(std::max(g, 1e-6f));
  }

private:
  int channels_ = 6;
  int look_ = 96;
  float ceiling_ = 0.891251f;
  float releaseCoef_ = 0.0001f;
  std::vector<float> delay_;  // channels_ * look_ samples
  std::vector<float> req_;    // required-gain history (look_+1)
  std::vector<float> min_;    // windowed-min history (look_+1)
  size_t delayPos_ = 0;
  size_t histPos_ = 0;
  double minSum_ = 0.0;
  float gain_ = 1.0f;
  std::atomic<float> deepestGain_{1.0f};
};
