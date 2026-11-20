// test_dsp_settings.cpp — dsp-settings.json (parse / format / save / live reload) and the PCM-mode
// helper, i.e. everything between the settings file and the DSP chain that does not need Windows.
#include "doctest.h"
#include "dsp/DspChain.h"
#include "dsp/DspSettings.h"
#include "dsp/PcmDsp.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

using namespace vac3dsp;
namespace fs = std::filesystem;

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

std::string TempPath(const char* name)
{
  return (fs::temp_directory_path() / (std::string("vac3dsp_test_") + name)).string();
}

void WriteText(const std::string& path, const std::string& text)
{
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f << text;
}

std::string ZeroBands()
{
  std::string s = "[";
  for (int i = 0; i < kBands; ++i)
    s += (i ? ",0" : "0");
  return s + "]";
}

double RmsDb(const std::vector<float>& v, size_t from, size_t to)
{
  double s = 0.0;
  for (size_t i = from; i < to; ++i)
    s += static_cast<double>(v[i]) * v[i];
  return 10.0 * std::log10(s / static_cast<double>(to - from) + 1e-30);
}
}  // namespace

// ---------------------------------------------------------------------------------------------
TEST_CASE("settings: an empty object gives the defaults (master off)")
{
  DspParams p;
  p.enabled = 1;
  REQUIRE(ParseSettings("{}", p));
  CHECK(p.enabled == 0);
  CHECK(p.eq.enabled == 1);
  CHECK(p.comp.enabled == 0);
  CHECK(p.order == 0);
  for (int c = 0; c < kMaxChannels; ++c)
    for (int b = 0; b < kBands; ++b)
      CHECK(p.eq.bandDb[c][b] == 0.0f);
}

TEST_CASE("settings: flat bands_db applies to every channel, per-channel arrays to their channel")
{
  DspParams p;
  std::string flat = R"({"enabled": true, "eq": {"preamp_db": -4.5, "bands_db": [)";
  for (int i = 0; i < kBands; ++i)
    flat += (i ? "," : "") + std::to_string(i % 7 - 3);
  flat += "]}}";
  REQUIRE(ParseSettings(flat, p));
  CHECK(p.enabled == 1);
  CHECK_NEAR(p.eq.preampDb, -4.5, 1e-6);
  for (int c = 0; c < kMaxChannels; ++c)
    for (int b = 0; b < kBands; ++b)
      CHECK(p.eq.bandDb[c][b] == static_cast<float>(b % 7 - 3));

  std::string per = R"({"eq": {"bands_db": [[6,6,6], [)" + ZeroBands() + R"(], [-3]]}})";
  REQUIRE(ParseSettings(per, p));
  CHECK(p.eq.bandDb[0][0] == 6.0f);
  CHECK(p.eq.bandDb[0][2] == 6.0f);
  CHECK(p.eq.bandDb[0][3] == 0.0f);   // short row: the rest is 0 dB
  CHECK(p.eq.bandDb[1][5] == 0.0f);
  CHECK(p.eq.bandDb[2][0] == -3.0f);
  CHECK(p.eq.bandDb[4][0] == 0.0f);   // channels not listed stay flat
}

TEST_CASE("settings: compressor, order, booleans as numbers, unknown keys, UTF-8 BOM")
{
  DspParams p;
  const std::string text = "\xEF\xBB\xBF" R"({
    "version": 1, "future_feature": {"x": [1,2,3]},
    "enabled": 1, "order": "comp_eq",
    "compressor": {"enabled": true, "threshold_db": -24, "ratio": 4, "attack_ms": 5,
                   "release_ms": 200, "knee_db": 3, "makeup_db": 2.5, "lfe_in_detector": true}
  })";
  REQUIRE(ParseSettings(text, p));
  CHECK(p.enabled == 1);
  CHECK(p.order == 1);
  CHECK(p.comp.enabled == 1);
  CHECK(p.comp.thresholdDb == -24.0f);
  CHECK(p.comp.ratio == 4.0f);
  CHECK(p.comp.attackMs == 5.0f);
  CHECK(p.comp.releaseMs == 200.0f);
  CHECK(p.comp.kneeDb == 3.0f);
  CHECK(p.comp.makeupDb == 2.5f);
  CHECK(p.comp.lfeInDetector == 1);
}

TEST_CASE("settings: out-of-range and wrongly typed values are sanitised, never trusted")
{
  DspParams p;
  REQUIRE(ParseSettings(R"({"enabled": "yes", "eq": {"preamp_db": 999, "bands_db": [100, -100, "x", null]},
                            "compressor": {"enabled": true, "ratio": 0, "attack_ms": -3, "makeup_db": 1e9}})", p));
  CHECK(p.enabled == 0);                    // a string is not a boolean: default kept
  CHECK(p.eq.preampDb == kEqPreampMaxDb);
  CHECK(p.eq.bandDb[0][0] == kEqBandMaxDb);
  CHECK(p.eq.bandDb[0][1] == -kEqBandMaxDb);
  CHECK(p.eq.bandDb[0][2] == 0.0f);
  CHECK(p.comp.ratio == 1.0f);
  CHECK(p.comp.attackMs == 0.1f);
  CHECK(p.comp.makeupDb == 24.0f);
}

