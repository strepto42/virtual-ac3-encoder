// test_matrix_upmix.cpp — the time-domain matrix upmix (upmix_algo=matrix): centre from the chosen band,
// width and focus, surround level / delay, engage / bypass logic, click-free parameter changes, block-size
// independence, stability, the DSP chain hook and the settings file.
#include "doctest.h"
#include "dsp/DspChain.h"
#include "dsp/DspSettings.h"
#include "dsp/MatrixUpmix.h"

#include <cmath>
#include <cstring>
#include <random>
#include <string>
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
constexpr int kRate = 48000;
constexpr int kPacket = 1536;   // the encoder's packet size

using Planes = std::vector<std::vector<float>>;

std::vector<float> Sines(std::initializer_list<double> hz, double amp, int n, int rate = kRate)
{
  std::vector<float> v(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i)
  {
    double s = 0.0;
    for (double f : hz)
      s += amp * std::sin(2.0 * kPi * f * i / rate);
    v[static_cast<size_t>(i)] = static_cast<float>(s);
  }
  return v;
}

double RmsDb(const std::vector<float>& v, size_t from = 0)
{
  double s = 0.0;
  for (size_t i = from; i < v.size(); ++i)
    s += static_cast<double>(v[i]) * static_cast<double>(v[i]);
  return 10.0 * std::log10(s / static_cast<double>(v.size() - from) + 1e-20);
}

// Runs the upmix over stereo input in encoder-sized packets; the four other planes start silent.
Planes Run(const std::vector<float>& l, const std::vector<float>& r, const UpmixParams& p, bool force, int rate = kRate,
           int packet = kPacket)
{
  MatrixUpmix u;
  u.Configure(rate);
  u.SetParams(p);
  u.Reset();
  u.SetForce(force);
  Planes o(6, std::vector<float>(l.size(), 0.0f));
  o[0] = l;
  o[1] = r;
  for (size_t pos = 0; pos < l.size(); pos += static_cast<size_t>(packet))
  {
    float* pl[6];
    for (int c = 0; c < 6; ++c)
      pl[c] = o[static_cast<size_t>(c)].data() + pos;
    u.Process(pl, static_cast<int>(std::min(static_cast<size_t>(packet), l.size() - pos)));
  }
  return o;
}
}  // namespace

TEST_CASE("upmix params: defaults, clamping, settings file round trip, old files")
{
  DspParams d;
  CHECK(d.upmix.enabled == 1);
  CHECK(d.upmix.centerLowHz == 150.0f);
  CHECK(d.upmix.centerHighHz == 5000.0f);
  CHECK(d.upmix.surroundDelayMs == 10.0f);
  d.upmix.centerWidth = 7.0f;
  d.upmix.centerFocus = -1.0f;
  d.upmix.centerLowHz = 10.0f;
  d.upmix.centerHighHz = 1e9f;
  d.upmix.surroundLevelDb = -100.0f;
  d.upmix.surroundDelayMs = std::nanf("");
  Sanitize(d);
  CHECK(d.upmix.centerWidth == 1.0f);
  CHECK(d.upmix.centerFocus == 0.0f);
  CHECK(d.upmix.centerLowHz == 60.0f);
  CHECK(d.upmix.centerHighHz == 12000.0f);
  CHECK(d.upmix.surroundLevelDb == -24.0f);
  CHECK(d.upmix.surroundDelayMs == 10.0f);

  DspParams p;
  p.upmix = {0, 0.8f, 0.25f, 200.0f, 6000.0f, 1.5f, 22.0f, 1, -9.0f, 80.0f};
  DspParams back;
  std::string err;
  REQUIRE(ParseSettings(FormatSettings(p), back, &err));
  CHECK(back.upmix.enabled == 0);
  CHECK_NEAR(back.upmix.centerWidth, 0.8, 1e-3);
  CHECK_NEAR(back.upmix.centerFocus, 0.25, 1e-3);
  CHECK_NEAR(back.upmix.centerLowHz, 200.0, 1e-2);
  CHECK_NEAR(back.upmix.centerHighHz, 6000.0, 1e-2);
  CHECK_NEAR(back.upmix.surroundLevelDb, 1.5, 1e-3);
  CHECK_NEAR(back.upmix.surroundDelayMs, 22.0, 1e-3);
  CHECK(back.upmix.lfeEnabled == 1);
  CHECK_NEAR(back.upmix.lfeLevelDb, -9.0, 1e-3);
  CHECK_NEAR(back.upmix.lfeCutHz, 80.0, 1e-2);

  DspParams old;   // a file from before the upmix section existed
  REQUIRE(ParseSettings("{\"enabled\": true, \"compressor\": {\"enabled\": true}}", old, &err));
  CHECK(old.upmix.enabled == 1);
  CHECK(old.upmix.centerLowHz == 150.0f);
  CHECK(old.comp.enabled == 1);
}

