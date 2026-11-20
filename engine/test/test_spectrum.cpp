// test_spectrum.cpp — spectrum taps and analyzer: ring buffer order and thread safety, FFT-based levels
// (a sine reads its amplitude in dBFS at its own frequency), channel pairing, channel combination, and the
// taps inside the DSP chain.
#include "doctest.h"
#include "dsp/DspChain.h"
#include "dsp/Spectrum.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>
#include <vector>

using namespace vac3dsp;

#define CHECK_NEAR(a, b, tol)                    \
  do                                             \
  {                                              \
    const double va_ = static_cast<double>(a);   \
    const double vb_ = static_cast<double>(b);   \
    INFO(va_ << " vs " << vb_);                  \
    CHECK(std::fabs(va_ - vb_) <= (tol));        \
  } while (0)

namespace {
constexpr double kPi = 3.14159265358979323846;

int BandNear(double hz)
{
  int best = 0;
  for (int i = 1; i < kSpecBands; ++i)
    if (std::fabs(std::log(SpectrumBandHz(i) / hz)) < std::fabs(std::log(SpectrumBandHz(best) / hz)))
      best = i;
  return best;
}

void PushSines(SpectrumTap& tap, int channels, int frames, int rate, const double* hz, const double* amp)
{
  std::vector<std::vector<float>> data(static_cast<size_t>(channels), std::vector<float>(static_cast<size_t>(frames)));
  const float* planes[kMaxChannels] = {};
  for (int c = 0; c < channels; ++c)
  {
    for (int i = 0; i < frames; ++i)
      data[static_cast<size_t>(c)][static_cast<size_t>(i)] =
          static_cast<float>(amp[c] * std::sin(2.0 * kPi * hz[c] * i / rate));
    planes[c] = data[static_cast<size_t>(c)].data();
  }
  tap.Push(planes, frames);
}
}  // namespace

TEST_CASE("spectrum tap: newest samples in time order, per channel; not enough data -> false")
{
  SpectrumTap tap;
  tap.Configure(2);
  std::vector<float> out(2 * 8);
  CHECK_FALSE(tap.Latest(8, out.data()));
  std::vector<float> a(5), b(5);
  const float* planes[2] = {a.data(), b.data()};
  for (int round = 0; round < 3; ++round)   // samples 0..14 in three chunks of 5
  {
    for (int i = 0; i < 5; ++i)
    {
      a[static_cast<size_t>(i)] = static_cast<float>(round * 5 + i);
      b[static_cast<size_t>(i)] = -static_cast<float>(round * 5 + i);
    }
    tap.Push(planes, 5);
  }
  CHECK(tap.Written() == 15u);
  REQUIRE(tap.Latest(8, out.data()));
  for (int i = 0; i < 8; ++i)
  {
    CHECK(out[static_cast<size_t>(i)] == static_cast<float>(7 + i));          // the last 8 of 0..14 are 7..14
    CHECK(out[static_cast<size_t>(8 + i)] == -static_cast<float>(7 + i));
  }
}

TEST_CASE("spectrum tap: wraps around the ring and never returns a torn window while the audio thread writes")
{
  SpectrumTap tap;
  tap.Configure(1);
  std::atomic<bool> stop{false};
  std::thread writer([&] {
    std::vector<float> chunk(64);
    const float* planes[1] = {chunk.data()};
    float v = 0.0f;
    while (!stop.load())
    {
      for (auto& s : chunk)
      {
        s = v;
        v += 1.0f;
      }
      tap.Push(planes, 64);
    }
  });
  std::vector<float> out(4096);
  int good = 0;
  bool bad = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (good < 3000 && std::chrono::steady_clock::now() < deadline)
    if (tap.Latest(4096, out.data()))
    {
      ++good;
      for (size_t i = 1; i < out.size(); ++i)
        if (out[i] != out[i - 1] + 1.0f)   // the writer counts up by one: a window must be one unbroken run
          bad = true;
    }
  stop = true;
  writer.join();
  CHECK_FALSE(bad);
  CHECK(good >= 100);
}

TEST_CASE("spectrum analyzer: a sine reads its amplitude in dBFS at its own frequency and little elsewhere")
{
  for (int rate : {44100, 48000, 96000})
  {
    SpectrumAnalyzer an;
    an.Configure(rate);
    for (int idx : {8, 30, 52, 74, 96, 110})   // tones exactly on band centres; one between two bands would split its energy
    {
      const double hz = SpectrumBandHz(idx);
      SpectrumTap tap;
      tap.Configure(1);
      const double h[1] = {hz}, a[1] = {0.5};
      PushSines(tap, 1, 20000, rate, h, a);
      float bands[kMaxChannels][kSpecBands];
      REQUIRE(an.Analyze(tap, bands));
      const int peak = idx;
      INFO("rate " << rate << " hz " << hz);
      CHECK_NEAR(bands[0][peak], -6.02, 1.0);
      // far from the tone the analyzer is quiet (Hann sidelobes)
      const int distant = BandNear(hz * 4.0 > 20000.0 ? hz / 4.0 : hz * 4.0);
      CHECK(bands[0][distant] < bands[0][peak] - 40.0f);
      CHECK(bands[1][peak] == kSpecFloorDb);   // channels the tap does not have stay at the floor
    }
  }
}