TEST_CASE("settings: invalid files are rejected with a message and leave the parameters untouched")
{
  const char* bad[] = {"", "   ", "{", "{\"eq\": ", "[1,2,3]", "{\"a\": }", "{\"a\": 1,}", "{\"a\": 1} trailing",
                       "{\"a\": tru}", "{\"a\": \"unterminated}", "not json at all"};
  for (const char* text : bad)
  {
    DspParams p;
    p.enabled = 1;
    p.eq.preampDb = -7.0f;
    std::string err;
    INFO("text: " << text);
    CHECK_FALSE(ParseSettings(text, p, &err));
    CHECK_FALSE(err.empty());
    CHECK(p.enabled == 1);
    CHECK(p.eq.preampDb == -7.0f);
  }
  std::string deep(40, '[');
  std::string err;
  DspParams p;
  CHECK_FALSE(ParseSettings(deep, p, &err));  // no stack overflow on hostile nesting
}

TEST_CASE("settings: FormatSettings -> ParseSettings round-trips (linked and per-channel curves)")
{
  DspParams a;
  a.enabled = 1;
  a.order = 1;
  a.eq.preampDb = -3.25f;
  for (int c = 0; c < kMaxChannels; ++c)
    for (int b = 0; b < kBands; ++b)
      a.eq.bandDb[c][b] = static_cast<float>((b % 9) - 4) * 1.5f;
  a.comp.enabled = 1;
  a.comp.thresholdDb = -22.5f;
  a.comp.ratio = 2.5f;
  a.comp.attackMs = 7.5f;
  a.comp.releaseMs = 250.0f;
  a.comp.kneeDb = 8.0f;
  a.comp.makeupDb = 3.0f;
  a.comp.lfeInDetector = 1;

  for (int variant = 0; variant < 2; ++variant)
  {
    if (variant == 1)
      for (int b = 0; b < kBands; ++b)
        a.eq.bandDb[2][b] = static_cast<float>(b % 5);  // centre channel differs: per-channel form
    const std::string text = FormatSettings(a);
    CHECK((text.find("[\n") != std::string::npos) == (variant == 1));
    DspParams back;
    std::string err;
    REQUIRE_MESSAGE(ParseSettings(text, back, &err), err);
    CHECK(std::memcmp(&a.eq, &back.eq, sizeof(a.eq)) == 0);
    CHECK(back.enabled == 1);
    CHECK(back.order == 1);
    CHECK(back.comp.thresholdDb == -22.5f);
    CHECK(back.comp.releaseMs == 250.0f);
    CHECK(back.comp.lfeInDetector == 1);
  }
}

TEST_CASE("settings: SaveSettings writes atomically and SettingsFile reloads only real changes")
{
  const std::string path = TempPath("live.json");
  fs::remove(path);
  SettingsFile watcher(path);
  DspParams p;

  CHECK(watcher.Poll(p) == SettingsFile::Result::Missing);     // reported once
  CHECK(watcher.Poll(p) == SettingsFile::Result::Unchanged);

  DspParams v1;
  v1.enabled = 1;
  v1.eq.preampDb = -2.0f;
  REQUIRE(SaveSettings(path, v1));
  CHECK_FALSE(fs::exists(path + ".tmp"));
  CHECK(watcher.Poll(p) == SettingsFile::Result::Loaded);
  CHECK(p.enabled == 1);
  CHECK(p.eq.preampDb == -2.0f);
  CHECK(watcher.Poll(p) == SettingsFile::Result::Unchanged);   // same file: nothing to do

  WriteText(path, "{ this is broken");                         // user is half-way through an edit
  fs::last_write_time(path, fs::last_write_time(path) + std::chrono::seconds(2));
  p.eq.preampDb = -2.0f;
  std::string err;
  CHECK(watcher.Poll(p, &err) == SettingsFile::Result::Failed);
  CHECK_FALSE(err.empty());
  CHECK(p.eq.preampDb == -2.0f);                               // previous parameters survive
  CHECK(watcher.Poll(p) == SettingsFile::Result::Unchanged);   // error reported only once

  DspParams v2 = v1;
  v2.eq.preampDb = -9.0f;
  REQUIRE(SaveSettings(path, v2));
  fs::last_write_time(path, fs::last_write_time(path) + std::chrono::seconds(4));
  CHECK(watcher.Poll(p) == SettingsFile::Result::Loaded);
  CHECK(p.eq.preampDb == -9.0f);

  fs::remove(path);
  CHECK(watcher.Poll(p) == SettingsFile::Result::Missing);
  CHECK(watcher.Poll(p) == SettingsFile::Result::Unchanged);
}

