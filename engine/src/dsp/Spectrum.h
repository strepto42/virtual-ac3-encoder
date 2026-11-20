// Spectrum.h — spectrum analyzer data for DSP Studio, kept off the audio thread's critical path.
//
//   SpectrumTap       the audio thread copies every chunk (before and after the DSP chain) into a ring of
//                     the most recent samples per channel: a few stores per sample, no locks, no allocation.
//   SpectrumAnalyzer  runs on the engine's main thread (every 100 ms, and only while DSP Studio is open):
//                     takes the latest window from a tap, applies a Hann window, FFT, and turns the result
//                     into kSpecBands log-spaced levels in dBFS (a sine of amplitude 1.0 reads 0 dB).
//   CombineSpectrum   Studio side: the spectrum of one channel, or the power sum of all channels.
#pragma once

#include "DspParams.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

namespace vac3dsp {

constexpr int kSpecBands = 120;          // 20 Hz .. 20 kHz, 1/12 octave apart
constexpr float kSpecFloorDb = -90.0f;

inline double SpectrumBandHz(int i)
{
  return 20.0 * std::pow(1000.0, static_cast<double>(i) / static_cast<double>(kSpecBands - 1));
}

// ---------------------------------------------------------------------------------------------
class SpectrumTap
{
public:
  static constexpr int kRing = 16384;  // samples per channel (power of two)

  // Not for the audio thread: allocates.
  void Configure(int channels)
  {
    channels_ = std::min(std::max(channels, 1), kMaxChannels);
    data_.reset(new std::atomic<float>[static_cast<size_t>(channels_) * kRing]);
    for (size_t i = 0; i < static_cast<size_t>(channels_) * kRing; ++i)
      data_[i].store(0.0f, std::memory_order_relaxed);
    written_.store(0, std::memory_order_release);
  }

  int channels() const { return channels_; }

  // Audio thread: `frames` samples of every channel (planes[c] points at the first of them).
  void Push(const float* const* planes, int frames)
  {
    if (!data_ || frames <= 0)
      return;
    const uint64_t w = written_.load(std::memory_order_relaxed);
    for (int c = 0; c < channels_; ++c)
    {
      std::atomic<float>* ring = data_.get() + static_cast<size_t>(c) * kRing;
      for (int i = 0; i < frames; ++i)
        ring[(w + static_cast<uint64_t>(i)) & (kRing - 1)].store(planes[c][i], std::memory_order_relaxed);
    }
    written_.store(w + static_cast<uint64_t>(frames), std::memory_order_release);
  }

  uint64_t Written() const { return written_.load(std::memory_order_acquire); }

