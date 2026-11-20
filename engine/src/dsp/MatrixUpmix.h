// MatrixUpmix.h — stereo -> 5.1 adaptive matrix upmix in the time domain (engine option upmix_algo=matrix).
//
// An alternative to the FFT `surround` filter: no look-ahead, so it adds no delay (the FFT filter adds ~87
// ms), and it costs next to nothing. The principle is the one of matrix decoders (Dolby Pro Logic and
// relatives): what left and right have IN COMMON is the centre, what DIFFERS is the surround.
//
//  Centre   Left and right are split at centerLowHz and centerHighHz (Linkwitz-Riley 4th order); the middle
//           band is the "centre band". The front channels are the SUM OF ALL BANDS (flat in level, only
//           phase-shifted), so taking the centre band out of them cancels it completely.
//           In that band the decoder measures, with ~12 ms smoothing, the level balance and the correlation
//           between the channels. A source that sits near the middle (centerWidth) and whose left and right
//           agree (centerFocus) steers a gain g in 0..1. The centre output is g * (L + R) / sqrt(2) of the
//           band, and the same common part is taken out of L and R (g * (L + R) / 2). Bass and treble are
//           never in the analysis, so a kick drum cannot pump the dialogue, and they stay in L and R.
//           g rises in ~6 ms and falls in ~120 ms, which keeps the steering from chattering.
//  Surround (L - R) / sqrt(2), band-limited (100 Hz .. 9 kHz), delayed, with opposite polarity in the two
//           surround channels (a wide, decorrelated image). Level and delay are settings.
//  LFE      optional (lfeEnabled): the low-passed (4th order, lfeCutHz) mono sum (L + R) / 2, with its own
//           level. Off by default: the receiver's bass management and the channel trims already cover bass.
//
// Planes: FL, FR, FC, LFE, SL, SR (as in the encoder frame). Process() works in place. When FC/SL/SR
// carry signal the input is already multichannel and passes untouched (unless Force: "always");
// the switch is a 20 ms crossfade, and the upmix engages after 200 ms of silence in those planes (stereo
// material), exactly like the engine's auto mode for the FFT upmix.
//
// Every moving value is slewed (cut-offs, level, delay), the update phase is kept between calls, so the
// result does not depend on how the audio is split into blocks. No allocation after Configure().
#pragma once

#include "DspParams.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace vac3dsp {

class MatrixUpmix
{
public:
  static constexpr int kSub = 32;                 // slow-update interval, samples
  static constexpr size_t kRing = 8192;           // delay line (power of two): 40 ms at up to 192 kHz
  static constexpr float kLevelSlewDb = 0.5f;     // per update
  static constexpr float kCutoffSlewOct = 0.02f;  // per update
  static constexpr float kDelaySlewSamples = 0.5f;  // per update (a 1.6 % pitch shift while the delay moves)
  static constexpr float kLfeOffDb = -96.0f;        // "off" as a level, so that on / off is a fade

  void Configure(int sampleRate)
  {
    rate_ = std::max(8000, sampleRate);
    ring_.assign(kRing, 0.0f);
    UpmixParams def;
    SetParams(def);
    Reset();
  }

  void SetForce(bool force) { force_ = force; }

  // Targets (audio thread). Out-of-range values are clamped.
  void SetParams(const UpmixParams& in)
  {
    DspParams wrap;
    wrap.upmix = in;
    Sanitize(wrap);
    const UpmixParams& p = wrap.upmix;
    enabled_ = p.enabled != 0;
    width_ = static_cast<double>(p.centerWidth);
    focus_ = static_cast<double>(p.centerFocus);
    lowT_ = p.centerLowHz;
    highT_ = p.centerHighHz;
    levelT_ = p.surroundLevelDb;
    delayT_ = p.surroundDelayMs * 0.001f * static_cast<float>(rate_);
    lfeLevelT_ = p.lfeEnabled != 0 ? p.lfeLevelDb : kLfeOffDb;
    lfeCutT_ = p.lfeCutHz;
  }