// ---------------------------------------------------------------------------------------------
TEST_CASE("file -> sound: a +12 dB band written in dsp-settings.json boosts that frequency by 12 dB")
{
  const int band = 16;                       // 904 Hz
  const double f = static_cast<double>(kBandHz[band]);
  std::string bands = "[";
  for (int i = 0; i < kBands; ++i)
    bands += std::string(i ? "," : "") + (i == band ? "12" : "0");
  bands += "]";
  DspParams p;
  REQUIRE(ParseSettings(R"({"enabled": true, "eq": {"bands_db": )" + bands + "}}", p));

  DspChain chain;
  chain.Configure(6, kRate);
  chain.SetParams(p);
  const int total = kRate * 2;
  std::vector<float> in(static_cast<size_t>(total)), out;
  for (int n = 0; n < total; ++n)
    in[static_cast<size_t>(n)] = 0.1f * static_cast<float>(std::sin(2.0 * kPi * f * n / kRate));
  std::vector<std::vector<float>> ch(6, in);
  for (int pos = 0; pos < total; pos += 1536)
  {
    float* planes[6];
    for (int c = 0; c < 6; ++c)
      planes[c] = ch[static_cast<size_t>(c)].data() + pos;
    chain.Process(planes, std::min(1536, total - pos));
  }
  const size_t from = static_cast<size_t>(total) - 24000, to = static_cast<size_t>(total);
  CHECK_NEAR(RmsDb(ch[0], from, to) - RmsDb(in, from, to), 12.0, 0.05);
  CHECK_NEAR(RmsDb(ch[5], from, to) - RmsDb(in, from, to), 12.0, 0.05);  // all channels via the flat form
}

TEST_CASE("chain: parameters set before Configure() survive it; Bypassed() tracks the fade")
{
  DspParams p;
  p.enabled = 1;
  p.eq.preampDb = 6.0f;

  DspChain chain;
  chain.SetParams(p);
  chain.Configure(2, kRate);
  chain.PullParams();
  CHECK_FALSE(chain.Bypassed());                // the request made before Configure() is active

  DspChain idle;
  idle.Configure(2, kRate);
  idle.PullParams();
  CHECK(idle.Bypassed());                       // defaults: master off

  std::vector<float> a(kRate / 10, 0.1f), b(kRate / 10, 0.1f);
  float* planes[2] = {a.data(), b.data()};
  chain.Process(planes, kRate / 10);            // 100 ms of audio: the EQ stage has faded in completely
  p.enabled = 0;
  chain.SetParams(p);
  chain.PullParams();
  CHECK_FALSE(chain.Bypassed());                // switched off, but the 10 ms fade-out has not run yet
  chain.Process(planes, kRate / 10);
  CHECK(chain.Bypassed());                      // 100 ms later the fade is over
}

// ---------------------------------------------------------------------------------------------
TEST_CASE("PCM helper: a bypassed chain leaves int16 / int32 / float buffers bit-identical")
{
  DspChain chain;
  chain.Configure(2, kRate);
  const int frames = 4800;
  std::vector<float> l(frames), r(frames);

  std::vector<int16_t> s16(static_cast<size_t>(frames) * 2);
  std::vector<int32_t> s32(static_cast<size_t>(frames) * 2);
  std::vector<float> f32(static_cast<size_t>(frames) * 2);
  for (size_t i = 0; i < s16.size(); ++i)
  {
    s16[i] = static_cast<int16_t>((i * 7919u) % 65536u - 32768u);
    s32[i] = static_cast<int32_t>((i * 2654435761u));
    f32[i] = static_cast<float>(static_cast<int>(i % 2001) - 1000) / 1000.0f * 1.5f;  // even > 1.0
  }
  const auto s16c = s16;
  const auto s32c = s32;
  const auto f32c = f32;
  CHECK_FALSE(ProcessInterleavedStereo(chain, PcmFormat::Int16, s16.data(), frames, l.data(), r.data()));
  CHECK_FALSE(ProcessInterleavedStereo(chain, PcmFormat::Int32, s32.data(), frames, l.data(), r.data()));
  CHECK_FALSE(ProcessInterleavedStereo(chain, PcmFormat::Float32, f32.data(), frames, l.data(), r.data()));
  CHECK(s16 == s16c);
  CHECK(s32 == s32c);
  CHECK(f32 == f32c);
}

