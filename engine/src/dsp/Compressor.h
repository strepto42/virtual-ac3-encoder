// Compressor.h — linked feed-forward compressor with soft knee for planar float audio.
//
//  * One detector for all channels ("linked"): the peak of the loudest channel drives one gain that
//    is applied to every channel, so the stereo/surround image does not move when it engages.
//    The LFE channel is left out of the detector by default (bass would pump the dialogue) but it
//    still receives the gain.
//  * Gain computer: classic soft-knee curve (Giannoulis/Massberg/Reiss).
//  * The gain reduction is smoothed in the dB domain: attack when reduction increases, release when
//    it decreases. Makeup gain is smoothed too, so moving the knob never clicks.
//  * No look-ahead (zero latency); the look-ahead limiter at the end of the chain catches what is left.
//
// Not thread-safe by itself; use from the audio thread only (DspChain does that).
#pragma once

#include "DspParams.h"

#include <algorithm>
#include <atomic>
#include <cmath>

namespace vac3dsp {

class LinkedCompressor
{
public:
  void Configure(int channels, int sampleRate)
  {
    channels_ = std::min(std::max(channels, 1), kMaxChannels);
    rate_ = std::max(8000, sampleRate);
    detDecay_ = std::exp(-1.0f / (0.020f * static_cast<float>(rate_)));  // 20 ms peak-hold decay
    makeupCoef_ = std::exp(-1.0f / (0.020f * static_cast<float>(rate_)));
    SetParams(CompParams{});
    Reset();
  }

  void SetParams(const CompParams& p)
  {
    params_ = p;
    attackCoef_ = TimeCoef(p.attackMs);
    releaseCoef_ = TimeCoef(p.releaseMs);
    makeupTargetDb_ = p.makeupDb;
    lfeDetected_ = p.lfeInDetector != 0;
  }

  void Reset()
  {
    env_ = 0.0f;
    grDb_ = 0.0f;
    makeupDb_ = makeupTargetDb_;
    deepest_.store(0.0f, std::memory_order_relaxed);
  }

  // Output level minus input level (dB, <= 0) of the static curve — also used by the UI to draw it.
  static float StaticGainDb(float levelDb, const CompParams& p)
  {
    const float over = levelDb - p.thresholdDb;
    const float slope = 1.0f / p.ratio - 1.0f;  // <= 0
    const float w = p.kneeDb;
    if (w <= 0.0f)
      return over > 0.0f ? slope * over : 0.0f;
    if (2.0f * over < -w)
      return 0.0f;
    if (2.0f * std::fabs(over) <= w)
    {
      const float t = over + 0.5f * w;
      return slope * t * t / (2.0f * w);
    }
    return slope * over;
  }

  void Process(float* const* planes, int frames)
  {
    if (channels_ <= 0 || frames <= 0)
      return;
    const int detFrom = 0;
    const int detCh = channels_;
    float blockDeepest = 0.0f;

    for (int n = 0; n < frames; ++n)
    {
      float peak = 0.0f;
      for (int c = detFrom; c < detCh; ++c)
      {
        if (c == kLfeChannel && channels_ > kLfeChannel && !lfeDetected_)
          continue;
        peak = std::max(peak, std::fabs(planes[c][n]));
      }
      env_ = std::max(peak, env_ * detDecay_);
      const float levelDb = 20.0f * std::log10(std::max(env_, 1e-6f));
      const float target = StaticGainDb(levelDb, params_);
      const float coef = (target < grDb_) ? attackCoef_ : releaseCoef_;
      grDb_ = target + coef * (grDb_ - target);
      makeupDb_ = makeupTargetDb_ + makeupCoef_ * (makeupDb_ - makeupTargetDb_);
      blockDeepest = std::min(blockDeepest, grDb_);

      const float totalDb = grDb_ + makeupDb_;
      if (totalDb != 0.0f)
      {
        const float gain = std::exp(totalDb * 0.11512925465f);  // dB -> linear (ln10 / 20)
        for (int c = 0; c < channels_; ++c)
          planes[c][n] *= gain;
      }
    }

    if (blockDeepest < deepest_.load(std::memory_order_relaxed))
      deepest_.store(blockDeepest, std::memory_order_relaxed);
  }

  // Deepest gain reduction (dB, >= 0) since the previous call. Safe from another thread.
  float TakeMaxReductionDb() { return -deepest_.exchange(0.0f, std::memory_order_relaxed); }

private:
  float TimeCoef(float ms) const
  {
    return std::exp(-1.0f / (std::max(ms, 0.01f) * 0.001f * static_cast<float>(rate_)));
  }

  int channels_ = 0;
  int rate_ = 48000;
  CompParams params_;
  bool lfeDetected_ = false;
  float detDecay_ = 0.999f, attackCoef_ = 0.99f, releaseCoef_ = 0.999f, makeupCoef_ = 0.999f;
  float env_ = 0.0f, grDb_ = 0.0f, makeupDb_ = 0.0f, makeupTargetDb_ = 0.0f;
  std::atomic<float> deepest_{0.0f};
};

}  // namespace vac3dsp