  // Analysis thread: the newest n samples (n <= kRing / 2) of every channel, oldest first, as one block
  // `out[c * n + i]`. Returns false when fewer than n samples have been written yet, or when the audio
  // thread overwrote part of the window while it was being copied (just try again next time).
  bool Latest(int n, float* out) const
  {
    if (!data_ || n <= 0 || n > kRing / 2)
      return false;
    const uint64_t w1 = written_.load(std::memory_order_acquire);
    if (w1 < static_cast<uint64_t>(n))
      return false;
    const uint64_t first = w1 - static_cast<uint64_t>(n);
    for (int c = 0; c < channels_; ++c)
    {
      const std::atomic<float>* ring = data_.get() + static_cast<size_t>(c) * kRing;
      for (int i = 0; i < n; ++i)
        out[static_cast<size_t>(c) * static_cast<size_t>(n) + static_cast<size_t>(i)] =
            ring[(first + static_cast<uint64_t>(i)) & (kRing - 1)].load(std::memory_order_relaxed);
    }
    const uint64_t w2 = written_.load(std::memory_order_acquire);
    return w2 - w1 <= static_cast<uint64_t>(kRing - n);   // the oldest sample of the window is still intact
  }

private:
  int channels_ = 0;
  std::unique_ptr<std::atomic<float>[]> data_;
  std::atomic<uint64_t> written_{0};
};

// ---------------------------------------------------------------------------------------------
class SpectrumAnalyzer
{
public:
  void Configure(int sampleRate)
  {
    rate_ = std::max(8000, sampleRate);
    n_ = rate_ >= 88200 ? 8192 : 4096;
    window_.resize(static_cast<size_t>(n_));
    double sum = 0.0, sumSq = 0.0;
    for (int i = 0; i < n_; ++i)
    {
      const double w = 0.5 - 0.5 * std::cos(kTwoPi * static_cast<double>(i) / static_cast<double>(n_));
      window_[static_cast<size_t>(i)] = static_cast<float>(w);
      sum += w;
      sumSq += w * w;
    }
    // power of a sine of amplitude A reads A^2: (2 / sum)^2 undoes the window gain, / ENBW sums the lobe
    scale_ = static_cast<float>((2.0 / sum) * (2.0 / sum) / (static_cast<double>(n_) * sumSq / (sum * sum)));
    bitrev_.assign(static_cast<size_t>(n_), 0);
    int bits = 0;
    while ((1 << bits) < n_)
      ++bits;
    for (int i = 0; i < n_; ++i)
    {
      int r = 0;
      for (int b = 0; b < bits; ++b)
        if (i & (1 << b))
          r |= 1 << (bits - 1 - b);
      bitrev_[static_cast<size_t>(i)] = r;
    }
    cosT_.resize(static_cast<size_t>(n_ / 2));
    sinT_.resize(static_cast<size_t>(n_ / 2));
    for (int i = 0; i < n_ / 2; ++i)
    {
      cosT_[static_cast<size_t>(i)] = static_cast<float>(std::cos(kTwoPi * i / n_));
      sinT_[static_cast<size_t>(i)] = static_cast<float>(-std::sin(kTwoPi * i / n_));
    }
    re_.assign(static_cast<size_t>(n_), 0.0f);
    im_.assign(static_cast<size_t>(n_), 0.0f);
    power_[0].assign(static_cast<size_t>(n_ / 2 + 1), 0.0f);
    power_[1].assign(static_cast<size_t>(n_ / 2 + 1), 0.0f);
    block_.assign(static_cast<size_t>(n_) * kMaxChannels, 0.0f);
  }

  int window() const { return n_; }

  // Spectrum of every channel the tap carries; out[c][band] in dBFS. Channels the tap does not have are
  // left at the floor. Returns false when the tap does not hold a full window yet.
  bool Analyze(const SpectrumTap& tap, float out[kMaxChannels][kSpecBands])
  {
    for (int c = 0; c < kMaxChannels; ++c)
      for (int b = 0; b < kSpecBands; ++b)
        out[c][b] = kSpecFloorDb;
    const int channels = tap.channels();
    if (n_ <= 0 || channels <= 0 || !tap.Latest(n_, block_.data()))
      return false;
    for (int c = 0; c < channels; c += 2)   // two real channels per complex FFT
    {
      const float* a = block_.data() + static_cast<size_t>(c) * static_cast<size_t>(n_);
      const float* b = (c + 1 < channels) ? a + n_ : nullptr;
      for (int i = 0; i < n_; ++i)
      {
        const float w = window_[static_cast<size_t>(i)];
        re_[static_cast<size_t>(i)] = a[i] * w;
        im_[static_cast<size_t>(i)] = b ? b[i] * w : 0.0f;
      }
      Fft();
      SplitPair(b != nullptr);
      ToBands(power_[0].data(), out[c]);
      if (b)
        ToBands(power_[1].data(), out[c + 1]);
    }
    return true;
  }

private:
  static constexpr double kTwoPi = 6.28318530717958647692;

