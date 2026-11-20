// test_eq_shapes.cpp — per-band frequency, width and shape (bell, shelves, notch, high-pass, low-pass):
// filter responses, agreement between the drawn curve and the real filter, click-free parameter changes,
// block-size independence, stability, the settings file, and the Studio model's shape editing.
#include "doctest.h"
#include "dsp/DspSettings.h"
#include "dsp/GraphicEq.h"
#include "dsp/StudioModel.h"

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

constexpr int kRate = 48000;
constexpr double kPi = 3.14159265358979323846;

// Gain (dB) the real filter applies to a steady sine: jump to the target, run, compare RMS.
double MeasureDb(const EqParams& p, double hz, int rate = kRate, double seconds = 1.2)
{
  GraphicEq eq;
  eq.Configure(1, rate);
  eq.SetParams(p);
  eq.Reset();
  const int n = static_cast<int>(seconds * rate);
  std::vector<float> x(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i)
    x[static_cast<size_t>(i)] = static_cast<float>(0.1 * std::sin(2.0 * kPi * hz * i / rate));
  const std::vector<float> in = x;
  float* planes[1] = {x.data()};
  eq.Process(planes, n);
  double si = 0, so = 0;
  for (int i = n / 2; i < n; ++i)
  {
    si += static_cast<double>(in[static_cast<size_t>(i)]) * in[static_cast<size_t>(i)];
    so += static_cast<double>(x[static_cast<size_t>(i)]) * x[static_cast<size_t>(i)];
  }
  return 10.0 * std::log10(so / si);
}

EqParams OneBand(int band, int type, float hz, float widthOct, float gainDb)
{
  EqParams p;
  p.shape[band].type = type;
  p.shape[band].freqHz = hz;
  p.shape[band].widthOct = widthOct;
  for (int c = 0; c < kMaxChannels; ++c)
    p.bandDb[c][band] = gainDb;
  return p;
}

double Resp(const EqParams& p, double hz) { return GraphicEq::ResponseDb(p.bandDb[0], p.shape, hz, kRate); }

}  // namespace

TEST_CASE("shapes: defaults are the 30 bell bands, Sanitize repairs bad shapes")
{
  EqParams p;
  for (int b = 0; b < kBands; ++b)
  {
    CHECK(p.shape[b].freqHz == kBandHz[b]);
    CHECK(p.shape[b].type == kBandBell);
    CHECK_NEAR(p.shape[b].widthOct, kBandWidthOct, 1e-6);
  }
  DspParams d;
  d.eq.shape[3].freqHz = 5.0f;           // below the range
  d.eq.shape[4].freqHz = 1e9f;           // above
  d.eq.shape[5].freqHz = std::nanf("");  // not a number
  d.eq.shape[6].widthOct = 0.0f;
  d.eq.shape[7].widthOct = 100.0f;
  d.eq.shape[8].type = 99;
  d.eq.shape[9].type = kBandHighPass;
  d.eq.shape[9].widthOct = std::nanf("");
  Sanitize(d);
  CHECK(d.eq.shape[3].freqHz == kBandFreqMinHz);
  CHECK(d.eq.shape[4].freqHz == kBandFreqMaxHz);
  CHECK(d.eq.shape[5].freqHz == kBandHz[5]);
  CHECK(d.eq.shape[6].widthOct == kBandWidthMinOct);
  CHECK(d.eq.shape[7].widthOct == kBandWidthMaxOct);
  CHECK(d.eq.shape[8].type == kBandBell);
  CHECK(d.eq.shape[9].widthOct == DefaultBandWidthOct(kBandHighPass));
}

TEST_CASE("shapes: a moved bell peaks where it was moved, width changes how far it reaches")
{
  EqParams p = OneBand(15, kBandBell, 2000.0f, 0.5f, 6.0f);   // band 15 normally sits at 712 Hz
  CHECK_NEAR(Resp(p, 2000.0), 6.0, 0.02);
  CHECK(std::fabs(Resp(p, 712.0)) < 1.5);
  EqParams narrow = OneBand(15, kBandBell, 2000.0f, 0.1f, 6.0f);
  EqParams wide = OneBand(15, kBandBell, 2000.0f, 3.0f, 6.0f);
  CHECK(Resp(narrow, 4000.0) < Resp(p, 4000.0));
  CHECK(Resp(p, 4000.0) < Resp(wide, 4000.0));
  CHECK_NEAR(Resp(narrow, 2000.0), 6.0, 0.02);
  CHECK_NEAR(Resp(wide, 2000.0), 6.0, 0.02);
}

