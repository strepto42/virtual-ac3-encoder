// test_dsp.cpp — DSP chain: 30-band graphic EQ, linked compressor, DspChain (bypass, order,
// meters, thread safety) and parameter sanitising.
#include "doctest.h"
#include "dsp/DspChain.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <thread>
#include <vector>

using namespace vac3dsp;

// Absolute tolerance check (dB or ratio); prints both values on failure.
#define CHECK_NEAR(a, b, tol)                    \
  do                                             \
  {                                              \
    const double va_ = static_cast<double>(a);   \
    const double vb_ = static_cast<double>(b);   \
    INFO(va_ << " vs " << vb_);                  \
    CHECK(std::fabs(va_ - vb_) <= (tol));        \
  } while (0)

namespace {
constexpr int kRate = 48000;
constexpr double kPi = 3.14159265358979323846;

struct Audio
{
  std::array<std::vector<float>, kMaxChannels> ch;
  explicit Audio(int frames = 0)
  {
    for (auto& v : ch)
      v.assign(static_cast<size_t>(frames), 0.0f);
  }
  int frames() const { return static_cast<int>(ch[0].size()); }
};

Audio Sine(int frames, float amp, double hz, int rate = kRate)
{
  Audio a(frames);
  for (auto& v : a.ch)
    for (int n = 0; n < frames; ++n)
      v[static_cast<size_t>(n)] = amp * static_cast<float>(std::sin(2.0 * kPi * hz * n / rate));
  return a;
}

// Square wave: |x| is constant, so the compressor's detector sees a steady level.
Audio Square(int frames, float amp, int ch = -1, float others = 0.0f)
{
  Audio a(frames);
  for (int c = 0; c < kMaxChannels; ++c)
    for (int n = 0; n < frames; ++n)
    {
      const float a_ = (ch < 0 || c == ch) ? amp : others;
      a.ch[static_cast<size_t>(c)][static_cast<size_t>(n)] = ((n / 24) % 2) ? -a_ : a_;
    }
  return a;
}

Audio Noise(int frames, float amp, uint32_t seed = 1)
{
  Audio a(frames);
  uint32_t s = seed;
  for (auto& v : a.ch)
    for (int n = 0; n < frames; ++n)
    {
      s = s * 1664525u + 1013904223u;
      v[static_cast<size_t>(n)] = amp * (static_cast<float>(s >> 8) / 8388608.0f - 1.0f);
    }
  return a;
}

template <class Proc>
void Run(Proc&& proc, Audio& a, int from, int to, int chunk)
{
  for (int pos = from; pos < to; pos += chunk)
  {
    const int n = std::min(chunk, to - pos);
    float* p[kMaxChannels];
    for (int c = 0; c < kMaxChannels; ++c)
      p[c] = a.ch[static_cast<size_t>(c)].data() + pos;
    proc(p, n);
  }
}

double RmsDb(const std::vector<float>& v, int from, int to)
{
  double s = 0.0;
  for (int i = from; i < to; ++i)
    s += static_cast<double>(v[static_cast<size_t>(i)]) * v[static_cast<size_t>(i)];
  return 10.0 * std::log10(s / (to - from) + 1e-30);
}

float Peak(const std::vector<float>& v, int from, int to)
{
  float m = 0.0f;
  for (int i = from; i < to; ++i)
    m = std::max(m, std::fabs(v[static_cast<size_t>(i)]));
  return m;
}

double ToDb(double lin) { return 20.0 * std::log10(lin + 1e-30); }

EqParams EqWith(int band, float db)
{
  EqParams e;
  for (int c = 0; c < kMaxChannels; ++c)
    e.bandDb[c][band] = db;
  return e;
}

// The band nearest to 1 kHz and its exact centre frequency (bands are log-spaced, not on 1 kHz).
const int kB1k = NearestBand(1000.0f);
const double kF1k = static_cast<double>(kBandHz[kB1k]);

// Steady-state gain of the EQ (dB) at `hz` for channel `ch`.
double MeasureEqGainDb(const EqParams& p, double hz, int rate = kRate, int ch = 0)
{
  GraphicEq eq;
  eq.Configure(kMaxChannels, rate);
  eq.SetParams(p);
  eq.Reset();
  // 4 s: a boosted 20 Hz band rings for ~140 ms per time constant, so give it time to settle
  const int total = rate * 4;
  Audio in = Sine(total, 0.1f, hz, rate);
  Audio out = in;
  Run([&](float* const* pl, int n) { eq.Process(pl, n); }, out, 0, total, 512);
  // measure over a whole number of periods so the RMS of a low-frequency sine is not biased
  const int periods = std::max(1, static_cast<int>(0.5 * hz));
  const int len = static_cast<int>(std::lround(periods * rate / hz));
  return RmsDb(out.ch[static_cast<size_t>(ch)], total - len, total) -
         RmsDb(in.ch[static_cast<size_t>(ch)], total - len, total);
}
}  // namespace