  // Jump to the targets, clear all memory.
  void Reset()
  {
    lowC_ = lowT_;
    highC_ = highT_;
    levelC_ = levelT_;
    delayC_ = delayT_;
    lvl_ = std::pow(10.0f, levelC_ / 20.0f);
    lfeLevelC_ = lfeLevelT_;
    lfeCutC_ = lfeCutT_;
    lfeLin_ = lfeLevelC_ <= kLfeOffDb + 0.5f ? 0.0f : std::pow(10.0f, lfeLevelC_ / 20.0f);
    SetFilters();
    for (Bank& b : bank_)
      b.Clear();
    for (Biquad& f : lfeLp_)
      f.z1 = f.z2 = 0.0;
    eL_ = eR_ = eX_ = g_ = 0.0;
    dLow_ = dHigh_ = 0.0;
    std::fill(ring_.begin(), ring_.end(), 0.0f);
    wr_ = 0;
    const double fs = static_cast<double>(rate_);
    aE_ = 1.0 - std::exp(-1.0 / (0.012 * fs));
    aAtt_ = 1.0 - std::exp(-1.0 / (0.006 * fs));
    aRel_ = 1.0 - std::exp(-1.0 / (0.120 * fs));
    aSurHp_ = 1.0 - std::exp(-2.0 * kPi * 100.0 / fs);
    aSurLp_ = 1.0 - std::exp(-2.0 * kPi * std::min(9000.0, 0.4 * fs) / fs);
    mixStep_ = static_cast<float>(1.0 / (0.020 * fs));
    mix_ = 0.0f;
    quiet_ = 0;
    phase_ = 0;
  }

  // 1 while the upmix is (being) applied, 0 while the input passes untouched. For diagnostics and tests.
  float Mix() const { return mix_; }

  void Process(float* const* planes, int frames)
  {
    if (frames <= 0 || ring_.empty())
      return;

    // Is the input already multichannel? (signal on FC / SL / SR)
    bool loud = false;
    for (int c : {2, 4, 5})
      for (int i = 0; i < frames && !loud; ++i)
        loud = std::fabs(planes[c][i]) > 1e-4f;
    if (loud)
      quiet_ = 0;
    else
      quiet_ = std::min<int64_t>(quiet_ + frames, int64_t{1} << 40);
    const bool want = enabled_ && (force_ || quiet_ >= rate_ / 5);
    const float mixTarget = want ? 1.0f : 0.0f;

    float* const fl = planes[0];
    float* const fr = planes[1];
    float* const fc = planes[2];
    float* const sl = planes[4];
    float* const sr = planes[5];
    float* const lfe = planes[3];

    for (int i = 0; i < frames; ++i)
    {
      if (phase_ == 0)
      {
        UpdateSlow();
        phase_ = kSub;
      }
      --phase_;

      const double l = static_cast<double>(fl[i]);
      const double r = static_cast<double>(fr[i]);

      // ---- centre: analyse the band
      double lb, rb, la, ra;   // centre band and the all-pass version of the channel
      bank_[0].Run(l, lb, la);
      bank_[1].Run(r, rb, ra);
      eL_ += aE_ * (lb * lb - eL_);
      eR_ += aE_ * (rb * rb - eR_);
      eX_ += aE_ * (lb * rb - eX_);
      double target = 0.0;
      if (eL_ + eR_ > 1e-9)
      {
        const double aL = std::sqrt(eL_), aR = std::sqrt(eR_);
        const double rho = std::min(1.0, std::max(-1.0, eX_ / (aL * aR + 1e-12)));
        const double pan = (aR - aL) / (aL + aR + 1e-12);           // -1 hard left .. +1 hard right
        const double wPan = 0.05 + 0.95 * width_;
        const double x = std::fabs(pan) / wPan;
        const double centred = x >= 1.0 ? 0.0 : 1.0 - x * x;         // 1 in the middle, 0 at the edge of the width ("near" is a Windows macro!)
        const double rho0 = 0.9 * focus_;
        const double agree = std::min(1.0, std::max(0.0, (rho - rho0) / (1.0 - rho0)));
        target = centred * agree;
      }
      g_ += (target > g_ ? aAtt_ : aRel_) * (target - g_);

      const double common = 0.5 * (lb + rb);                         // the band part that L and R share
      const double centreOut = kSqrt2 * g_ * common;
      const double outL = la - g_ * common;
      const double outR = ra - g_ * common;

      // ---- surround: the difference, band-limited, delayed
      const double d = (l - r) * kInvSqrt2;
      dLow_ += aSurHp_ * (d - dLow_);
      const double dh = d - dLow_;                                    // high-pass 100 Hz
      dHigh_ += aSurLp_ * (dh - dHigh_);                              // low-pass 9 kHz
      ring_[wr_ & (kRing - 1)] = static_cast<float>(dHigh_);
      const double rd = static_cast<double>(wr_) - static_cast<double>(delayC_);
      const double fl0 = std::floor(rd);
      const double frac = rd - fl0;
      const uint64_t i0 = static_cast<uint64_t>(fl0);
      const double delayed = static_cast<double>(ring_[i0 & (kRing - 1)]) * (1.0 - frac) +
                             static_cast<double>(ring_[(i0 + 1) & (kRing - 1)]) * frac;
      ++wr_;
      const float surr = static_cast<float>(delayed * static_cast<double>(lvl_));

      // ---- LFE: low-passed mono sum (the filter keeps running so that switching it on is instant and clean)
      const double bass = lfeLp_[1].Run(lfeLp_[0].Run(0.5 * (l + r)));
      const float lfeOut = static_cast<float>(bass * static_cast<double>(lfeLin_));

      // ---- crossfade between "input passes" and "upmixed"
      if (mix_ < mixTarget)
        mix_ = std::min(mixTarget, mix_ + mixStep_);
      else if (mix_ > mixTarget)
        mix_ = std::max(mixTarget, mix_ - mixStep_);
      if (mix_ <= 0.0f)
        continue;                                                     // the planes stay as they came
      const float m = mix_;
      fl[i] = fl[i] + m * (static_cast<float>(outL) - fl[i]);
      fr[i] = fr[i] + m * (static_cast<float>(outR) - fr[i]);
      fc[i] = fc[i] + m * (static_cast<float>(centreOut) - fc[i]);
      sl[i] = sl[i] + m * (surr - sl[i]);
      sr[i] = sr[i] + m * (-surr - sr[i]);
      if (lfeLin_ > 0.0f)                                             // LFE untouched while it is switched off
        lfe[i] = lfe[i] + m * (lfeOut - lfe[i]);
    }
  }

private:
  static constexpr double kPi = 3.14159265358979323846;
  static constexpr double kSqrt2 = 1.41421356237309504880;
  static constexpr double kInvSqrt2 = 0.70710678118654752440;