TEST_CASE("spectrum analyzer: channels come out in the right slots (pairs share one FFT), silence is the floor")
{
  SpectrumAnalyzer an;
  an.Configure(48000);
  SpectrumTap tap;
  tap.Configure(6);
  const double hz[6] = {SpectrumBandHz(20), SpectrumBandHz(35), SpectrumBandHz(50),
                        SpectrumBandHz(65), SpectrumBandHz(80), SpectrumBandHz(95)};
  const double amp[6] = {0.5, 0.25, 0.125, 0.0, 0.5, 0.0625};
  PushSines(tap, 6, 20000, 48000, hz, amp);
  float bands[kMaxChannels][kSpecBands];
  REQUIRE(an.Analyze(tap, bands));
  for (int c = 0; c < 6; ++c)
  {
    if (amp[c] == 0.0)
    {
      for (int b = 0; b < kSpecBands; ++b)
        CHECK(bands[c][b] <= kSpecFloorDb + 0.5f);
      continue;
    }
    const int peak = 20 + 15 * c;
    INFO("channel " << c);
    CHECK_NEAR(bands[c][peak], 20.0 * std::log10(amp[c]), 1.0);
    for (int other = 0; other < 6; ++other)   // no leakage from neighbours of the packed pair
      if (other != c && amp[other] > 0.0 && std::fabs(hz[other] - hz[c]) > 1.0)
        CHECK(bands[c][BandNear(hz[other])] < bands[c][peak] - 40.0f);
  }
}

TEST_CASE("spectrum analyzer: not enough samples yet -> false, floor everywhere")
{
  SpectrumAnalyzer an;
  an.Configure(48000);
  SpectrumTap tap;
  tap.Configure(2);
  const double h[2] = {1000.0, 1000.0}, a[2] = {0.5, 0.5};
  PushSines(tap, 2, 1000, 48000, h, a);
  float bands[kMaxChannels][kSpecBands];
  CHECK_FALSE(an.Analyze(tap, bands));
  CHECK(bands[0][BandNear(1000.0)] == kSpecFloorDb);
}

TEST_CASE("spectrum: band table and channel combination")
{
  CHECK_NEAR(SpectrumBandHz(0), 20.0, 1e-9);
  CHECK_NEAR(SpectrumBandHz(kSpecBands - 1), 20000.0, 1e-6);
  float bands[kMaxChannels][kSpecBands];
  for (auto& row : bands)
    for (float& v : row)
      v = kSpecFloorDb;
  bands[0][10] = -20.0f;
  bands[1][10] = -20.0f;
  bands[2][11] = -30.0f;
  float out[kSpecBands];
  CombineSpectrum(bands, 0, out);
  CHECK(out[10] == -20.0f);
  CHECK(out[11] == kSpecFloorDb);
  CombineSpectrum(bands, -1, out);
  CHECK_NEAR(out[10], -20.0 + 3.0103, 0.01);   // two equal channels: +3 dB
  CHECK_NEAR(out[11], -30.0, 0.01);
  CHECK(out[12] == kSpecFloorDb);
}

TEST_CASE("dsp chain taps: off = nothing copied; on = exact copies of the chain's input and output")
{
  DspChain chain;
  chain.Configure(2, 48000);
  DspParams p;
  p.enabled = 1;
  for (int c = 0; c < kMaxChannels; ++c)
    p.eq.bandDb[c][12] = 9.0f;
  chain.SetParams(p);
  const int frames = 6000;
  std::vector<float> l(static_cast<size_t>(frames)), r(static_cast<size_t>(frames));
  for (int i = 0; i < frames; ++i)
  {
    l[static_cast<size_t>(i)] = static_cast<float>(0.3 * std::sin(2.0 * kPi * 700.0 * i / 48000));
    r[static_cast<size_t>(i)] = static_cast<float>(0.2 * std::sin(2.0 * kPi * 1500.0 * i / 48000));
  }
  const auto l0 = l, r0 = r;
  float* planes[2] = {l.data(), r.data()};
  chain.Process(planes, frames);
  CHECK(chain.TapIn().Written() == 0u);
  CHECK(chain.TapOut().Written() == 0u);

  l = l0;
  r = r0;
  chain.SetSpectrumTap(true);
  chain.Process(planes, frames);
  CHECK(chain.TapIn().Written() == static_cast<uint64_t>(frames));
  CHECK(chain.TapOut().Written() == static_cast<uint64_t>(frames));
  const int n = 2048;
  std::vector<float> in(static_cast<size_t>(2 * n)), out(static_cast<size_t>(2 * n));
  REQUIRE(chain.TapIn().Latest(n, in.data()));
  REQUIRE(chain.TapOut().Latest(n, out.data()));
  bool inSame = true, outSame = true, changed = false;
  for (int i = 0; i < n; ++i)
  {
    const size_t k = static_cast<size_t>(frames - n + i);
    inSame = inSame && in[static_cast<size_t>(i)] == l0[k] && in[static_cast<size_t>(n + i)] == r0[k];
    outSame = outSame && out[static_cast<size_t>(i)] == l[k] && out[static_cast<size_t>(n + i)] == r[k];
    changed = changed || l[k] != l0[k];
  }
  CHECK(inSame);    // the tap before the chain holds the untouched input
  CHECK(outSame);   // the tap after it holds exactly what the chain delivered
  CHECK(changed);   // and the EQ really did something
}