// ---------------------------------------------------------------------------------------------
TEST_CASE("band table: 30 log-spaced centres, 20 Hz .. 20 kHz")
{
  CHECK(kBands == 30);
  CHECK(kBandHz[0] == 20.0f);
  CHECK(kBandHz[kBands - 1] == 20000.0f);
  for (int b = 1; b < kBands; ++b)
  {
    CHECK(kBandHz[b] > kBandHz[b - 1]);
    const double ratio = static_cast<double>(kBandHz[b]) / static_cast<double>(kBandHz[b - 1]);
    CHECK_NEAR(ratio, 1.26896, 0.0005);  // constant ratio: the bands tile the range evenly
  }
}

TEST_CASE("EQ: all gains at 0 dB is a bit-exact pass-through, whatever the block size")
{
  for (int chunk : {1, 37, 64, 1536})
  {
    GraphicEq eq;
    eq.Configure(6, kRate);
    eq.SetParams(EqParams{});
    eq.Reset();
    Audio in = Noise(5000, 0.8f);
    Audio out = in;
    Run([&](float* const* p, int n) { eq.Process(p, n); }, out, 0, 5000, chunk);
    for (int c = 0; c < 6; ++c)
      CHECK(std::memcmp(in.ch[static_cast<size_t>(c)].data(), out.ch[static_cast<size_t>(c)].data(),
                        5000 * sizeof(float)) == 0);
  }
}

TEST_CASE("EQ: a single band hits its gain at the centre and leaves far-away frequencies alone")
{
  CHECK_NEAR(MeasureEqGainDb(EqWith(kB1k, 12.0f), kF1k), 12.0, 0.01);
  CHECK_NEAR(MeasureEqGainDb(EqWith(kB1k, -12.0f), kF1k), -12.0, 0.01);
  CHECK_NEAR(MeasureEqGainDb(EqWith(kB1k, 12.0f), 100.0), 0.0, 0.05);
  CHECK_NEAR(MeasureEqGainDb(EqWith(kB1k, 12.0f), 10000.0), 0.0, 0.05);

  // the two end bands: 20 Hz and 20 kHz
  CHECK_NEAR(MeasureEqGainDb(EqWith(0, 12.0f), 20.0), 12.0, 0.01);
  CHECK_NEAR(MeasureEqGainDb(EqWith(kBands - 1, 12.0f), 20000.0), 12.0, 0.02);
}

TEST_CASE("EQ: the response the UI draws (ResponseDb) matches what the filter really does")
{
  EqParams p;
  const float g[kBands] = {6, 4, 2, 0, -3, -6, -9, 0, 3, 6, 9, 12, 6, 0, -6, -12, 0, 3,
                           6, 3, 0, -3, -6, 0, 5, 7, 2, -4, -8, 4};
  for (int c = 0; c < kMaxChannels; ++c)
    for (int b = 0; b < kBands; ++b)
      p.bandDb[c][b] = g[b];
  for (double hz : {30.0, 80.0, 250.0, 700.0, 1000.0, 2300.0, 5500.0, 9000.0, 15000.0})
    CHECK_NEAR(MeasureEqGainDb(p, hz), GraphicEq::ResponseDb(g, hz, kRate), 0.002);
}