  struct Biquad
  {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0, z1 = 0.0, z2 = 0.0;
    double Run(double x)
    {
      const double y = b0 * x + z1;
      z1 = b1 * x - a1 * y + z2;
      z2 = b2 * x - a2 * y;
      return y;
    }
  };

  // One channel split into low / centre band / high with Linkwitz-Riley 4th order sections (two identical
  // Butterworth biquads, Q = 1/sqrt 2). `mid` is the centre band; `aligned` is low + mid + high with the
  // upper split applied to the low part as well, which makes it an all-pass version of the input: same
  // level at every frequency, phase-shifted exactly like `mid`. Subtracting `mid` from `aligned` therefore
  // cancels the centre band completely, which subtracting from the raw input would not.
  struct Bank
  {
    Biquad hp1[2], lp1[2];       // split at the low cut-off
    Biquad lp2a[2], hp2a[2];     // split of the upper part at the high cut-off
    Biquad lp2b[2], hp2b[2];     // the same split of the low part
    static double Cascade(Biquad (&f)[2], double x) { return f[1].Run(f[0].Run(x)); }
    void Run(double x, double& mid, double& aligned)
    {
      const double up = Cascade(hp1, x);
      const double low = Cascade(lp1, x);
      mid = Cascade(lp2a, up);
      aligned = mid + Cascade(hp2a, up) + Cascade(lp2b, low) + Cascade(hp2b, low);
    }
    void Clear()
    {
      for (Biquad* g : {hp1, lp1, lp2a, hp2a, lp2b, hp2b})
        g[0].z1 = g[0].z2 = g[1].z1 = g[1].z2 = 0.0;
    }
  };