TEST_CASE("shapes: low and high shelf")
{
  EqParams lo = OneBand(10, kBandLowShelf, 1000.0f, 1.9f, 9.0f);
  CHECK_NEAR(Resp(lo, 30.0), 9.0, 0.4);
  CHECK_NEAR(Resp(lo, 1000.0), 4.5, 0.6);   // half the gain at the corner
  CHECK(std::fabs(Resp(lo, 16000.0)) < 0.4);
  EqParams hi = OneBand(10, kBandHighShelf, 1000.0f, 1.9f, -8.0f);
  CHECK_NEAR(Resp(hi, 18000.0), -8.0, 0.5);
  CHECK_NEAR(Resp(hi, 1000.0), -4.0, 0.6);
  CHECK(std::fabs(Resp(hi, 40.0)) < 0.4);
  EqParams flat = OneBand(10, kBandLowShelf, 1000.0f, 1.9f, 0.0f);
  CHECK(Resp(flat, 100.0) == 0.0);
}

TEST_CASE("shapes: notch, high-pass and low-pass act without any gain and ignore the gain value")
{
  EqParams notch = OneBand(12, kBandNotch, 1000.0f, 0.34f, 0.0f);
  CHECK(Resp(notch, 1000.0) < -30.0);
  CHECK(std::fabs(Resp(notch, 100.0)) < 0.5);
  CHECK(std::fabs(Resp(notch, 10000.0)) < 0.5);

  EqParams hp = OneBand(5, kBandHighPass, 100.0f, 1.9f, 0.0f);
  CHECK_NEAR(Resp(hp, 100.0), -3.01, 0.1);
  CHECK_NEAR(Resp(hp, 25.0), -24.0, 1.5);      // 12 dB per octave, two octaves below
  CHECK(std::fabs(Resp(hp, 10000.0)) < 0.05);
  EqParams hpGain = OneBand(5, kBandHighPass, 100.0f, 1.9f, 12.0f);
  for (double f : {30.0, 100.0, 400.0, 5000.0})
    CHECK(Resp(hp, f) == Resp(hpGain, f));

  EqParams lp = OneBand(25, kBandLowPass, 5000.0f, 1.9f, -12.0f);
  CHECK_NEAR(Resp(lp, 5000.0), -3.01, 0.1);
  CHECK(std::fabs(Resp(lp, 100.0)) < 0.05);
  CHECK(Resp(lp, 20000.0) < -20.0);
}

TEST_CASE("shapes: the curve the UI draws matches what the real filter does, for every shape")
{
  struct Case { int type; float hz; float width; float gain; };
  const Case cases[] = {{kBandBell, 800.0f, 0.5f, 7.0f},      {kBandBell, 3000.0f, 2.0f, -9.0f},
                        {kBandLowShelf, 200.0f, 1.9f, 8.0f},  {kBandHighShelf, 6000.0f, 1.2f, -7.0f},
                        {kBandNotch, 1500.0f, 0.4f, 0.0f},    {kBandHighPass, 80.0f, 1.9f, 0.0f},
                        {kBandLowPass, 9000.0f, 0.9f, 0.0f}};
  for (const Case& c : cases)
  {
    const EqParams p = OneBand(14, c.type, c.hz, c.width, c.gain);
    for (double f : {120.0, 400.0, 900.0, 2500.0, 7000.0, 15000.0})
      CHECK_NEAR(MeasureDb(p, f), Resp(p, f), 0.03);
  }
  // several shaped bands at once
  EqParams multi;
  multi.shape[3] = {60.0f, 1.9f, kBandHighPass};
  multi.shape[12] = {1000.0f, 1.0f, kBandLowShelf};
  multi.bandDb[0][12] = 5.0f;
  multi.shape[20] = {2500.0f, 0.3f, kBandBell};
  multi.bandDb[0][20] = -6.0f;
  multi.shape[26] = {9000.0f, 1.9f, kBandLowPass};
  for (double f : {70.0, 500.0, 2500.0, 8000.0})
    CHECK_NEAR(MeasureDb(multi, f), Resp(multi, f), 0.05);
}