TEST_CASE("EQ: channels are independent")
{
  GraphicEq eq;
  eq.Configure(6, kRate);
  EqParams p;
  for (int b = 0; b < kBands; ++b)
    p.bandDb[2][b] = 9.0f;  // centre channel only
  eq.SetParams(p);
  eq.Reset();
  Audio in = Noise(4000, 0.5f);
  Audio out = in;
  Run([&](float* const* pl, int n) { eq.Process(pl, n); }, out, 0, 4000, 256);
  for (int c : {0, 1, 3, 4, 5})
    CHECK(std::memcmp(in.ch[static_cast<size_t>(c)].data(), out.ch[static_cast<size_t>(c)].data(),
                      4000 * sizeof(float)) == 0);
  CHECK(std::memcmp(in.ch[2].data(), out.ch[2].data(), 4000 * sizeof(float)) != 0);
}

TEST_CASE("EQ: result does not depend on how the audio is split into blocks (even while gains move)")
{
  std::vector<Audio> results;
  for (int chunk : {1, 37, 64, 500})
  {
    GraphicEq eq;
    eq.Configure(6, kRate);
    eq.SetParams(EqWith(NearestBand(500.0f), 6.0f));
    eq.Reset();
    Audio a = Noise(6000, 0.4f, 7);
    Run([&](float* const* pl, int n) { eq.Process(pl, n); }, a, 0, 2000, chunk);
    EqParams moved = EqWith(NearestBand(500.0f), -10.0f);
    moved.preampDb = -5.0f;
    eq.SetParams(moved);
    Run([&](float* const* pl, int n) { eq.Process(pl, n); }, a, 2000, 6000, chunk);
    results.push_back(a);
  }
  for (size_t i = 1; i < results.size(); ++i)
    CHECK(std::memcmp(results[0].ch[0].data(), results[i].ch[0].data(), 6000 * sizeof(float)) == 0);
}

TEST_CASE("EQ: moving a fader does not click")
{
  GraphicEq eq;
  eq.Configure(6, kRate);
  eq.SetParams(EqParams{});
  eq.Reset();
  const int total = kRate / 2;
  Audio a = Sine(total, 0.25f, kF1k);
  Run([&](float* const* pl, int n) { eq.Process(pl, n); }, a, 0, 4000, 256);
  eq.SetParams(EqWith(kB1k, 12.0f));  // +12 dB: the sine at the band centre becomes 4x louder
  Run([&](float* const* pl, int n) { eq.Process(pl, n); }, a, 4000, total, 256);

  float maxStep = 0.0f;
  for (int n = 4001; n < total; ++n)
    maxStep = std::max(maxStep, std::fabs(a.ch[0][static_cast<size_t>(n)] - a.ch[0][static_cast<size_t>(n - 1)]));
  const float finalMaxSlope = 1.0f * static_cast<float>(2.0 * kPi * kF1k / kRate);  // 4 * 0.25 amplitude
  CHECK(maxStep <= finalMaxSlope * 1.2f);
  // and it really reached +12 dB
  CHECK(RmsDb(a.ch[0], total - 4800, total) - RmsDb(Sine(total, 0.25f, kF1k).ch[0], total - 4800, total) ==
        doctest::Approx(12.0).epsilon(0.01));
}

TEST_CASE("EQ: preamp scales the signal and a cut-only curve cannot overshoot the input level")
{
  GraphicEq eq;
  eq.Configure(6, kRate);
  EqParams p;
  p.preampDb = -6.0f;
  eq.SetParams(p);
  eq.Reset();
  Audio a = Sine(kRate / 2, 0.5f, 440.0);
  Run([&](float* const* pl, int n) { eq.Process(pl, n); }, a, 0, kRate / 2, 480);
  CHECK_NEAR(ToDb(Peak(a.ch[0], 4000, kRate / 2) / 0.5), -6.0, 0.01);
}