  static void Rbj(Biquad& f, double fc, double fs, bool highPass)
  {
    const double w0 = 2.0 * kPi * std::min(fc, 0.45 * fs) / fs;
    const double c = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * kInvSqrt2);
    const double a0 = 1.0 + alpha;
    const double sign = highPass ? 1.0 : -1.0;
    f.b0 = (1.0 + sign * c) / 2.0 / a0;
    f.b1 = -sign * (1.0 + sign * c) / a0;
    f.b2 = f.b0;
    f.a1 = -2.0 * c / a0;
    f.a2 = (1.0 - alpha) / a0;
  }

  void SetLfeFilters()
  {
    for (Biquad& f : lfeLp_)
      Rbj(f, static_cast<double>(lfeCutC_), static_cast<double>(rate_), false);
  }

  void SetFilters()
  {
    SetLfeFilters();
    const double fs = static_cast<double>(rate_), lo = static_cast<double>(lowC_), hi = static_cast<double>(highC_);
    for (Bank& b : bank_)
      for (int s = 0; s < 2; ++s)
      {
        Rbj(b.hp1[s], lo, fs, true);
        Rbj(b.lp1[s], lo, fs, false);
        Rbj(b.lp2a[s], hi, fs, false);
        Rbj(b.hp2a[s], hi, fs, true);
        Rbj(b.lp2b[s], hi, fs, false);
        Rbj(b.hp2b[s], hi, fs, true);
      }
  }

  static float Approach(float cur, float target, float step)
  {
    const float d = target - cur;
    return std::fabs(d) <= step ? target : cur + (d > 0.0f ? step : -step);
  }

  static float ApproachLog(float cur, float target, float stepOct)
  {
    const double d = std::log2(static_cast<double>(target) / static_cast<double>(cur));
    if (std::fabs(d) <= static_cast<double>(stepOct))
      return target;
    return static_cast<float>(static_cast<double>(cur) * std::exp2(d > 0.0 ? static_cast<double>(stepOct) : -static_cast<double>(stepOct)));
  }

  void UpdateSlow()
  {
    if (lowC_ != lowT_ || highC_ != highT_)
    {
      lowC_ = ApproachLog(lowC_, lowT_, kCutoffSlewOct);
      highC_ = ApproachLog(highC_, highT_, kCutoffSlewOct);
      SetFilters();
    }
    if (levelC_ != levelT_)
    {
      levelC_ = Approach(levelC_, levelT_, kLevelSlewDb);
      lvl_ = std::pow(10.0f, levelC_ / 20.0f);
    }
    delayC_ = Approach(delayC_, delayT_, kDelaySlewSamples);
    if (lfeCutC_ != lfeCutT_)
    {
      lfeCutC_ = ApproachLog(lfeCutC_, lfeCutT_, kCutoffSlewOct);
      SetLfeFilters();
    }
    if (lfeLevelC_ != lfeLevelT_)
    {
      lfeLevelC_ = Approach(lfeLevelC_, lfeLevelT_, kLevelSlewDb);   // on / off is a 0.5 dB-per-step fade (~130 ms)
      lfeLin_ = lfeLevelC_ <= kLfeOffDb + 0.5f ? 0.0f : std::pow(10.0f, lfeLevelC_ / 20.0f);
    }
  }

  int rate_ = 48000;
  bool force_ = false;
  bool enabled_ = true;
  double width_ = 0.5, focus_ = 0.5;
  float lowT_ = 150.0f, highT_ = 5000.0f, levelT_ = -3.0f, delayT_ = 480.0f;
  float lowC_ = 150.0f, highC_ = 5000.0f, levelC_ = -3.0f, delayC_ = 480.0f, lvl_ = 0.7f;

  Bank bank_[2];
  Biquad lfeLp_[2];
  float lfeLevelT_ = -96.0f, lfeLevelC_ = -96.0f, lfeCutT_ = 100.0f, lfeCutC_ = 100.0f, lfeLin_ = 0.0f;
  double eL_ = 0, eR_ = 0, eX_ = 0, g_ = 0;
  double aE_ = 0, aAtt_ = 0, aRel_ = 0, aSurHp_ = 0, aSurLp_ = 0;
  double dLow_ = 0, dHigh_ = 0;
  std::vector<float> ring_;
  uint64_t wr_ = 0;
  float mix_ = 0.0f, mixStep_ = 0.001f;
  int64_t quiet_ = 0;
  int phase_ = 0;
};

}  // namespace vac3dsp