TEST_CASE("matrix upmix: dialogue panned to the middle goes to the centre channel and leaves L/R")
{
  const int n = kRate * 2;
  const auto s = Sines({400, 900, 2100}, 0.1, n);
  std::vector<float> l(s.size()), r(s.size());
  for (size_t i = 0; i < s.size(); ++i)
    l[i] = r[i] = 0.707f * s[i];                    // a phantom centre: -3 dB in each channel
  const auto o = Run(l, r, UpmixParams{}, true);
  const size_t from = static_cast<size_t>(n) / 2;
  CHECK_NEAR(RmsDb(o[2], from), RmsDb(s, from), 0.8);            // the centre carries it at full level
  CHECK(RmsDb(o[0], from) < RmsDb(l, from) - 30.0);              // and the front channels are (nearly) empty
  CHECK(RmsDb(o[1], from) < RmsDb(r, from) - 30.0);
  CHECK(RmsDb(o[4], from) < -60.0);                              // no difference signal: no surround
  CHECK(RmsDb(o[5], from) < -60.0);
  CHECK(RmsDb(o[3], from) < -100.0);                             // the LFE is not produced
}

TEST_CASE("matrix upmix: bass and treble stay in L/R, the centre is taken only from its band")
{
  const int n = kRate * 2;
  const size_t from = static_cast<size_t>(n) / 2;
  const auto s = Sines({50, 14000}, 0.1, n);                     // outside 150 Hz .. 5 kHz
  const auto o = Run(s, s, UpmixParams{}, true);
  CHECK(RmsDb(o[2], from) < RmsDb(s, from) - 30.0);
  CHECK_NEAR(RmsDb(o[0], from), RmsDb(s, from), 0.6);            // L/R keep them (level; only the phase moves)

  // the band is a setting: with the low edge at 400 Hz a 250 Hz tone is no longer taken
  const auto t = Sines({250}, 0.1, n);
  UpmixParams low150, low400;
  low400.centerLowHz = 400.0f;
  const auto a = Run(t, t, low150, true);
  const auto b = Run(t, t, low400, true);
  CHECK(RmsDb(a[2], from) > RmsDb(t, from) - 2.0);
  CHECK(RmsDb(b[2], from) < RmsDb(t, from) - 12.0);   // 250 Hz is 0.64 of 400 Hz: the 4th-order slope leaves ~-15 dB
}

TEST_CASE("matrix upmix: hard-panned, uncorrelated and out-of-phase material stays out of the centre")
{
  const int n = kRate * 2;
  const size_t from = static_cast<size_t>(n) / 2;
  const auto s = Sines({400, 900, 2100}, 0.1, n);
  const std::vector<float> zero(s.size(), 0.0f);
  const auto left = Run(s, zero, UpmixParams{}, true);
  CHECK(RmsDb(left[2], from) < -80.0);
  CHECK_NEAR(RmsDb(left[0], from), RmsDb(s, from), 0.4);

  std::mt19937 g(1);
  std::uniform_real_distribution<float> u(-0.2f, 0.2f);
  std::vector<float> nl(s.size()), nr(s.size());
  for (auto& x : nl) x = u(g);
  for (auto& x : nr) x = u(g);
  UpmixParams loose;
  loose.centerFocus = 0.0f;
  CHECK(RmsDb(Run(nl, nr, UpmixParams{}, true)[2], from) < RmsDb(nl, from) - 25.0);
  CHECK(RmsDb(Run(nl, nr, loose, true)[2], from) < RmsDb(nl, from) - 20.0);

  std::vector<float> inv(s.size());
  for (size_t i = 0; i < s.size(); ++i) inv[i] = -s[i];
  UpmixParams surr;
  surr.surroundLevelDb = 0.0f;
  const auto o = Run(s, inv, surr, true);
  CHECK(RmsDb(o[2], from) < -60.0);
  CHECK_NEAR(RmsDb(o[4], from), RmsDb(s, from) + 3.0, 0.6);      // (L-R)/sqrt2: +3 dB for an anti-phase pair
  double mx = 0.0;
  for (size_t i = 0; i < s.size(); ++i)
    mx = std::max(mx, static_cast<double>(std::fabs(o[4][i] + o[5][i])));
  CHECK(mx < 1e-6);                                              // opposite polarity in the two surrounds
}