TEST_CASE("EQ: stable and finite at every supported sample rate, even with every band at +12 / -12 dB")
{
  for (int rate : {32000, 44100, 48000, 96000})
    for (float db : {12.0f, -12.0f})
    {
      GraphicEq eq;
      eq.Configure(6, rate);
      EqParams p;
      for (int c = 0; c < kMaxChannels; ++c)
        for (int b = 0; b < kBands; ++b)
          p.bandDb[c][b] = db;
      eq.SetParams(p);
      eq.Reset();
      Audio a = Noise(rate, 0.5f, 3);
      Run([&](float* const* pl, int n) { eq.Process(pl, n); }, a, 0, rate, 1024);
      float pk = 0.0f;
      for (int n = 0; n < rate; ++n)
      {
        REQUIRE(std::isfinite(a.ch[0][static_cast<size_t>(n)]));
        pk = std::max(pk, std::fabs(a.ch[0][static_cast<size_t>(n)]));
      }
      CHECK(pk < 20.0f);
    }
}

TEST_CASE("EQ: filter tails die out to exact zero (no denormal slowdown after long silence)")
{
  GraphicEq eq;
  eq.Configure(6, kRate);
  eq.SetParams(EqWith(0, 12.0f));  // the 20 Hz band rings the longest
  eq.Reset();
  const int total = kRate * 16;
  Audio a(total);
  for (int n = 0; n < 4800; ++n)
    a.ch[0][static_cast<size_t>(n)] = 0.5f * static_cast<float>(std::sin(2.0 * kPi * 20.0 * n / kRate));
  Run([&](float* const* pl, int n) { eq.Process(pl, n); }, a, 0, total, 1536);
  for (int n = total - 2000; n < total; ++n)
    REQUIRE(a.ch[0][static_cast<size_t>(n)] == 0.0f);
}

// ---------------------------------------------------------------------------------------------
TEST_CASE("compressor: static curve — hard knee, soft knee, ratio 1")
{
  CompParams k;
  k.thresholdDb = -30.0f;
  k.ratio = 4.0f;
  k.kneeDb = 0.0f;
  CHECK(LinkedCompressor::StaticGainDb(-40.0f, k) == 0.0f);
  CHECK(LinkedCompressor::StaticGainDb(-30.0f, k) == 0.0f);
  CHECK(LinkedCompressor::StaticGainDb(-10.0f, k) == doctest::Approx(-15.0f));  // 20 dB over -> 5 dB over
  k.kneeDb = 6.0f;
  CHECK(LinkedCompressor::StaticGainDb(-34.0f, k) == 0.0f);
  CHECK(LinkedCompressor::StaticGainDb(-30.0f, k) == doctest::Approx(-0.5625f));
  CHECK(LinkedCompressor::StaticGainDb(-27.0f, k) == doctest::Approx(-2.25f));  // knee end meets the line
  CHECK(LinkedCompressor::StaticGainDb(-10.0f, k) == doctest::Approx(-15.0f));
  k.ratio = 1.0f;
  CHECK(LinkedCompressor::StaticGainDb(-5.0f, k) == 0.0f);
}

TEST_CASE("compressor: steady-state gain reduction follows the curve")
{
  LinkedCompressor comp;
  comp.Configure(6, kRate);
  CompParams k;
  k.enabled = 1;
  k.thresholdDb = -30.0f;
  k.ratio = 4.0f;
  k.kneeDb = 0.0f;
  k.attackMs = 1.0f;
  k.releaseMs = 50.0f;
  comp.SetParams(k);
  comp.Reset();
  Audio a = Square(kRate, 0.316228f);  // -10 dBFS
  Run([&](float* const* p, int n) { comp.Process(p, n); }, a, 0, kRate, 512);
  CHECK_NEAR(ToDb(Peak(a.ch[0], kRate - 4800, kRate)), -25.0, 0.005);
  CHECK_NEAR(comp.TakeMaxReductionDb(), 15.0, 0.01);
  CHECK(comp.TakeMaxReductionDb() == 0.0f);  // the tracker resets

  Audio q = Square(kRate, 0.01f);  // -40 dBFS, below the threshold: untouched
  Run([&](float* const* p, int n) { comp.Process(p, n); }, q, 0, kRate, 512);
  CHECK_NEAR(ToDb(Peak(q.ch[0], kRate - 4800, kRate)), -40.0, 0.001);
}