TEST_CASE("shapes: gain bands at 0 dB with moved frequency/width are still a bit-exact pass-through")
{
  EqParams p;
  p.shape[4] = {123.0f, 2.5f, kBandLowShelf};
  p.shape[9] = {5000.0f, 0.1f, kBandHighShelf};
  p.shape[17] = {777.0f, 1.0f, kBandBell};
  GraphicEq eq;
  eq.Configure(2, kRate);
  eq.SetParams(p);
  eq.Reset();
  std::mt19937 rng(3);
  std::uniform_real_distribution<float> u(-1.0f, 1.0f);
  std::vector<float> l(5000), r(5000);
  for (auto& v : l) v = u(rng);
  for (auto& v : r) v = u(rng);
  const auto l0 = l, r0 = r;
  float* planes[2] = {l.data(), r.data()};
  eq.Process(planes, 5000);
  CHECK(std::memcmp(l.data(), l0.data(), l.size() * sizeof(float)) == 0);
  CHECK(std::memcmp(r.data(), r0.data(), r.size() * sizeof(float)) == 0);
}

TEST_CASE("shapes: after moving frequency/width/type live, the filter converges to the exact new response")
{
  GraphicEq eq;
  eq.Configure(1, kRate);
  EqParams p;
  eq.SetParams(p);
  eq.Reset();
  const int sequence[] = {kBandHighShelf, kBandNotch, kBandHighPass, kBandLowPass, kBandLowShelf, kBandBell};
  std::vector<float> chunk(kRate / 2);
  for (int type : sequence)
  {
    p = OneBand(11, type, 1700.0f, 1.4f, 6.0f);
    eq.SetParams(p);
    for (int pass = 0; pass < 2; ++pass)   // 1 s: far longer than the slowest morph
    {
      for (size_t i = 0; i < chunk.size(); ++i)
        chunk[i] = static_cast<float>(0.1 * std::sin(2.0 * kPi * 3000.0 * static_cast<double>(i) / kRate));
      float* planes[1] = {chunk.data()};
      eq.Process(planes, static_cast<int>(chunk.size()));
    }
    // measure the settled state with the same engine instance
    double si = 0, so = 0;
    std::vector<float> probe(kRate / 2);
    for (size_t i = 0; i < probe.size(); ++i)
      probe[i] = static_cast<float>(0.1 * std::sin(2.0 * kPi * 3000.0 * static_cast<double>(i) / kRate));
    const auto in = probe;
    float* planes[1] = {probe.data()};
    eq.Process(planes, static_cast<int>(probe.size()));
    for (size_t i = probe.size() / 2; i < probe.size(); ++i)
    {
      si += static_cast<double>(in[i]) * in[i];
      so += static_cast<double>(probe[i]) * probe[i];
    }
    CHECK_NEAR(10.0 * std::log10(so / si), Resp(p, 3000.0), 0.03);
  }
}