TEST_CASE("matrix upmix: width and focus decide what reaches the centre")
{
  const int n = kRate * 2;
  const size_t from = static_cast<size_t>(n) / 2;
  const auto s = Sines({400, 900, 2100}, 0.1, n);
  std::vector<float> l(s.size()), r(s.size());
  for (size_t i = 0; i < s.size(); ++i)
  {
    l[i] = 0.9f * s[i];                                          // a source a little left of the middle
    r[i] = 0.45f * s[i];
  }
  UpmixParams narrow, wide;
  narrow.centerWidth = 0.1f;
  wide.centerWidth = 1.0f;
  CHECK(RmsDb(Run(l, r, wide, true)[2], from) > RmsDb(Run(l, r, narrow, true)[2], from) + 10.0);

  std::mt19937 g(9);
  std::uniform_real_distribution<float> u(-0.1f, 0.1f);
  std::vector<float> dl(s.size()), dr(s.size());
  for (size_t i = 0; i < s.size(); ++i)                           // centre content plus independent diffuse parts
  {
    dl[i] = 0.5f * s[i] + u(g) * 1.0f;
    dr[i] = 0.5f * s[i] + u(g) * 1.0f;
  }
  UpmixParams loose, strict;
  loose.centerFocus = 0.0f;
  strict.centerFocus = 0.95f;
  CHECK(RmsDb(Run(dl, dr, loose, true)[2], from) > RmsDb(Run(dl, dr, strict, true)[2], from) + 6.0);
}

TEST_CASE("matrix upmix: surround delay and level")
{
  for (float ms : {0.0f, 10.0f, 25.0f})
  {
    const int n = kRate;
    std::vector<float> l(static_cast<size_t>(n), 0.0f), r(static_cast<size_t>(n), 0.0f);
    l[4800] = 1.0f;
    r[4800] = -1.0f;
    UpmixParams p;
    p.surroundLevelDb = 0.0f;
    p.surroundDelayMs = ms;
    const auto o = Run(l, r, p, true);
    size_t peak = 0;
    for (size_t i = 0; i < o[4].size(); ++i)
      if (std::fabs(o[4][i]) > std::fabs(o[4][peak]))
        peak = i;
    INFO("delay " << ms << " ms");
    CHECK_NEAR(static_cast<double>(peak), 4800.0 + static_cast<double>(ms) * 48.0, 1.5);
  }
  const auto s = Sines({700}, 0.1, kRate);
  std::vector<float> inv(s.size());
  for (size_t i = 0; i < s.size(); ++i) inv[i] = -s[i];
  UpmixParams a, b;
  a.surroundLevelDb = 0.0f;
  b.surroundLevelDb = -9.0f;
  CHECK_NEAR(RmsDb(Run(s, inv, a, true)[4], 24000) - RmsDb(Run(s, inv, b, true)[4], 24000), 9.0, 0.3);
}

