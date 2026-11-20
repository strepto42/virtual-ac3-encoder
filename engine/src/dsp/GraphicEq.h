// GraphicEq.h — 30-band equalizer for planar float audio (up to 6 channels).
//
// Every band is one biquad (RBJ cookbook). Out of the box the 30 bands are bell filters, 0.34 octave
// wide, centred on the log-spaced frequencies (20 Hz .. 20 kHz) in DspParams.h, so the EQ behaves like a
// classic graphic equalizer; but each band can also be moved (frequency), made narrower or wider, and
// given another shape: bell, low shelf, high shelf, notch, high-pass, low-pass (see BandShape). The
// shape of a band is shared by all channels, the gains are per channel (the UI may link them).
//
//  * Coefficients and states are double precision (low bands at 25 Hz are numerically touchy).
//  * A gain band (bell/shelf) whose gain is exactly 0 dB and whose state has decayed is skipped, so a
//    flat EQ is a bit-exact pass-through.
//  * Nothing is ever switched abruptly. Every kSub samples each moving value steps towards its target:
//      gain      at most kSlewDb (12 dB takes ~32 ms at 48 kHz),
//      frequency at most kFreqSlewOct, width at most kWidthSlewOct,
//      shape     the band fades out (kFadeStep per update, as gentle as a gain slew), the shape is
//                swapped while the band is transparent, and it fades back in. Gain bands fade by scaling their dB gain, notch /
//                cut bands fade by mixing the filtered signal with the dry one.
//    The update phase is kept between calls, so the result does not depend on how the audio is split
//    into blocks.
//  * The audio thread never allocates (Configure() allocates).
//
// Not thread-safe by itself: SetParams()/Process() must be called from the audio thread (DspChain
// takes care of that).
#pragma once

#include "DspParams.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace vac3dsp {

class GraphicEq
{
public:
  static constexpr int kSub = 64;               // coefficient update interval, samples
  static constexpr float kSlewDb = 0.5f;        // max gain step per update
  static constexpr float kFreqSlewOct = 0.01f;  // max frequency step per update, octaves (1 octave in ~67 ms)
  static constexpr float kWidthSlewOct = 0.02f; // max width step per update, octaves
  // Shape change: the band fades out, swaps shape, fades in. A +-12 dB band changes by at most half of
  // kSlewDb per update (48 updates = 64 ms each way at 48 kHz), because gain, frequency and width may
  // be moving at the same time and their steps add up.
  static constexpr float kFadeStep = 0.5f * kSlewDb / kEqBandMaxDb;

  void Configure(int channels, int sampleRate)
  {
    channels_ = std::min(std::max(channels, 1), kMaxChannels);
    rate_ = std::max(8000, sampleRate);
    const BandShape* def = DefaultBandShapes();
    for (int b = 0; b < kBands; ++b)
    {
      Shape& s = shape_[b];
      s = Shape{};
      s.freq = s.tFreq = def[b].freqHz;
      s.width = s.tWidth = def[b].widthOct;
      s.type = s.tType = kBandBell;
      s.geom = MakeBand(static_cast<double>(s.freq), static_cast<double>(s.width), rate_, s.type);
    }
    cells_.assign(static_cast<size_t>(channels_) * kBands, Cell{});
    preTargetDb_ = 0.0f;
    Reset();
  }

  // Targets. A band whose frequency the sample rate cannot realise (too close to Nyquist) is switched
  // off (its gain fades to 0 and it keeps its last shape).
  void SetParams(const EqParams& p)
  {
    for (int b = 0; b < kBands; ++b)
    {
      Shape& s = shape_[b];
      const BandShape& sh = p.shape[b];
      const bool ok = static_cast<double>(sh.freqHz) < 0.49 * static_cast<double>(rate_);
      s.usable = ok;
      if (ok)
      {
        s.tFreq = sh.freqHz;
        s.tWidth = sh.widthOct;
        s.tType = NormalizeBandType(sh.type);
      }
      else
        s.tType = kBandBell;
      for (int c = 0; c < channels_; ++c)
        cells_[Index(c, b)].target = ok ? p.bandDb[c][b] : 0.0f;
    }
    preTargetDb_ = p.preampDb;
  }