TEST_CASE("PCM helper: with the DSP on, a flat curve and 0 dB preamp is transparent in int16 and clamps instead of wrapping")
{
  DspChain chain;
  chain.Configure(2, kRate);
  DspParams p;
  p.enabled = 1;
  chain.SetParams(p);                           // EQ on but flat, compressor off
  const int frames = 48000;
  std::vector<float> l(frames), r(frames);
  std::vector<int16_t> pcm(static_cast<size_t>(frames) * 2);
  for (int n = 0; n < frames; ++n)
  {
    pcm[static_cast<size_t>(n) * 2] = static_cast<int16_t>(std::lround(8000.0 * std::sin(2.0 * kPi * 440.0 * n / kRate)));
    pcm[static_cast<size_t>(n) * 2 + 1] = static_cast<int16_t>(-pcm[static_cast<size_t>(n) * 2]);
  }
  const auto orig = pcm;
  for (int pos = 0; pos < frames; pos += 1024)  // fade-in done after 10 ms, so compare afterwards
    CHECK(ProcessInterleavedStereo(chain, PcmFormat::Int16, pcm.data() + pos * 2, std::min(1024, frames - pos), l.data(),
                                   r.data()));
  for (size_t i = 2 * 2400; i < pcm.size(); ++i)
    REQUIRE(pcm[i] == orig[i]);                 // flat EQ at full wet: exact round trip through float

  // +12 dB preamp on a loud signal: must saturate at +/-32767, never wrap to the other sign
  DspParams boost = p;
  boost.eq.preampDb = 12.0f;
  chain.SetParams(boost);
  std::vector<int16_t> loud(static_cast<size_t>(frames) * 2);
  for (int n = 0; n < frames; ++n)
    loud[static_cast<size_t>(n) * 2] = loud[static_cast<size_t>(n) * 2 + 1] =
        static_cast<int16_t>(std::lround(20000.0 * std::sin(2.0 * kPi * 100.0 * n / kRate)));
  for (int pos = 0; pos < frames; pos += 1024)
    ProcessInterleavedStereo(chain, PcmFormat::Int16, loud.data() + pos * 2, std::min(1024, frames - pos), l.data(),
                             r.data());
  int maxv = 0, minv = 0;
  for (size_t i = 2 * 4800; i < loud.size(); i += 2)
  {
    maxv = std::max<int>(maxv, loud[i]);
    minv = std::min<int>(minv, loud[i]);
  }
  CHECK(maxv == 32767);
  CHECK(minv == -32768);
}

// ---------------------------------------------------------------------------------------------
#include "dsp/Mailbox.h"
#include <atomic>
#include <thread>

namespace {
struct Block
{
  int v[48] = {};
  void Fill(int x)
  {
    for (int& i : v)
      i = x;
  }
  bool Consistent() const
  {
    for (int i : v)
      if (i != v[0])
        return false;
    return true;
  }
};
}  // namespace

TEST_CASE("mailbox: nothing new -> nullptr; only the newest value is delivered; Repost re-delivers")
{
  Mailbox<Block> box;
  CHECK(box.Take() == nullptr);
  Block b;
  b.Fill(1);
  box.Post(b);
  b.Fill(2);
  box.Post(b);
  b.Fill(3);
  box.Post(b);
  const Block* got = box.Take();
  REQUIRE(got != nullptr);
  CHECK(got->v[0] == 3);                 // 1 and 2 were superseded
  CHECK(box.Take() == nullptr);
  box.Repost();
  got = box.Take();
  REQUIRE(got != nullptr);
  CHECK(got->v[47] == 3);                // the last posted value again
}

TEST_CASE("mailbox: several writers and a reader never produce a torn or older-than-seen value")
{
  Mailbox<Block> box;
  std::atomic<bool> stop{false};
  std::atomic<int> counter{0};
  std::vector<std::thread> writers;
  for (int t = 0; t < 3; ++t)
    writers.emplace_back([&] {
      Block b;
      while (!stop.load())
      {
        b.Fill(counter.fetch_add(1) + 1);
        box.Post(b);
      }
    });
  // Wait until the writers are really posting (thread start-up can be slower than a fast reader),
  // then read until both enough iterations and enough fresh values were seen (with a time limit).
  using Clock = std::chrono::steady_clock;
  const auto deadline = Clock::now() + std::chrono::seconds(10);
  while (counter.load() < 2000 && Clock::now() < deadline)
    std::this_thread::yield();
  bool torn = false;
  int taken = 0;
  for (int i = 0; (i < 200000 || taken < 100) && Clock::now() < deadline; ++i)
    if (const Block* got = box.Take())
    {
      ++taken;
      if (!got->Consistent())
        torn = true;
    }
  stop = true;
  for (auto& w : writers)
    w.join();
  CHECK_FALSE(torn);
  CHECK(taken >= 100);
}