TEST_CASE("matrix upmix: engages on stereo material, leaves real 5.1 alone, bypass is exact")
{
  const int n = kRate * 2;
  const auto s = Sines({400, 900, 2100}, 0.1, n);

  // stereo only: silent centre for the first 200 ms, then the upmix is on
  {
    const auto o = Run(s, s, UpmixParams{}, false);
    CHECK(RmsDb(std::vector<float>(o[2].begin(), o[2].begin() + 7000), 0) < -100.0);
    CHECK(RmsDb(o[2], static_cast<size_t>(n) / 2) > RmsDb(s, 0) - 3.0);
  }
  // 5.1 input (signal on centre and surrounds): untouched, bit for bit
  {
    MatrixUpmix u;
    u.Configure(kRate);
    Planes in(6, std::vector<float>(static_cast<size_t>(n)));
    for (int c = 0; c < 6; ++c)
      in[static_cast<size_t>(c)] = Sines({300.0 + 100.0 * c}, 0.1, n);
    Planes o = in;
    for (int pos = 0; pos < n; pos += kPacket)
    {
      float* pl[6];
      for (int c = 0; c < 6; ++c) pl[c] = o[static_cast<size_t>(c)].data() + pos;
      u.Process(pl, std::min(kPacket, n - pos));
    }
    for (int c = 0; c < 6; ++c)
      CHECK(std::memcmp(o[static_cast<size_t>(c)].data(), in[static_cast<size_t>(c)].data(), static_cast<size_t>(n) * sizeof(float)) == 0);
  }
  // stereo first, real 5.1 later: the upmix steps aside within 20 ms and the 5.1 comes out untouched
  {
    MatrixUpmix u;
    u.Configure(kRate);
    Planes o(6, std::vector<float>(static_cast<size_t>(n), 0.0f));
    o[0] = s;
    o[1] = s;
    const auto surround = Sines({1234}, 0.05, n);
    for (size_t i = static_cast<size_t>(n) / 2; i < static_cast<size_t>(n); ++i)
      o[4][i] = surround[i];                                      // from 1 s on the surround plane carries signal
    const Planes in = o;
    for (int pos = 0; pos < n; pos += kPacket)
    {
      float* pl[6];
      for (int c = 0; c < 6; ++c) pl[c] = o[static_cast<size_t>(c)].data() + pos;
      u.Process(pl, std::min(kPacket, n - pos));
    }
    const size_t tail = static_cast<size_t>(n) - 6000;
    for (int c = 0; c < 6; ++c)
      CHECK(std::memcmp(o[static_cast<size_t>(c)].data() + tail, in[static_cast<size_t>(c)].data() + tail, 6000 * sizeof(float)) == 0);
  }
  // disabled: exact bypass even when forced
  {
    UpmixParams off;
    off.enabled = 0;
    const auto o = Run(s, s, off, true);
    CHECK(std::memcmp(o[0].data(), s.data(), s.size() * sizeof(float)) == 0);
    CHECK(RmsDb(o[2], 0) < -100.0);
  }
}

TEST_CASE("matrix upmix: the result does not depend on how the audio is split into blocks")
{
  std::mt19937 rng(5);
  std::uniform_real_distribution<float> u(-0.3f, 0.3f);
  const int n = kRate;
  const auto tone = Sines({700, 1500}, 0.1, n);
  std::vector<float> l(static_cast<size_t>(n)), r(static_cast<size_t>(n));
  for (size_t i = 0; i < l.size(); ++i)
  {
    l[i] = tone[i] + 0.3f * u(rng);
    r[i] = tone[i] + 0.3f * u(rng);
  }
  UpmixParams a, b;
  b.centerLowHz = 300.0f;
  b.centerHighHz = 3000.0f;
  b.surroundDelayMs = 30.0f;
  b.surroundLevelDb = 3.0f;
  const int switchAt = 20000;
  auto run = [&](int packet) {
    MatrixUpmix up;
    up.Configure(kRate);
    up.SetParams(a);
    up.Reset();
    up.SetForce(true);
    Planes o(6, std::vector<float>(static_cast<size_t>(n), 0.0f));
    o[0] = l;
    o[1] = r;
    int pos = 0;
    while (pos < n)
    {
      if (pos == switchAt)
        up.SetParams(b);
      int len = std::min(packet, n - pos);
      if (pos < switchAt)
        len = std::min(len, switchAt - pos);                       // the change lands on the same sample in every run
      float* pl[6];
      for (int c = 0; c < 6; ++c) pl[c] = o[static_cast<size_t>(c)].data() + pos;
      up.Process(pl, len);
      pos += len;
    }
    return o;
  };
  const Planes whole = run(n), odd = run(1), mid = run(333), packets = run(kPacket);
  for (int c = 0; c < 6; ++c)
  {
    const size_t bytes = static_cast<size_t>(n) * sizeof(float);
    CHECK(std::memcmp(whole[static_cast<size_t>(c)].data(), odd[static_cast<size_t>(c)].data(), bytes) == 0);
    CHECK(std::memcmp(whole[static_cast<size_t>(c)].data(), mid[static_cast<size_t>(c)].data(), bytes) == 0);
    CHECK(std::memcmp(whole[static_cast<size_t>(c)].data(), packets[static_cast<size_t>(c)].data(), bytes) == 0);
  }
}