  // Clears all filter memory and jumps straight to the targets (no slewing).
  void Reset()
  {
    for (int b = 0; b < kBands; ++b)
    {
      Shape& s = shape_[b];
      s.freq = s.tFreq;
      s.width = s.tWidth;
      s.type = s.tType;
      s.level = s.levelPrev = 1.0f;
      s.geom = MakeBand(static_cast<double>(s.freq), static_cast<double>(s.width), rate_, s.type);
      for (int c = 0; c < channels_; ++c)
      {
        Cell& cell = cells_[Index(c, b)];
        cell.cur = cell.target;
        cell.f = Biquad{};
        SetCoeffs(s.geom, s.type, static_cast<double>(cell.cur * s.level), cell.f);
      }
    }
    preCurDb_ = preTargetDb_;
    preStart_ = preEnd_ = DbToLin(preCurDb_);
    preStep_ = 0.0f;
    phase_ = 0;
  }

  void Process(float* const* planes, int frames)
  {
    if (cells_.empty() || frames <= 0)
      return;
    int done = 0;
    while (done < frames)
    {
      if (phase_ == 0)
      {
        UpdateCoefficients();
        phase_ = kSub;
      }
      const int n = std::min(frames - done, phase_);
      const int consumed = kSub - phase_;  // samples of this sub-block already processed
      for (int c = 0; c < channels_; ++c)
        RunChannel(c, planes[c] + done, n, consumed);
      phase_ -= n;
      done += n;
    }
  }

  // Q equivalent of a width in octaves at a given centre frequency (display only).
  static double WidthToQ(double widthOct, double freqHz, int sampleRate, int type = kBandBell)
  {
    const double w0 = 2.0 * kPi * freqHz / static_cast<double>(sampleRate);
    const double s = std::sin(w0);
    if (s <= 1e-9)
      return 1.0;
    const double k = 0.5 * std::log(2.0) * widthOct;
    return 1.0 / (2.0 * std::sinh(WidthIsPrewarped(NormalizeBandType(type)) ? k * w0 / s : k));
  }

  // Magnitude (dB) of the whole cascade for one channel's gains and the given band shapes — what the UI
  // draws as the curve.
  static double ResponseDb(const float* bandDb, const BandShape* shape, double freqHz, int sampleRate)
  {
    double out = 0.0;
    ResponseCurveDb(bandDb, shape, sampleRate, &freqHz, 1, &out);
    return out;
  }
  static double ResponseDb(const float* bandDb, double freqHz, int sampleRate)
  {
    return ResponseDb(bandDb, DefaultBandShapes(), freqHz, sampleRate);
  }

  // Response (dB) of the whole cascade at `n` frequencies: what the Studio draws. The band coefficients
  // are computed once, not once per point.
  static void ResponseCurveDb(const float* bandDb, const BandShape* shape, int sampleRate, const double* freqsHz, int n,
                              double* outDb)
  {
    Biquad f[kBands];
    bool used[kBands];
    for (int b = 0; b < kBands; ++b)
    {
      const int type = NormalizeBandType(shape[b].type);
      const BandGeom g = MakeBand(static_cast<double>(shape[b].freqHz), static_cast<double>(shape[b].widthOct), sampleRate, type);
      used[b] = g.valid && (!BandTypeUsesGain(type) || bandDb[b] != 0.0f);
      if (used[b])
        SetCoeffs(g, type, static_cast<double>(bandDb[b]), f[b]);
    }
    for (int i = 0; i < n; ++i)
    {
      const double w = 2.0 * kPi * freqsHz[i] / static_cast<double>(sampleRate);
      double total = 0.0;
      for (int b = 0; b < kBands; ++b)
        if (used[b])
          total += BiquadMagnitudeDb(f[b], w);
      outDb[i] = total;
    }
  }
  static void ResponseCurveDb(const float* bandDb, int sampleRate, const double* freqsHz, int n, double* outDb)
  {
    ResponseCurveDb(bandDb, DefaultBandShapes(), sampleRate, freqsHz, n, outDb);
  }

private:
  static constexpr double kPi = 3.14159265358979323846;
  static constexpr double kFlush = 1e-20;