TEST_CASE("shapes: changing frequency, width, gain and shape every few ms never clicks")
{
  std::mt19937 rng(11);
  std::uniform_int_distribution<int> pickType(0, kBandTypeCount - 1);
  std::uniform_real_distribution<float> pickGain(-12.0f, 12.0f), pickLogF(std::log(100.0f), std::log(8000.0f)),
      pickW(0.3f, 3.0f);
  GraphicEq eq;
  eq.Configure(1, kRate);
  EqParams p;
  eq.SetParams(p);
  eq.Reset();
  const int total = kRate * 3, step = 300;   // a new random target every 6 ms: worst case of a hand on the mouse
  std::vector<float> y(static_cast<size_t>(total));
  for (int i = 0; i < total; ++i)
    y[static_cast<size_t>(i)] = static_cast<float>(0.5 * std::sin(2.0 * kPi * 1000.0 * i / kRate));
  for (int pos = 0; pos < total; pos += step)
  {
    p = OneBand(15, pickType(rng), std::exp(pickLogF(rng)), pickW(rng), pickGain(rng));
    eq.SetParams(p);
    float* planes[1] = {y.data() + pos};
    eq.Process(planes, std::min(step, total - pos));
  }
  double maxAbs = 0, maxStep = 0;
  for (int i = 1; i < total; ++i)
  {
    maxAbs = std::max(maxAbs, static_cast<double>(std::fabs(y[static_cast<size_t>(i)])));
    maxStep = std::max(maxStep, static_cast<double>(std::fabs(y[static_cast<size_t>(i)] - y[static_cast<size_t>(i - 1)])));
  }
  // Allowed: the steepest slope a 1 kHz sine of that size has, plus one coefficient update's worth of jump.
  // A 0.5 dB gain step on a strongly boosted band moves the output by up to ~6 % of its level, and gain,
  // frequency and fade steps can coincide, hence 12 % (-18 dB). A real click (an abrupt change of the
  // filter, as in an earlier version of this code) moves the output by 50..100 % of its level.
  const double limit = maxAbs * (2.0 * kPi * 1000.0 / kRate + 0.12);
  INFO("maxAbs " << maxAbs << " maxStep " << maxStep << " limit " << limit);
  CHECK(maxStep <= limit + 1e-3);
}

TEST_CASE("shapes: the result does not depend on how the audio is split into blocks")
{
  std::mt19937 rng(5);
  std::uniform_real_distribution<float> u(-0.5f, 0.5f);
  const int total = 48000;
  std::vector<float> src(static_cast<size_t>(total));
  for (auto& v : src) v = u(rng);
  EqParams a = OneBand(8, kBandHighShelf, 3000.0f, 1.0f, 6.0f);
  EqParams b = OneBand(8, kBandNotch, 700.0f, 0.5f, 0.0f);
  b.shape[20] = {150.0f, 1.9f, kBandHighPass};
  const int switchAt = 20000;

  auto run = [&](const std::vector<int>& sizes) {
    GraphicEq eq;
    eq.Configure(1, kRate);
    eq.SetParams(a);
    eq.Reset();
    std::vector<float> y = src;
    int pos = 0;
    size_t k = 0;
    while (pos < total)
    {
      if (pos == switchAt)
        eq.SetParams(b);
      int n = sizes[k++ % sizes.size()];
      n = std::min(n, total - pos);
      if (pos < switchAt)
        n = std::min(n, switchAt - pos);   // the parameter change happens at the same sample in every run
      float* planes[1] = {y.data() + pos};
      eq.Process(planes, n);
      pos += n;
    }
    return y;
  };
  const auto whole = run({48000});
  const auto odd = run({1, 7, 63, 64, 65, 333, 1000});
  const auto tiny = run({17});
  CHECK(std::memcmp(whole.data(), odd.data(), whole.size() * sizeof(float)) == 0);
  CHECK(std::memcmp(whole.data(), tiny.data(), whole.size() * sizeof(float)) == 0);
}

TEST_CASE("shapes: extreme and random settings stay finite and bounded at every sample rate")
{
  std::mt19937 rng(21);
  std::uniform_int_distribution<int> pickType(0, kBandTypeCount - 1);
  std::uniform_real_distribution<float> pickGain(-12.0f, 12.0f), pickLogF(std::log(20.0f), std::log(20000.0f)),
      pickLogW(std::log(0.05f), std::log(4.0f)), u(-1.0f, 1.0f);
  for (int rate : {32000, 44100, 48000, 96000})
  {
    GraphicEq eq;
    eq.Configure(6, rate);
    EqParams p;
    eq.SetParams(p);
    eq.Reset();
    const int blocks = 40, block = rate / 20;
    std::vector<std::vector<float>> ch(6, std::vector<float>(static_cast<size_t>(block)));
    float peak = 0.0f;
    for (int k = 0; k < blocks; ++k)
    {
      EqParams q;
      for (int b = 0; b < kBands; ++b)
      {
        q.shape[b].type = pickType(rng);
        q.shape[b].freqHz = std::exp(pickLogF(rng));
        q.shape[b].widthOct = std::exp(pickLogW(rng));
        for (int c = 0; c < kMaxChannels; ++c)
          q.bandDb[c][b] = (rng() % 4 == 0) ? pickGain(rng) : 0.0f;
      }
      {
        DspParams wrap;
        wrap.eq = q;
        Sanitize(wrap);
        q = wrap.eq;
      }
      eq.SetParams(q);
      float* planes[6];
      for (int c = 0; c < 6; ++c)
      {
        for (auto& v : ch[static_cast<size_t>(c)]) v = 0.2f * u(rng);
        planes[c] = ch[static_cast<size_t>(c)].data();
      }
      eq.Process(planes, block);
      for (auto& v : ch)
        for (float s : v)
        {
          REQUIRE(std::isfinite(s));
          peak = std::max(peak, std::fabs(s));
        }
    }
    INFO("rate " << rate << " peak " << peak);
    CHECK(peak < 1000.0f);
  }
}