TEST_CASE("matrix upmix: moving every setting at once never clicks")
{
  const int n = kRate * 3;
  const auto s = Sines({1000}, 0.3, n);
  std::vector<float> inv(s.size());
  for (size_t i = 0; i < s.size(); ++i) inv[i] = 0.4f * s[i] - 0.2f * static_cast<float>(std::sin(2.0 * kPi * 1000.0 * static_cast<double>(i) / kRate + 1.0));
  UpmixParams a, b;
  b.centerLowHz = 400.0f;
  b.centerHighHz = 2500.0f;
  b.surroundDelayMs = 40.0f;
  b.surroundLevelDb = 6.0f;
  b.centerWidth = 1.0f;
  b.centerFocus = 0.0f;
  MatrixUpmix up;
  up.Configure(kRate);
  up.SetParams(a);
  up.Reset();
  up.SetForce(true);
  Planes o(6, std::vector<float>(static_cast<size_t>(n), 0.0f));
  o[0] = s;
  o[1] = inv;
  for (int pos = 0; pos < n; pos += 300)                            // a new target every 6 ms, flipping between two extremes
  {
    up.SetParams(((pos / 300) % 2 == 0) ? a : b);
    float* pl[6];
    for (int c = 0; c < 6; ++c) pl[c] = o[static_cast<size_t>(c)].data() + pos;
    up.Process(pl, std::min(300, n - pos));
  }
  for (int c : {0, 1, 2, 4, 5})
  {
    double maxAbs = 0.0, maxStep = 0.0;
    const auto& v = o[static_cast<size_t>(c)];
    for (size_t i = 1; i < v.size(); ++i)
    {
      REQUIRE(std::isfinite(v[i]));
      maxAbs = std::max(maxAbs, static_cast<double>(std::fabs(v[i])));
      maxStep = std::max(maxStep, static_cast<double>(std::fabs(v[i] - v[i - 1])));
    }
    INFO("plane " << c << " maxAbs " << maxAbs << " maxStep " << maxStep);
    CHECK(maxStep <= maxAbs * (2.0 * kPi * 1000.0 / kRate * 1.3 + 0.12) + 1e-3);
  }
}

TEST_CASE("matrix upmix: random material and extreme settings stay finite and bounded at every sample rate")
{
  std::mt19937 rng(77);
  std::uniform_real_distribution<float> u(-1.0f, 1.0f), logLow(std::log(60.0f), std::log(400.0f)),
      logHigh(std::log(2000.0f), std::log(12000.0f));
  for (int rate : {32000, 44100, 48000, 96000})
  {
    MatrixUpmix up;
    up.Configure(rate);
    up.SetForce(true);
    const int blocks = 40, len = rate / 20;
    for (int k = 0; k < blocks; ++k)
    {
      UpmixParams p;
      p.enabled = 1;
      p.centerWidth = (u(rng) + 1.0f) / 2.0f;
      p.centerFocus = (u(rng) + 1.0f) / 2.0f;
      p.centerLowHz = std::exp(logLow(rng));
      p.centerHighHz = std::exp(logHigh(rng));
      p.surroundLevelDb = 6.0f * u(rng) - 9.0f;
      p.surroundDelayMs = 20.0f * (u(rng) + 1.0f);
      up.SetParams(p);
      Planes o(6, std::vector<float>(static_cast<size_t>(len), 0.0f));
      for (int i = 0; i < len; ++i)
      {
        const float common = 0.4f * u(rng);
        o[0][static_cast<size_t>(i)] = common + 0.3f * u(rng);
        o[1][static_cast<size_t>(i)] = ((k % 3 == 0) ? common : -common) + 0.3f * u(rng);
      }
      float* pl[6];
      for (int c = 0; c < 6; ++c) pl[c] = o[static_cast<size_t>(c)].data();
      up.Process(pl, len);
      for (const auto& v : o)
        for (float x : v)
        {
          REQUIRE(std::isfinite(x));
          REQUIRE(std::fabs(x) < 20.0f);
        }
    }
  }
}

