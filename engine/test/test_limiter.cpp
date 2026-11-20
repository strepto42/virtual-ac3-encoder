// test_limiter.cpp — LookaheadLimiter: never exceeds the ceiling, transparent below it,
// linked across channels, independent of block size, recovers after a loud burst.
#include "doctest.h"
#include "Limiter.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {
constexpr int kRate = 48000;
constexpr int kCh = 6;

using Planes = std::array<std::vector<float>, kCh>;

Planes MakeSignal(int frames, float amp, float freqHz)
{
  Planes p;
  for (int c = 0; c < kCh; ++c)
  {
    p[c].resize(static_cast<size_t>(frames));
    for (int n = 0; n < frames; ++n)
      p[c][static_cast<size_t>(n)] =
          amp * std::sin(2.0f * 3.14159265f * freqHz * static_cast<float>(n + c * 7) / kRate);
  }
  return p;
}

// Run the limiter over `p` in chunks of `chunk` frames (in place).
void Run(LookaheadLimiter& lim, Planes& p, int chunk)
{
  const int total = static_cast<int>(p[0].size());
  for (int pos = 0; pos < total; pos += chunk)
  {
    const int n = std::min(chunk, total - pos);
    float* ptrs[kCh];
    for (int c = 0; c < kCh; ++c)
      ptrs[c] = p[c].data() + pos;
    lim.Process(ptrs, n);
  }
}
} // namespace

TEST_CASE("limiter is transparent (just delayed) below the ceiling")
{
  LookaheadLimiter lim;
  lim.Configure(kCh, kRate, -1.0f);
  const int L = lim.LatencySamples();
  const Planes in = MakeSignal(4800, 0.5f, 440.0f); // 0.5 = -6 dBFS < -1 dBFS ceiling
  Planes out = in;
  Run(lim, out, 512);
  for (int c = 0; c < kCh; ++c)
    for (int n = L; n < 4800; ++n)
      REQUIRE(out[c][static_cast<size_t>(n)] == doctest::Approx(in[c][static_cast<size_t>(n - L)]).epsilon(1e-6));
  CHECK(lim.TakeMaxReductionDb() == doctest::Approx(0.0f));
}

TEST_CASE("limiter never exceeds the ceiling, even on a +6 dB overload")
{
  LookaheadLimiter lim;
  lim.Configure(kCh, kRate, -1.0f);
  const float ceiling = std::pow(10.0f, -1.0f / 20.0f);
  Planes p = MakeSignal(48000, 2.0f, 997.0f); // +6 dBFS, sustained
  // Add a few hard single-sample spikes on random channels.
  p[3][10000] = 4.0f; p[1][20000] = -5.0f; p[5][30000] = 3.0f; p[0][1] = 8.0f;
  Run(lim, p, 480);
  float worst = 0.0f;
  for (int c = 0; c < kCh; ++c)
    for (float v : p[c])
      worst = std::max(worst, std::fabs(v));
  CHECK(worst <= ceiling + 1e-5f);
  CHECK(lim.TakeMaxReductionDb() > 5.0f); // it really had to work
}

TEST_CASE("limiter is linked: a peak on one channel scales all channels equally")
{
  LookaheadLimiter lim;
  lim.Configure(kCh, kRate, -1.0f);
  const int frames = 9600;
  Planes p;
  for (int c = 0; c < kCh; ++c)
    p[c].assign(static_cast<size_t>(frames), 0.3f); // steady DC-like level on every channel
  for (int n = 4000; n < 4400; ++n)
    p[0][static_cast<size_t>(n)] = 1.8f;             // only FL is overloaded
  Run(lim, p, 256);
  // While FL is clamped (inside the burst, after the latency), the quiet channels were reduced too.
  const size_t probe = 4200 + static_cast<size_t>(lim.LatencySamples());
  const float ratioFL = p[0][probe] / 1.8f;
  const float ratioC = p[2][probe] / 0.3f;
  CHECK(ratioFL == doctest::Approx(ratioC).epsilon(0.01));
  CHECK(ratioC < 0.7f);
}

TEST_CASE("limiter output does not depend on the block size")
{
  LookaheadLimiter a, b;
  a.Configure(kCh, kRate, -1.0f);
  b.Configure(kCh, kRate, -1.0f);
  Planes pa = MakeSignal(20000, 1.7f, 311.0f);
  Planes pb = pa;
  Run(a, pa, 1);
  Run(b, pb, 1536);
  for (int c = 0; c < kCh; ++c)
    for (size_t n = 0; n < pa[c].size(); ++n)
      REQUIRE(pa[c][n] == doctest::Approx(pb[c][n]).epsilon(1e-6));
}

TEST_CASE("limiter releases back to unity gain after a burst")
{
  LookaheadLimiter lim;
  lim.Configure(kCh, kRate, -1.0f, 2.0f, 100.0f);
  const int frames = kRate * 3;
  Planes p;
  for (int c = 0; c < kCh; ++c)
    p[c].assign(static_cast<size_t>(frames), 0.2f);
  for (int n = 1000; n < 1500; ++n)
    p[2][static_cast<size_t>(n)] = 3.0f;
  Run(lim, p, 480);
  // 2 s after the burst the level of an untouched channel must be back to ~0.2.
  CHECK(p[4][static_cast<size_t>(frames - 100)] == doctest::Approx(0.2f).epsilon(0.001));
}