TEST_CASE("shapes: settings file keeps shapes, leaves untouched files as they were, reads partial input")
{
  DspParams plain;
  plain.enabled = 1;
  const std::string plainText = FormatSettings(plain);
  CHECK(plainText.find("band_type") == std::string::npos);     // factory layout: no new keys, same file as before
  CHECK(plainText.find("band_freq_hz") == std::string::npos);
  CHECK(plainText.find("band_width_oct") == std::string::npos);

  DspParams d;
  d.enabled = 1;
  d.eq.shape[2] = {42.5f, 1.9f, kBandHighPass};
  d.eq.shape[10] = {1234.0f, 0.75f, kBandLowShelf};
  d.eq.shape[11].type = kBandNotch;
  d.eq.shape[11].widthOct = 0.2f;
  d.eq.bandDb[0][10] = 5.5f;
  const std::string text = FormatSettings(d);
  CHECK(text.find("\"band_type\"") != std::string::npos);
  CHECK(text.find("\"high_pass\"") != std::string::npos);
  CHECK(text.find("\"low_shelf\"") != std::string::npos);
  DspParams back;
  std::string err;
  REQUIRE(ParseSettings(text, back, &err));
  for (int b = 0; b < kBands; ++b)
  {
    CHECK_NEAR(back.eq.shape[b].freqHz, d.eq.shape[b].freqHz, 0.01);
    CHECK_NEAR(back.eq.shape[b].widthOct, d.eq.shape[b].widthOct, 0.0006);
    CHECK(back.eq.shape[b].type == d.eq.shape[b].type);
  }
  CHECK(back.eq.bandDb[0][10] == 5.5f);
  CHECK(back.eq.bandDb[3][10] == 0.0f);

  // old files (no shape keys) load as the factory layout
  DspParams oldStyle;
  REQUIRE(ParseSettings(plainText, oldStyle, &err));
  for (int b = 0; b < kBands; ++b)
  {
    CHECK(oldStyle.eq.shape[b].freqHz == kBandHz[b]);
    CHECK(oldStyle.eq.shape[b].type == kBandBell);
  }

  // hand-written, partial: names, numbers, unknown names, a short array, a typed band without its own width
  DspParams hand;
  REQUIRE(ParseSettings("{\"enabled\": true, \"eq\": {\"band_type\": [\"low_pass\", 4, \"banana\", \"notch\"],"
                        " \"band_freq_hz\": [15000, 50], \"band_width_oct\": [0.9]}}", hand, &err));
  CHECK(hand.eq.shape[0].type == kBandLowPass);
  CHECK(hand.eq.shape[0].freqHz == 15000.0f);
  CHECK(hand.eq.shape[0].widthOct == 0.9f);
  CHECK(hand.eq.shape[1].type == kBandHighPass);
  CHECK(hand.eq.shape[1].freqHz == 50.0f);
  CHECK(hand.eq.shape[1].widthOct == DefaultBandWidthOct(kBandHighPass));
  CHECK(hand.eq.shape[2].type == kBandBell);                 // unknown name -> bell
  CHECK(hand.eq.shape[3].type == kBandNotch);
  CHECK(hand.eq.shape[3].freqHz == kBandHz[3]);              // not listed -> factory value
  CHECK(hand.eq.shape[29].type == kBandBell);
  // out-of-range numbers are clamped by Sanitize
  DspParams wild;
  REQUIRE(ParseSettings("{\"eq\": {\"band_freq_hz\": [1, 99999], \"band_width_oct\": [0, 50]}}", wild, &err));
  CHECK(wild.eq.shape[0].freqHz == kBandFreqMinHz);
  CHECK(wild.eq.shape[1].freqHz == kBandFreqMaxHz);
  CHECK(wild.eq.shape[0].widthOct == kBandWidthMinOct);
  CHECK(wild.eq.shape[1].widthOct == kBandWidthMaxOct);
}