TEST_CASE("dsp chain: the matrix upmix runs in front of the EQ whatever the master switch says; stereo chains ignore it")
{
  const int n = kRate;
  const auto s = Sines({400, 900, 2100}, 0.1, n);
  std::vector<float> m(s.size());
  for (size_t i = 0; i < s.size(); ++i) m[i] = 0.707f * s[i];
  DspChain chain;
  chain.Configure(6, kRate);
  chain.ConfigureUpmix(true, true);
  CHECK(chain.UpmixActive());
  DspParams p;                         // master switch off (the default)
  CHECK(p.enabled == 0);
  chain.SetParams(p);
  chain.SetSpectrumTap(true);
  Planes o(6, std::vector<float>(static_cast<size_t>(n), 0.0f));
  o[0] = m;
  o[1] = m;
  float* pl[6];
  for (int c = 0; c < 6; ++c) pl[c] = o[static_cast<size_t>(c)].data();
  chain.Process(pl, n);
  CHECK(RmsDb(o[2], 24000) > RmsDb(s, 24000) - 1.0);              // centre filled although the DSP is "off"
  std::vector<float> tapped(static_cast<size_t>(6 * 2048));
  REQUIRE(chain.TapIn().Latest(2048, tapped.data()));
  double centreEnergy = 0.0;
  for (size_t i = 0; i < 2048; ++i)
    centreEnergy += static_cast<double>(tapped[2 * 2048 + i]) * tapped[2 * 2048 + i];
  CHECK(centreEnergy > 1.0);                                      // the spectrum tap in front of the EQ sees the upmixed centre

  DspChain stereo;
  stereo.Configure(2, kRate);
  stereo.ConfigureUpmix(true, true);
  CHECK_FALSE(stereo.UpmixActive());                              // the PCM path has no 5.1 planes: nothing to do
}

#include "dsp/ConfEdit.h"

TEST_CASE("conf edit: reads and rewrites one key and leaves the rest of the file alone")
{
  const std::string conf =
      "# virtual-ac3-encoder\r\n"
      "output_mode = ac3\r\n"
      "#upmix_algo=matrix\r\n"
      "upmix=surround\r\n"
      "  upmix_algo = surround  \r\n"
      "headroom_db=-6\r\n";
  CHECK(ConfGet(conf, "upmix_algo") == "surround");          // the commented-out line does not count
  CHECK(ConfGet(conf, "upmix") == "surround");
  CHECK(ConfGet(conf, "missing", "dflt") == "dflt");

  const std::string changed = ConfSet(conf, "upmix_algo", "matrix");
  CHECK(ConfGet(changed, "upmix_algo") == "matrix");
  CHECK(changed.find("#upmix_algo=matrix\r\n") != std::string::npos);   // the comment is untouched
  CHECK(changed.find("headroom_db=-6\r\n") != std::string::npos);
  CHECK(changed.find("\r\n") != std::string::npos);
  size_t bare = 0;                                                      // no line ending was lost or turned into LF
  for (size_t i = 0; i < changed.size(); ++i)
    if (changed[i] == '\n' && (i == 0 || changed[i - 1] != '\r'))
      ++bare;
  CHECK(bare == 0u);

  const std::string added = ConfSet("upmix=off\n", "upmix_algo", "matrix");   // absent key: appended
  CHECK(added == "upmix=off\nupmix_algo=matrix\n");
  CHECK(ConfSet("upmix=off", "upmix_algo", "matrix") == "upmix=off\nupmix_algo=matrix\n");   // no final newline yet
  CHECK(ConfSet("", "k", "v") == "k=v\n");

  const std::string twice = ConfSet("a=1\na=2\n", "a", "9");            // the last one wins in the parser: change all
  CHECK(twice == "a=9\na=9\n");
  CHECK(ConfSet(ConfSet(conf, "upmix_algo", "matrix"), "upmix_algo", "surround") == ConfSet(conf, "upmix_algo", "surround"));
}