  struct BandGeom
  {
    double cosW0 = 1.0;
    double alpha = 0.0;
    bool valid = false;
  };
  struct Biquad
  {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    double z1 = 0.0, z2 = 0.0;
  };
  struct Cell   // one channel of one band: own gain, own filter state
  {
    Biquad f;
    float cur = 0.0f;
    float target = 0.0f;
  };
  struct Shape  // one band, shared by all channels
  {
    float freq = 1000.0f, tFreq = 1000.0f;
    float width = 0.34f, tWidth = 0.34f;
    int type = kBandBell, tType = kBandBell;
    float level = 1.0f, levelPrev = 1.0f;   // 1 = fully in; fades through 0 while the type changes
    bool usable = true;
    BandGeom geom;
  };

  static float DbToLin(float db) { return std::pow(10.0f, db / 20.0f); }

  // Bell and notch: width is a bandwidth in octaves, pre-warped (RBJ "BW") so the default layout is
  // exactly the old graphic EQ. Shelves and cut filters: width is the same number turned into a plain Q
  // (1.9 octaves = Q 0.707, the maximally flat slope) that does not drift with the frequency.
  static bool WidthIsPrewarped(int type) { return type == kBandBell || type == kBandNotch; }

  static BandGeom MakeBand(double fc, double widthOct, int rate, int type)
  {
    BandGeom g;
    if (fc >= 0.49 * static_cast<double>(rate))
      return g;
    const double w0 = 2.0 * kPi * fc / static_cast<double>(rate);
    const double s = std::sin(w0);
    const double k = 0.5 * std::log(2.0) * widthOct;
    g.alpha = s * std::sinh(WidthIsPrewarped(type) ? k * w0 / s : k);
    g.cosW0 = std::cos(w0);
    g.valid = true;
    return g;
  }