TEST_CASE("compressor: attack and release times")
{
  LinkedCompressor comp;
  comp.Configure(6, kRate);
  CompParams k;
  k.enabled = 1;
  k.thresholdDb = -30.0f;
  k.ratio = 4.0f;
  k.kneeDb = 0.0f;
  k.attackMs = 20.0f;
  k.releaseMs = 100.0f;
  comp.SetParams(k);
  comp.Reset();

  const int quiet = kRate / 2, loud = kRate, tail = kRate * 2;
  Audio a(quiet + loud + tail);
  for (int n = 0; n < quiet + loud + tail; ++n)
  {
    const float amp = (n >= quiet && n < quiet + loud) ? 0.316228f : 0.001f;
    a.ch[0][static_cast<size_t>(n)] = ((n / 24) % 2) ? -amp : amp;
  }
  Audio ref = a;
  Run([&](float* const* p, int n) { comp.Process(p, n); }, a, 0, static_cast<int>(a.ch[0].size()), 256);
  auto gainDb = [&](int n) { return ToDb(std::fabs(a.ch[0][static_cast<size_t>(n)])) - ToDb(std::fabs(ref.ch[0][static_cast<size_t>(n)])); };

  CHECK_NEAR(gainDb(quiet - 10), 0.0, 0.01);
  // after one attack time constant 63% of the final -15 dB has been reached
  CHECK_NEAR(gainDb(quiet + kRate / 50), -15.0 * (1.0 - std::exp(-1.0)), 0.05);
  CHECK_NEAR(gainDb(quiet + loud - 10), -15.0, 0.01);
  // release: still compressing 20 ms after the loud part ends, fully recovered after 1.5 s
  CHECK(gainDb(quiet + loud + kRate / 50) < -10.0);
  CHECK(gainDb(quiet + loud + kRate * 3 / 2) > -0.1);
}

TEST_CASE("compressor: makeup gain, and ratio 1 with no makeup is bit-exact")
{
  LinkedCompressor comp;
  comp.Configure(6, kRate);
  CompParams k;
  k.enabled = 1;
  k.ratio = 1.0f;
  k.makeupDb = 6.0f;
  comp.SetParams(k);
  comp.Reset();
  Audio a = Sine(kRate, 0.2f, 1000.0);
  Run([&](float* const* p, int n) { comp.Process(p, n); }, a, 0, kRate, 512);
  CHECK_NEAR(ToDb(Peak(a.ch[0], kRate - 4800, kRate) / 0.2), 6.0, 0.01);

  k.makeupDb = 0.0f;
  comp.SetParams(k);
  comp.Reset();
  Audio n = Noise(3000, 0.9f);
  Audio ref = n;
  Run([&](float* const* p, int cnt) { comp.Process(p, cnt); }, n, 0, 3000, 300);
  CHECK(std::memcmp(ref.ch[0].data(), n.ch[0].data(), 3000 * sizeof(float)) == 0);
}