  void Fft()   // in place, radix-2, re_/im_
  {
    const size_t n = static_cast<size_t>(n_);
    for (size_t i = 0; i < n; ++i)
    {
      const size_t j = static_cast<size_t>(bitrev_[i]);
      if (j > i)
      {
        std::swap(re_[i], re_[j]);
        std::swap(im_[i], im_[j]);
      }
    }
    for (size_t len = 2; len <= n; len <<= 1)
    {
      const size_t half = len / 2, step = n / len;
      for (size_t i = 0; i < n; i += len)
        for (size_t k = 0; k < half; ++k)
        {
          const float wr = cosT_[k * step], wi = sinT_[k * step];
          const size_t u = i + k, v = i + k + half;
          const float tr = re_[v] * wr - im_[v] * wi;
          const float ti = re_[v] * wi + im_[v] * wr;
          re_[v] = re_[u] - tr;
          im_[v] = im_[u] - ti;
          re_[u] += tr;
          im_[u] += ti;
        }
    }
  }

  // After an FFT of z = a + j*b: separate the two real spectra and store their normalised powers.
  void SplitPair(bool haveB)
  {
    const size_t n = static_cast<size_t>(n_);
    for (size_t k = 0; k <= n / 2; ++k)
    {
      const size_t m = (n - k) & (n - 1);
      const float ar = 0.5f * (re_[k] + re_[m]), ai = 0.5f * (im_[k] - im_[m]);
      power_[0][k] = (ar * ar + ai * ai) * scale_;
      if (haveB)
      {
        const float br = 0.5f * (im_[k] + im_[m]), bi = -0.5f * (re_[k] - re_[m]);
        power_[1][k] = (br * br + bi * bi) * scale_;
      }
    }
  }

  // Power per bin -> level per log band: the power of all bins inside the band, summed.
  void ToBands(const float* power, float* out) const
  {
    const double binHz = static_cast<double>(rate_) / static_cast<double>(n_);
    const int last = n_ / 2;
    for (int i = 0; i < kSpecBands; ++i)
    {
      const double f = SpectrumBandHz(i);
      if (f >= 0.5 * rate_)
      {
        out[i] = kSpecFloorDb;
        continue;
      }
      // +-1/24 octave around the centre, but never narrower than +-1.5 bins: that is the main lobe of the
      // Hann window, so a tone keeps its full level even where a band is narrower than one FFT bin.
      const double c = f / binHz;
      const double lo = std::min(f * 0.97153194 / binHz, c - 1.5), hi = std::max(f * 1.02930224 / binHz, c + 1.5);
      const int k0 = std::max(1, static_cast<int>(std::ceil(lo)));
      const int k1 = std::min(last, static_cast<int>(std::floor(hi)));
      double p = 0.0;
      for (int k = k0; k <= k1; ++k)
        p += static_cast<double>(power[k]);
      out[i] = static_cast<float>(std::max(static_cast<double>(kSpecFloorDb), 10.0 * std::log10(std::max(p, 1e-12))));
    }
  }

  int rate_ = 48000;
  int n_ = 0;
  float scale_ = 1.0f;
  std::vector<float> window_, cosT_, sinT_, re_, im_, block_;
  std::vector<int> bitrev_;
  std::vector<float> power_[2];
};

// ---------------------------------------------------------------------------------------------
// Studio: spectrum of the chosen channel (0..5) or, with channel < 0, the power sum of all channels.
inline void CombineSpectrum(const float (*bands)[kSpecBands], int channel, float* out)
{
  for (int b = 0; b < kSpecBands; ++b)
  {
    if (channel >= 0 && channel < kMaxChannels)
    {
      out[b] = bands[channel][b];
      continue;
    }
    double p = 0.0;
    for (int c = 0; c < kMaxChannels; ++c)
      if (bands[c][b] > kSpecFloorDb)
        p += std::pow(10.0, static_cast<double>(bands[c][b]) / 10.0);
    out[b] = p > 0.0 ? static_cast<float>(std::max(static_cast<double>(kSpecFloorDb), 10.0 * std::log10(p))) : kSpecFloorDb;
  }
}

}  // namespace vac3dsp
