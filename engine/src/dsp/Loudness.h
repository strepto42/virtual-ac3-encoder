// Loudness.h — loudness difference between the DSP chain's input and output, for the "match loudness" button.
//
// The engine already publishes spectra (120 log bands, dBFS) before and after the chain. A frame's loudness is
// the K-weighted (ITU-R BS.1770) power sum over bands and channels (surrounds count 1.41x in power, the LFE
// not at all). LoudnessMatcher averages the POWER of many frames, ignoring silence, and says by how many dB
// the output has to change to be as loud as the input.
#pragma once

#include "Spectrum.h"

#include <array>
#include <cmath>

namespace vac3dsp {

namespace loudness_detail {
inline double BiquadDb(double b0, double b1, double b2, double a0, double a1, double a2, double w)
{
  const double c1 = std::cos(w), s1 = std::sin(w), c2 = std::cos(2.0 * w), s2 = std::sin(2.0 * w);
  const double nr = b0 + b1 * c1 + b2 * c2, ni = -(b1 * s1 + b2 * s2);
  const double dr = a0 + a1 * c1 + a2 * c2, di = -(a1 * s1 + a2 * s2);
  return 10.0 * std::log10((nr * nr + ni * ni) / (dr * dr + di * di));
}
}  // namespace loudness_detail

// K-weighting magnitude (dB) at `hz` (the two BS.1770 filters: +4 dB high shelf and a 38 Hz high-pass).
inline double KWeightDb(double hz)
{
  using loudness_detail::BiquadDb;
  const double kPi = 3.14159265358979323846, fs = 48000.0;
  const double w = 2.0 * kPi * hz / fs;

  const double f0 = 1681.974450955533, gain = 3.999843853973347, q = 0.7071752369554196;   // stage 1: high shelf
  const double A = std::pow(10.0, gain / 40.0), w0 = 2.0 * kPi * f0 / fs, c = std::cos(w0), al = std::sin(w0) / (2.0 * q);
  const double sq = 2.0 * std::sqrt(A) * al;
  const double shelf = BiquadDb(A * ((A + 1.0) + (A - 1.0) * c + sq), -2.0 * A * ((A - 1.0) + (A + 1.0) * c),
                                A * ((A + 1.0) + (A - 1.0) * c - sq), (A + 1.0) - (A - 1.0) * c + sq,
                                2.0 * ((A - 1.0) - (A + 1.0) * c), (A + 1.0) - (A - 1.0) * c - sq, w);

  const double h0 = 38.13547087602444, hq = 0.5003270373238773;                           // stage 2: high-pass
  const double hw0 = 2.0 * kPi * h0 / fs, hc = std::cos(hw0), ha = std::sin(hw0) / (2.0 * hq);
  const double hp = BiquadDb((1.0 + hc) / 2.0, -(1.0 + hc), (1.0 + hc) / 2.0, 1.0 + ha, -2.0 * hc, 1.0 - ha, w);
  return shelf + hp;
}

class LoudnessMatcher
{
public:
  LoudnessMatcher()
  {
    for (int b = 0; b < kSpecBands; ++b)
      weight_[static_cast<size_t>(b)] = std::pow(10.0, KWeightDb(SpectrumBandHz(b)) / 10.0);
    Reset();
  }

  void Reset()
  {
    frames_ = 0;
    pIn_ = pOut_ = 0.0;
  }

  // One published frame. Frames in which the input is (nearly) silent are skipped.
  void AddFrame(const float (*in)[kSpecBands], const float (*out)[kSpecBands])
  {
    const double pi = FramePower(in), po = FramePower(out);
    if (pi < kSilence)
      return;
    pIn_ += pi;
    pOut_ += po;
    ++frames_;
  }

  int Frames() const { return frames_; }   // frames that counted (not silent)

  // How many dB the OUTPUT must change to be as loud as the INPUT (positive: the output is too quiet).
  double DeltaDb() const
  {
    if (frames_ == 0 || pOut_ <= 0.0)
      return 0.0;
    return 10.0 * std::log10(pIn_ / pOut_);
  }

private:
  static constexpr double kSilence = 1e-7;   // about -70 dB of K-weighted power

  double FramePower(const float (*spec)[kSpecBands]) const
  {
    static const double channelWeight[kMaxChannels] = {1.0, 1.0, 1.0, 0.0, 1.41, 1.41};
    double sum = 0.0;
    for (int c = 0; c < kMaxChannels; ++c)
    {
      if (channelWeight[c] == 0.0)
        continue;
      double p = 0.0;
      for (int b = 0; b < kSpecBands; ++b)
        if (spec[c][b] > kSpecFloorDb)
          p += std::pow(10.0, static_cast<double>(spec[c][b]) / 10.0) * weight_[static_cast<size_t>(b)];
      sum += channelWeight[c] * p;
    }
    return sum;
  }

  std::array<double, kSpecBands> weight_{};
  int frames_ = 0;
  double pIn_ = 0.0, pOut_ = 0.0;
};

}  // namespace vac3dsp