TEST_CASE("compressor: linked gain on all channels; LFE does not trigger it unless asked")
{
  CompParams k;
  k.enabled = 1;
  k.thresholdDb = -30.0f;
  k.ratio = 4.0f;
  k.kneeDb = 0.0f;
  k.attackMs = 1.0f;

  auto gainOn = [&](int loudCh, int probeCh, int lfeIn) {
    LinkedCompressor comp;
    comp.Configure(6, kRate);
    CompParams p = k;
    p.lfeInDetector = lfeIn;
    comp.SetParams(p);
    comp.Reset();
    Audio a = Square(kRate, 0.316228f, loudCh, 0.01f);
    Audio ref = a;
    Run([&](float* const* pl, int n) { comp.Process(pl, n); }, a, 0, kRate, 512);
    return ToDb(Peak(a.ch[static_cast<size_t>(probeCh)], kRate - 4800, kRate)) -
           ToDb(Peak(ref.ch[static_cast<size_t>(probeCh)], kRate - 4800, kRate));
  };

  CHECK_NEAR(gainOn(0, 0, 0), -15.0, 0.01);
  CHECK_NEAR(gainOn(0, 1, 0), -15.0, 0.01);  // quiet right channel ducks too
  CHECK_NEAR(gainOn(0, 3, 0), -15.0, 0.01);  // and so does the LFE
  CHECK_NEAR(gainOn(3, 0, 0), 0.0, 0.01);     // loud LFE alone: no compression
  CHECK_NEAR(gainOn(3, 0, 1), -15.0, 0.01);  // unless it is in the detector
}

// ---------------------------------------------------------------------------------------------
TEST_CASE("chain: default parameters and master off are a bit-exact bypass")
{
  DspChain chain;
  chain.Configure(6, kRate);
  Audio in = Noise(8000, 0.9f, 11);
  Audio out = in;
  Run([&](float* const* p, int n) { chain.Process(p, n); }, out, 0, 8000, 1536);
  CHECK(std::memcmp(in.ch[0].data(), out.ch[0].data(), 8000 * sizeof(float)) == 0);

  DspParams p;
  p.enabled = 0;
  p.eq = EqWith(kB1k, 12.0f);
  p.comp.enabled = 1;
  chain.SetParams(p);
  out = in;
  Run([&](float* const* pl, int n) { chain.Process(pl, n); }, out, 0, 8000, 1536);
  CHECK(std::memcmp(in.ch[5].data(), out.ch[5].data(), 8000 * sizeof(float)) == 0);
}

TEST_CASE("chain: switching DSP on and off mid-stream crossfades without clicks")
{
  DspChain chain;
  chain.Configure(6, kRate);
  const int total = kRate;
  Audio a = Sine(total, 0.25f, kF1k);
  DspParams on;
  on.enabled = 1;
  on.eq = EqWith(kB1k, 12.0f);

  Run([&](float* const* p, int n) { chain.Process(p, n); }, a, 0, 6000, 480);
  chain.SetParams(on);
  Run([&](float* const* p, int n) { chain.Process(p, n); }, a, 6000, 30000, 480);
  DspParams off = on;
  off.enabled = 0;
  chain.SetParams(off);
  Run([&](float* const* p, int n) { chain.Process(p, n); }, a, 30000, total, 480);

  float maxStep = 0.0f;
  for (int n = 1; n < total; ++n)
    maxStep = std::max(maxStep, std::fabs(a.ch[0][static_cast<size_t>(n)] - a.ch[0][static_cast<size_t>(n - 1)]));
  const float finalSlope = 1.0f * static_cast<float>(2.0 * kPi * kF1k / kRate);  // loudest part: 4 x 0.25
  CHECK(maxStep <= finalSlope * 1.35f);
  // processed section is +12 dB, and the end is the untouched sine again
  CHECK_NEAR(Peak(a.ch[0], 20000, 24000), 1.0f, 0.02);
  CHECK_NEAR(Peak(a.ch[0], total - 2400, total), 0.25f, 0.005);
}