TEST_CASE("studio model: shape editing, default widths per shape, reset, Q, auto-preamp with shelves")
{
  StudioModel m;
  m.SetFreq(7, 5.0f);
  CHECK(m.Shape(7).freqHz == kBandFreqMinHz);
  m.SetFreq(7, 99999.0f);
  CHECK(m.Shape(7).freqHz == kBandFreqMaxHz);
  m.SetFreq(7, 1234.567f);
  CHECK(m.Shape(7).freqHz == 1234.57f);
  m.SetWidth(7, 0.0f);
  CHECK(m.Shape(7).widthOct == kBandWidthMinOct);
  m.SetWidth(7, 99.0f);
  CHECK(m.Shape(7).widthOct == kBandWidthMaxOct);

  m.SetWidth(7, static_cast<float>(kBandWidthOct));
  m.SetType(7, kBandHighPass);                       // untouched width follows the shape
  CHECK(m.Shape(7).type == kBandHighPass);
  CHECK(m.Shape(7).widthOct == DefaultBandWidthOct(kBandHighPass));
  m.SetWidth(7, 0.8f);
  m.SetType(7, kBandLowShelf);                       // a width the user chose stays
  CHECK(m.Shape(7).widthOct == 0.8f);
  m.SetType(7, 77);                                  // junk becomes bell
  CHECK(m.Shape(7).type == kBandBell);

  m.SetBand(7, 4.0f);
  m.ResetBand(7);
  CHECK(m.Shape(7).freqHz == kBandHz[7]);
  CHECK(m.Shape(7).type == kBandBell);
  CHECK_NEAR(m.Shape(7).widthOct, kBandWidthOct, 1e-6);
  CHECK(m.Band(7) == 0.0f);

  CHECK_NEAR(m.Q(15, 48000), 1.0 / (2.0 * std::sinh(0.5 * std::log(2.0) * kBandWidthOct)), 0.05);
  m.SetWidth(15, 0.1f);
  const double narrow = m.Q(15, 48000);
  m.SetWidth(15, 2.0f);
  CHECK(narrow > m.Q(15, 48000) * 5.0);

  StudioModel s;
  s.SetType(10, kBandLowShelf);
  s.SetFreq(10, 1000.0f);
  s.SetBand(10, 6.0f);
  double curve[kCurvePoints];
  s.Curve(48000, curve);
  CHECK(curve[0] > 5.0);                              // the whole low end is lifted
  CHECK(std::fabs(curve[kCurvePoints - 1]) < 0.5);    // the top is not
  const float pre = s.AutoPreampDb(48000);
  double peak = 0;
  for (double v : curve) peak = std::max(peak, v);
  CHECK(pre <= -peak);
  CHECK(pre > -peak - 0.51f);

  s.ResetEq();
  CHECK(s.Shape(10).type == kBandBell);
  CHECK(s.Shape(10).freqHz == kBandHz[10]);
  CHECK(s.Band(10) == 0.0f);

  CHECK(std::string(BandTypeShort(kBandBell)) == "B");
  CHECK(std::string(BandTypeShort(kBandHighPass)) == "HP");
  CHECK(WidthFraction(kBandWidthMinOct) == doctest::Approx(0.0f));
  CHECK(WidthFraction(kBandWidthMaxOct) == doctest::Approx(1.0f));
  CHECK(WidthFraction(0.3f) < WidthFraction(1.0f));
  CHECK(FreqLabel(1234.0) == "1.2k");
}