TEST_CASE("matrix upmix: optional LFE = low-passed mono sum; off leaves the plane untouched")
{
  const int n = kRate * 2;
  const size_t from = static_cast<size_t>(n) / 2;
  const auto bass = Sines({50}, 0.2, n);
  const auto mixed = Sines({50, 1000, 6000}, 0.1, n);
  UpmixParams off, on;
  on.lfeEnabled = 1;
  on.lfeLevelDb = 0.0f;
  on.lfeCutHz = 100.0f;

  const auto a = Run(bass, bass, off, true);
  CHECK(RmsDb(a[3], from) < -100.0);                               // default: no LFE at all

  const auto b = Run(bass, bass, on, true);
  CHECK_NEAR(RmsDb(b[3], from), RmsDb(bass, from) - 0.26, 0.4);    // 50 Hz through a 100 Hz LR4 low-pass: -0.26 dB
  const auto c = Run(mixed, mixed, on, true);
  CHECK(RmsDb(c[3], from) < RmsDb(Sines({50}, 0.1, n), from) + 0.6);   // only the bass comes through ...
  CHECK(RmsDb(c[3], from) > RmsDb(Sines({50}, 0.1, n), from) - 0.8);   // ... and nothing of 1 kHz / 6 kHz is added

  UpmixParams quiet = on;
  quiet.lfeLevelDb = -12.0f;
  CHECK_NEAR(RmsDb(b[3], from) - RmsDb(Run(bass, bass, quiet, true)[3], from), 12.0, 0.3);

  UpmixParams wide = on;
  wide.lfeCutHz = 200.0f;
  const auto hi = Sines({150}, 0.2, n);
  CHECK(RmsDb(Run(hi, hi, wide, true)[3], from) > RmsDb(Run(hi, hi, on, true)[3], from) + 6.0);   // the corner is a setting

  const auto o5 = Run(bass, bass, [&] { UpmixParams q = on; q.enabled = 0; return q; }(), true);
  CHECK(RmsDb(o5[3], from) < -100.0);                              // upmix bypassed: LFE untouched too

  // switching it on and off while audio runs: fades, never clicks
  MatrixUpmix up;
  up.Configure(kRate);
  up.SetForce(true);
  Planes o(6, std::vector<float>(static_cast<size_t>(n), 0.0f));
  o[0] = bass;
  o[1] = bass;
  for (int pos = 0; pos < n; pos += 14400)                          // 300 ms on, 300 ms off: long enough for the ~130 ms fade
  {
    up.SetParams(((pos / 14400) % 2 == 0) ? on : off);
    float* pl[6];
    for (int ch = 0; ch < 6; ++ch) pl[ch] = o[static_cast<size_t>(ch)].data() + pos;
    up.Process(pl, std::min(14400, n - pos));
  }
  double maxAbs = 0.0, maxStep = 0.0;
  for (size_t i = 1; i < o[3].size(); ++i)
  {
    maxAbs = std::max(maxAbs, static_cast<double>(std::fabs(o[3][i])));
    maxStep = std::max(maxStep, static_cast<double>(std::fabs(o[3][i] - o[3][i - 1])));
  }
  CHECK(maxAbs > 0.1);
  CHECK(maxStep <= maxAbs * (2.0 * kPi * 50.0 / kRate * 1.3 + 0.07) + 1e-3);   // one 0.5 dB step = 6 %
}

TEST_CASE("upmix presets: only the upmix section, round trip, tolerant of complete settings files")
{
  UpmixParams u;
  u.centerWidth = 0.9f;
  u.centerFocus = 0.3f;
  u.centerLowHz = 100.0f;
  u.centerHighHz = 7000.0f;
  u.surroundLevelDb = -9.0f;
  u.surroundDelayMs = 6.0f;
  u.lfeEnabled = 1;
  const std::string text = FormatUpmixPreset(u);
  CHECK(text.find("\"eq\"") == std::string::npos);                 // no equalizer in an upmix preset
  CHECK(text.find("\"upmix\"") != std::string::npos);
  UpmixParams back;
  std::string err;
  REQUIRE(ParseUpmixPreset(text, back, &err));
  CHECK_NEAR(back.centerWidth, 0.9, 1e-3);
  CHECK_NEAR(back.centerHighHz, 7000.0, 1e-2);
  CHECK(back.lfeEnabled == 1);

  DspParams whole;
  whole.upmix.centerFocus = 0.77f;
  UpmixParams fromWhole;
  REQUIRE(ParseUpmixPreset(FormatSettings(whole), fromWhole, &err));
  CHECK_NEAR(fromWhole.centerFocus, 0.77, 1e-3);
  UpmixParams bad;
  CHECK_FALSE(ParseUpmixPreset("{ nope", bad, &err));
}