  // RBJ cookbook. Bell/shelves at exactly 0 dB: numerator equals denominator bit for bit.
  static void SetCoeffs(const BandGeom& g, int type, double gainDb, Biquad& f)
  {
    const double c = g.cosW0, al = g.alpha;
    double b0, b1, b2, a0, a1, a2;
    switch (type)
    {
      case kBandLowShelf:
      {
        const double A = std::pow(10.0, gainDb / 40.0);
        const double sq = 2.0 * std::sqrt(A) * al;
        b0 = A * ((A + 1.0) - (A - 1.0) * c + sq);
        b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * c);
        b2 = A * ((A + 1.0) - (A - 1.0) * c - sq);
        a0 = (A + 1.0) + (A - 1.0) * c + sq;
        a1 = -2.0 * ((A - 1.0) + (A + 1.0) * c);
        a2 = (A + 1.0) + (A - 1.0) * c - sq;
        break;
      }
      case kBandHighShelf:
      {
        const double A = std::pow(10.0, gainDb / 40.0);
        const double sq = 2.0 * std::sqrt(A) * al;
        b0 = A * ((A + 1.0) + (A - 1.0) * c + sq);
        b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * c);
        b2 = A * ((A + 1.0) + (A - 1.0) * c - sq);
        a0 = (A + 1.0) - (A - 1.0) * c + sq;
        a1 = 2.0 * ((A - 1.0) - (A + 1.0) * c);
        a2 = (A + 1.0) - (A - 1.0) * c - sq;
        break;
      }
      case kBandNotch:
        b0 = 1.0; b1 = -2.0 * c; b2 = 1.0;
        a0 = 1.0 + al; a1 = -2.0 * c; a2 = 1.0 - al;
        break;
      case kBandHighPass:
        b0 = (1.0 + c) / 2.0; b1 = -(1.0 + c); b2 = (1.0 + c) / 2.0;
        a0 = 1.0 + al; a1 = -2.0 * c; a2 = 1.0 - al;
        break;
      case kBandLowPass:
        b0 = (1.0 - c) / 2.0; b1 = 1.0 - c; b2 = (1.0 - c) / 2.0;
        a0 = 1.0 + al; a1 = -2.0 * c; a2 = 1.0 - al;
        break;
      default:  // bell (peaking)
      {
        const double A = std::pow(10.0, gainDb / 40.0);
        b0 = 1.0 + al * A; b1 = -2.0 * c; b2 = 1.0 - al * A;
        a0 = 1.0 + al / A; a1 = -2.0 * c; a2 = 1.0 - al / A;
        break;
      }
    }
    f.b0 = b0 / a0;
    f.b1 = b1 / a0;
    f.b2 = b2 / a0;
    f.a1 = a1 / a0;
    f.a2 = a2 / a0;
  }

  static double BiquadMagnitudeDb(const Biquad& f, double w)
  {
    const double c1 = std::cos(w), s1 = std::sin(w), c2 = std::cos(2.0 * w), s2 = std::sin(2.0 * w);
    const double nr = f.b0 + f.b1 * c1 + f.b2 * c2;
    const double ni = -(f.b1 * s1 + f.b2 * s2);
    const double dr = 1.0 + f.a1 * c1 + f.a2 * c2;
    const double di = -(f.a1 * s1 + f.a2 * s2);
    const double num = std::max(nr * nr + ni * ni, 1e-30);   // a notch is allowed to be (nearly) silent
    return 10.0 * std::log10(num / (dr * dr + di * di));
  }

  size_t Index(int ch, int band) const { return static_cast<size_t>(ch) * kBands + static_cast<size_t>(band); }

  static float Approach(float cur, float target, float step)
  {
    const float d = target - cur;
    return (std::fabs(d) <= step) ? target : cur + (d > 0.0f ? step : -step);
  }

  // Frequency and width move in the log domain, a fixed number of octaves per update.
  static float ApproachLog(float cur, float target, float stepOct)
  {
    const double d = std::log2(static_cast<double>(target) / static_cast<double>(cur));
    if (std::fabs(d) <= static_cast<double>(stepOct))
      return target;
    return static_cast<float>(static_cast<double>(cur) * std::exp2(d > 0.0 ? static_cast<double>(stepOct) : -static_cast<double>(stepOct)));
  }

  void UpdateCoefficients()
  {
    for (int b = 0; b < kBands; ++b)
    {
      Shape& s = shape_[b];
      bool geomChanged = false;
      s.levelPrev = s.level;
      if (s.type != s.tType)
      {
        if (s.level == 0.0f)
        {
          if (!BandTypeUsesGain(s.type))   // a notch / cut filter holds real signal in its memory: drop it, do not let it ring
            for (int c = 0; c < channels_; ++c)
            {
              Biquad& f = cells_[Index(c, b)].f;
              f.z1 = f.z2 = 0.0;
            }
          s.type = s.tType;              // swapped while the band is transparent
          geomChanged = true;
        }
        else
          s.level = std::max(0.0f, s.level - kFadeStep);
      }
      else if (s.level < 1.0f)
        s.level = std::min(1.0f, s.level + kFadeStep);

      if (s.freq != s.tFreq)
      {
        s.freq = ApproachLog(s.freq, s.tFreq, kFreqSlewOct);
        geomChanged = true;
      }
      if (s.width != s.tWidth)
      {
        s.width = ApproachLog(s.width, s.tWidth, kWidthSlewOct);
        geomChanged = true;
      }
      if (geomChanged)
        s.geom = MakeBand(static_cast<double>(s.freq), static_cast<double>(s.width), rate_, s.type);
      const bool levelChanged = s.level != s.levelPrev;

      for (int c = 0; c < channels_; ++c)
      {
        Cell& cell = cells_[Index(c, b)];
        const bool gainChanged = cell.cur != cell.target;
        if (gainChanged)
          cell.cur = Approach(cell.cur, cell.target, kSlewDb);
        if (gainChanged || levelChanged || geomChanged)
          SetCoeffs(s.geom, s.type, static_cast<double>(cell.cur * s.level), cell.f);
      }
    }

    preCurDb_ = Approach(preCurDb_, preTargetDb_, kSlewDb);
    preStart_ = preEnd_;
    preEnd_ = DbToLin(preCurDb_);
    preStep_ = (preEnd_ - preStart_) / static_cast<float>(kSub);
  }

  // n <= kSub samples of one channel, in place. `consumed` = position of x[0] inside the sub-block.
  void RunChannel(int ch, float* x, int n, int consumed)
  {
    double buf[kSub];
    const bool preUnity = (preStart_ == 1.0f && preEnd_ == 1.0f);
    for (int i = 0; i < n; ++i)
    {
      const float g = preStart_ + preStep_ * static_cast<float>(consumed + i + 1);
      buf[i] = preUnity ? static_cast<double>(x[i]) : static_cast<double>(x[i]) * static_cast<double>(g);
    }

    bool touched = !preUnity;
    for (int b = 0; b < kBands; ++b)
    {
      const Shape& s = shape_[b];
      Cell& cell = cells_[Index(ch, b)];
      Biquad& f = cell.f;
      const bool mixType = !BandTypeUsesGain(s.type);
      const bool active = mixType || cell.cur * s.level != 0.0f || std::fabs(f.z1) > kFlush || std::fabs(f.z2) > kFlush;
      if (!active)
        continue;
      touched = true;
      double z1 = f.z1, z2 = f.z2;
      if (mixType && !(s.levelPrev == 1.0f && s.level == 1.0f))
      {
        // notch / cut band while it fades: the filter keeps running, only its share in the output ramps
        const float step = (s.level - s.levelPrev) / static_cast<float>(kSub);
        for (int i = 0; i < n; ++i)
        {
          const double in = buf[i];
          const double out = f.b0 * in + z1;
          z1 = f.b1 * in - f.a1 * out + z2;
          z2 = f.b2 * in - f.a2 * out;
          const double m = static_cast<double>(s.levelPrev + step * static_cast<float>(consumed + i + 1));
          buf[i] = in + m * (out - in);
        }
      }
      else
      {
        for (int i = 0; i < n; ++i)
        {
          const double in = buf[i];
          const double out = f.b0 * in + z1;
          z1 = f.b1 * in - f.a1 * out + z2;
          z2 = f.b2 * in - f.a2 * out;
          buf[i] = out;
        }
      }
      if (std::fabs(z1) < kFlush && std::fabs(z2) < kFlush)
        z1 = z2 = 0.0;  // flush denormal-range tails (far below -400 dBFS, inaudible)
      f.z1 = z1;
      f.z2 = z2;
    }

    if (touched)
      for (int i = 0; i < n; ++i)
        x[i] = static_cast<float>(buf[i]);
  }

  int channels_ = 0;
  int rate_ = 48000;
  Shape shape_[kBands];
  std::vector<Cell> cells_;

  float preTargetDb_ = 0.0f, preCurDb_ = 0.0f;
  float preStart_ = 1.0f, preEnd_ = 1.0f, preStep_ = 0.0f;
  int phase_ = 0;
};

}  // namespace vac3dsp