TEST_CASE("chain: stage order matters (EQ boost before or after the compressor)")
{
  auto levelDb = [&](int order) {
    DspChain chain;
    chain.Configure(6, kRate);
    DspParams p;
    p.enabled = 1;
    p.order = order;
    p.eq = EqWith(kB1k, 12.0f);
    p.comp.enabled = 1;
    p.comp.thresholdDb = -20.0f;
    p.comp.ratio = 4.0f;
    p.comp.kneeDb = 0.0f;
    p.comp.attackMs = 1.0f;
    p.comp.releaseMs = 50.0f;
    chain.SetParams(p);
    Audio a = Sine(kRate, 0.1f, kF1k);  // -20 dBFS: sits exactly on the threshold
    Run([&](float* const* pl, int n) { chain.Process(pl, n); }, a, 0, kRate, 512);
    return ToDb(Peak(a.ch[0], kRate - 4800, kRate));
  };
  const double eqFirst = levelDb(0);    // boosted to -8 dB, then compressed
  const double compFirst = levelDb(1);  // untouched by the compressor, then boosted to -8 dB
  CHECK_NEAR(compFirst, -8.0, 0.05);
  CHECK(eqFirst < compFirst - 4.0);
}

TEST_CASE("chain: meters report peaks and reset after reading")
{
  DspChain chain;
  chain.Configure(6, kRate);
  Audio a = Sine(4800, 0.5f, 1000.0);
  Run([&](float* const* p, int n) { chain.Process(p, n); }, a, 0, 4800, 480);
  DspMeters m = chain.TakeMeters();
  CHECK_NEAR(m.inPeak[0], 0.5f, 0.002);
  CHECK(m.outPeak[0] == m.inPeak[0]);
  CHECK(m.compReductionDb == 0.0f);
  m = chain.TakeMeters();
  CHECK(m.inPeak[0] == 0.0f);
}

TEST_CASE("chain: parameters changed from another thread never break the audio thread")
{
  DspChain chain;
  chain.Configure(6, kRate);
  std::atomic<bool> stop{false};
  std::thread writer([&] {
    uint32_t s = 12345;
    while (!stop.load())
    {
      DspParams p;
      s = s * 1664525u + 1013904223u;
      p.enabled = (s >> 3) & 1;
      p.order = (s >> 4) & 1;
      p.comp.enabled = (s >> 5) & 1;
      p.comp.thresholdDb = -60.0f + static_cast<float>((s >> 8) & 63);
      for (int b = 0; b < kBands; ++b)
        p.eq.bandDb[0][b] = static_cast<float>((s >> (b % 20)) & 31) - 16.0f;
      p.eq.preampDb = (s & 1) ? std::nanf("") : 3.0f;  // garbage must be sanitised
      chain.SetParams(p);
    }
  });

  Audio a = Noise(1536, 0.3f, 5);
  bool allFinite = true;
  for (int i = 0; i < 1500; ++i)
  {
    Audio b = a;
    Run([&](float* const* p, int n) { chain.Process(p, n); }, b, 0, 1536, 1536);
    for (int n = 0; n < 1536; ++n)
      allFinite = allFinite && std::isfinite(b.ch[0][static_cast<size_t>(n)]) && std::fabs(b.ch[0][static_cast<size_t>(n)]) < 100.0f;
  }
  stop = true;
  writer.join();
  CHECK(allFinite);
}

TEST_CASE("Sanitize clamps everything into range and replaces NaN/Inf")
{
  DspParams p;
  p.enabled = 7;
  p.order = 9;
  p.eq.preampDb = 99.0f;
  p.eq.bandDb[1][4] = -99.0f;
  p.eq.bandDb[2][5] = std::nanf("");
  p.comp.ratio = 0.0f;
  p.comp.attackMs = -1.0f;
  p.comp.thresholdDb = std::numeric_limits<float>::infinity();
  p.comp.makeupDb = 1000.0f;
  Sanitize(p);
  CHECK(p.enabled == 1);
  CHECK(p.order == 0);
  CHECK(p.eq.preampDb == kEqPreampMaxDb);
  CHECK(p.eq.bandDb[1][4] == -kEqBandMaxDb);
  CHECK(p.eq.bandDb[2][5] == 0.0f);
  CHECK(p.comp.ratio == 1.0f);
  CHECK(p.comp.attackMs == 0.1f);
  CHECK(p.comp.thresholdDb == -18.0f);
  CHECK(p.comp.makeupDb == 24.0f);
}
