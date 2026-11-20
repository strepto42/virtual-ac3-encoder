// test_studio_tools.cpp — loudness matching, Equalizer APO / AutoEq / REW import and export, auto profiles.
#include "doctest.h"
#include "dsp/AutoProfiles.h"
#include "dsp/EqImport.h"
#include "dsp/Loudness.h"
#include "dsp/MeterShm.h"

#include <cmath>
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
using Spec = float[kMaxChannels][kSpecBands];

void Fill(Spec s, float db)
{
  for (int c = 0; c < kMaxChannels; ++c)
    for (int b = 0; b < kSpecBands; ++b)
      s[c][b] = db;
}
void Floor(Spec s) { Fill(s, kSpecFloorDb); }
}  // namespace

TEST_CASE("k-weighting: ~0 dB in the middle, +4 dB shelf at the top, steep roll-off at the bottom")
{
  CHECK_NEAR(KWeightDb(1000.0), 0.7, 0.4);     // the shelf has begun to rise
  CHECK_NEAR(KWeightDb(10000.0), 4.0, 0.4);    // full shelf
  CHECK_NEAR(KWeightDb(100.0), -1.2, 0.4);      // the 38 Hz high-pass is still biting a little
  CHECK(KWeightDb(20.0) < -12.0);
  CHECK(KWeightDb(38.13547) < -3.0);           // high-pass corner (Q 0.5: -6 dB)
}

TEST_CASE("loudness matcher: how many dB the output must change to be as loud as the input")
{
  Spec in, out;
  LoudnessMatcher m;
  Fill(in, -40.0f);
  Fill(out, -37.0f);                            // everything 3 dB louder after the chain
  for (int i = 0; i < 30; ++i)
    m.AddFrame(in, out);
  CHECK(m.Frames() == 30);
  CHECK_NEAR(m.DeltaDb(), -3.0, 0.01);          // so turn the output down by 3 dB

  m.Reset();
  Fill(out, -44.0f);                            // 4 dB quieter
  for (int i = 0; i < 10; ++i)
    m.AddFrame(in, out);
  CHECK_NEAR(m.DeltaDb(), 4.0, 0.01);

  m.Reset();                                    // silence does not count
  Floor(in);
  Floor(out);
  for (int i = 0; i < 20; ++i)
    m.AddFrame(in, out);
  CHECK(m.Frames() == 0);
  CHECK(m.DeltaDb() == 0.0);
}

TEST_CASE("loudness matcher: weighting by frequency, surround channels count more, the LFE not at all")
{
  // a boost at 4 kHz is heard more than the same boost at 40 Hz: K-weighting sees the difference
  Spec in, lowBoost, highBoost;
  Fill(in, -50.0f);
  Fill(lowBoost, -50.0f);
  Fill(highBoost, -50.0f);
  for (int c = 0; c < kMaxChannels; ++c)
  {
    lowBoost[c][3] = -30.0f;                    // ~25 Hz
    highBoost[c][100] = -30.0f;                 // ~7 kHz
  }
  LoudnessMatcher a, b;
  a.AddFrame(in, lowBoost);
  b.AddFrame(in, highBoost);
  CHECK(b.DeltaDb() < a.DeltaDb() - 3.0);       // the 7 kHz boost makes the output louder: more negative delta

  // the same extra energy: in the LFE it counts nothing, in a surround channel a bit more than in the front
  Spec lfe, sur, front;
  Fill(in, -50.0f);
  Fill(lfe, -50.0f);
  Fill(sur, -50.0f);
  Fill(front, -50.0f);
  for (int bnd = 40; bnd < 80; ++bnd)
  {
    lfe[3][bnd] = -30.0f;
    sur[4][bnd] = -30.0f;
    front[0][bnd] = -30.0f;
  }
  LoudnessMatcher l, s, f;
  l.AddFrame(in, lfe);
  s.AddFrame(in, sur);
  f.AddFrame(in, front);
  CHECK(std::fabs(l.DeltaDb()) < 1e-9);
  CHECK(s.DeltaDb() < f.DeltaDb());
}

// ---------------------------------------------------------------------------------------------------------
namespace {
double Resp(const EqParams& eq, int channel, double hz)
{
  return GraphicEq::ResponseDb(eq.bandDb[channel], eq.shape, hz, 48000);
}
}  // namespace

TEST_CASE("apo import: an AutoEq file lands on bands with its own frequencies, shapes, widths and gains")
{
  const std::string text =
      "Preamp: -5.9 dB\n"
      "Filter 1: ON LSC Fc 105 Hz Gain 6.2 dB Q 0.70\n"
      "Filter 2: ON PK Fc 2600 Hz Gain -4.1 dB Q 2.10\n"
      "Filter 3: ON PK Fc 7400 Hz Gain 3.5 dB Q 3.00\n"
      "Filter 4: ON HSC Fc 10000 Hz Gain -2.0 dB Q 0.70\n"
      "Filter 5: OFF PK Fc 500 Hz Gain 3 dB Q 1\n";
  EqParams eq;
  EqImportResult r;
  REQUIRE(ParseApoText(text, 48000, eq, &r));
  CHECK(r.imported == 4);
  CHECK(r.skipped == 1);
  CHECK(r.preamp);
  CHECK_NEAR(eq.preampDb, -5.9, 1e-4);

  int bell2600 = -1, shelfLow = -1;
  for (int b = 0; b < kBands; ++b)
  {
    if (eq.shape[b].type == kBandBell && std::fabs(eq.shape[b].freqHz - 2600.0f) < 0.5f) bell2600 = b;
    if (eq.shape[b].type == kBandLowShelf) shelfLow = b;
  }
  REQUIRE(bell2600 >= 0);
  REQUIRE(shelfLow >= 0);
  CHECK_NEAR(GraphicEq::WidthToQ(eq.shape[bell2600].widthOct, 2600.0, 48000, kBandBell), 2.10, 0.01);   // Q survives
  CHECK_NEAR(eq.bandDb[0][bell2600], -4.1, 1e-4);
  CHECK(eq.bandDb[5][bell2600] == eq.bandDb[0][bell2600]);                                              // all channels
  CHECK_NEAR(GraphicEq::WidthToQ(eq.shape[shelfLow].widthOct, 105.0, 48000, kBandLowShelf), 0.70, 0.01);
  // the curve really is what the file describes: gain at the centre of the bells, shelf plateaus
  CHECK_NEAR(Resp(eq, 0, 2600.0), -4.1, 0.25);
  CHECK(Resp(eq, 0, 7400.0) > 2.6);             // +3.5 dB bell, pulled down a little by the -2 dB shelf at 10 kHz
  CHECK(Resp(eq, 0, 7400.0) < 3.6);
  CHECK(Resp(eq, 0, 30.0) > 5.0);
  CHECK(Resp(eq, 0, 18000.0) < -1.2);
  CHECK(std::fabs(Resp(eq, 0, 500.0)) < 1.0);   // the OFF filter did nothing
}

TEST_CASE("apo import: REW spacing, decimal commas, the BW form, cut filters and what is not supported")
{
  EqParams eq;
  EqImportResult r;
  REQUIRE(ParseApoText("Filter  1: ON  PK       Fc    63,0 Hz  Gain  -4,5 dB  Q  4,30\n"
                       "Filter: ON PK Fc 1000 Hz Gain 3 dB BW Oct 0.5\n"
                       "Filter 3: ON HPQ Fc 30 Hz Q 0.707\n"
                       "Filter 4: ON NO Fc 5000 Hz Q 10\n"
                       "Filter 5: ON BP Fc 400 Hz Q 1\n"
                       "Channel: L\n"
                       "# a comment\n",
                       48000, eq, &r));
  CHECK(r.imported == 4);
  CHECK(r.skipped == 1);
  CHECK(r.notes.size() >= 2);                   // the band-pass and the Channel line are reported
  int n63 = -1, n1k = -1, hp = -1, no = -1;
  for (int b = 0; b < kBands; ++b)
  {
    if (std::fabs(eq.shape[b].freqHz - 63.0f) < 0.1f) n63 = b;
    if (std::fabs(eq.shape[b].freqHz - 1000.0f) < 0.1f) n1k = b;
    if (eq.shape[b].type == kBandHighPass) hp = b;
    if (eq.shape[b].type == kBandNotch) no = b;
  }
  REQUIRE((n63 >= 0 && n1k >= 0 && hp >= 0 && no >= 0));
  CHECK_NEAR(eq.bandDb[0][n63], -4.5, 1e-4);
  CHECK_NEAR(eq.shape[n1k].widthOct, 0.5, 1e-4);                // BW Oct goes straight in
  CHECK_NEAR(eq.bandDb[0][n1k], 3.0, 1e-4);
  CHECK(Resp(eq, 0, 5000.0) < -20.0);                           // the notch is deep
  CHECK(Resp(eq, 0, 10.0) < -18.0);                             // the high-pass cuts the bottom (12 dB/oct, corner 30 Hz)

  EqParams none;
  EqImportResult empty;
  CHECK_FALSE(ParseApoText("this is not an EQ preset\n", 48000, none, &empty));
}

TEST_CASE("apo import: limits - more than 30 filters, gain beyond 12 dB, absurd Q")
{
  std::string text;
  for (int i = 0; i < 40; ++i)
    text += "Filter " + std::to_string(i + 1) + ": ON PK Fc " + std::to_string(40 + i * 400) + " Hz Gain 1 dB Q 2\n";
  EqParams eq;
  EqImportResult r;
  REQUIRE(ParseApoText(text, 48000, eq, &r));
  CHECK(r.imported == kBands);
  CHECK(r.skipped == 10);

  EqParams eq2;
  EqImportResult r2;
  REQUIRE(ParseApoText("Filter 1: ON PK Fc 1000 Hz Gain 20 dB Q 500\nFilter 2: ON PK Fc 3000 Hz Gain -30 dB Q 0.01\n", 48000, eq2, &r2));
  int found = 0;
  for (int b = 0; b < kBands; ++b)
    if (eq2.shape[b].freqHz == 1000.0f || eq2.shape[b].freqHz == 3000.0f)
    {
      ++found;
      CHECK(std::fabs(eq2.bandDb[0][b]) == kEqBandMaxDb);
      CHECK(eq2.shape[b].widthOct >= kBandWidthMinOct);
      CHECK(eq2.shape[b].widthOct <= kBandWidthMaxOct);
    }
  CHECK(found == 2);
  CHECK(r2.notes.size() >= 4);
}

TEST_CASE("apo export / import round trip keeps the curve")
{
  EqParams eq;
  eq.preampDb = -4.5f;
  eq.shape[4] = {85.0f, 1.9f, kBandLowShelf};
  eq.shape[10] = {420.0f, 0.5f, kBandBell};
  eq.shape[18] = {1800.0f, 0.12f, kBandBell};
  eq.shape[22] = {5200.0f, 0.4f, kBandNotch};
  eq.shape[26] = {11000.0f, 1.9f, kBandHighShelf};
  eq.shape[2] = {28.0f, 1.9f, kBandHighPass};
  for (int c = 0; c < kMaxChannels; ++c)
  {
    eq.bandDb[c][4] = 5.0f;
    eq.bandDb[c][10] = -3.5f;
    eq.bandDb[c][18] = 6.0f;
    eq.bandDb[c][26] = -2.5f;
  }
  const std::string text = FormatApoText(eq, -1, 48000);
  CHECK(text.find("Preamp: -4.5 dB") != std::string::npos);
  CHECK(text.find("ON LSC") != std::string::npos);
  CHECK(text.find("ON NO") != std::string::npos);
  CHECK(text.find("ON HPQ") != std::string::npos);
  // flat bells are not exported, and the filters come out in frequency order
  size_t filters = 0;
  for (size_t p = text.find("Filter "); p != std::string::npos; p = text.find("Filter ", p + 1))
    ++filters;
  CHECK(filters == 6);
  CHECK(text.find("Filter 1: ON HPQ Fc 28.00") != std::string::npos);

  EqParams back;
  EqImportResult r;
  REQUIRE(ParseApoText(text, 48000, back, &r));
  CHECK(r.imported == 6);
  double worst = 0.0;
  for (int i = 0; i < 160; ++i)
  {
    const double hz = 20.0 * std::pow(1000.0, i / 159.0);
    worst = std::max(worst, std::fabs(Resp(eq, 0, hz) - Resp(back, 0, hz)));
  }
  INFO("worst difference " << worst << " dB");
  CHECK(worst < 0.08);
  CHECK_NEAR(back.preampDb, -4.5, 1e-4);
}

// ---------------------------------------------------------------------------------------------------------
TEST_CASE("auto profiles: file format round trip, tolerant of garbage")
{
  AutoProfileConfig cfg;
  cfg.enabled = true;
  cfg.holdSeconds = 5.0;
  cfg.defaultDsp = "Плоский \"базовый\"";
  cfg.rules.push_back({"vlc.exe", "Кино", "Movie"});
  cfg.rules.push_back({"foobar2000.exe", "Music-V", ""});
  const std::string text = FormatAutoProfiles(cfg);
  AutoProfileConfig back;
  std::string err;
  REQUIRE(ParseAutoProfiles(text, back, &err));
  CHECK(back.enabled);
  CHECK(back.holdSeconds == 5.0);
  CHECK(back.defaultDsp == cfg.defaultDsp);                     // Cyrillic and quotes survive
  REQUIRE(back.rules.size() == 2);
  CHECK(back.rules[0].dsp == "Кино");
  CHECK(back.rules[0].upmix == "Movie");
  CHECK(back.rules[1].upmix.empty());

  AutoProfileConfig empty;
  REQUIRE(ParseAutoProfiles(FormatAutoProfiles(empty), empty, &err));
  CHECK_FALSE(empty.enabled);
  CHECK(empty.rules.empty());
  AutoProfileConfig clamp;
  REQUIRE(ParseAutoProfiles("{\"enabled\": true, \"hold_seconds\": 999, \"rules\": [{\"process\": \"\"}, {\"process\": \"a.exe\"}, 5]}", clamp, &err));
  CHECK(clamp.holdSeconds == 60.0);
  CHECK(clamp.rules.size() == 1);                               // a rule without a process is dropped
  CHECK_FALSE(ParseAutoProfiles("{ nope", clamp, &err));
  CHECK(ProcessStem("C:\\Program Files\\VLC\\VLC.EXE") == "vlc");
  CHECK(ProcessStem("Foobar2000") == "foobar2000");
}

TEST_CASE("auto profiles: the first matching rule wins, a change must hold, silence keeps the profile")
{
  AutoProfileConfig cfg;
  cfg.enabled = true;
  cfg.holdSeconds = 3.0;
  cfg.rules.push_back({"vlc", "Кино", "Movie"});
  cfg.rules.push_back({"foobar2000.exe", "Music-V", "Music"});
  AutoProfileSelector sel;
  int rule = -9;

  CHECK_FALSE(sel.Update(cfg, {"VLC.EXE"}, 0.0, &rule));        // candidate seen, not held yet
  CHECK_FALSE(sel.Update(cfg, {"vlc.exe"}, 2.0, &rule));
  CHECK(sel.Update(cfg, {"vlc.exe"}, 3.1, &rule));
  CHECK(rule == 0);
  CHECK_FALSE(sel.Update(cfg, {"vlc.exe"}, 9.0, &rule));        // already applied: nothing again, a manual choice stays
  CHECK_FALSE(sel.Update(cfg, {}, 20.0, &rule));                // silence: keep
  CHECK_FALSE(sel.Update(cfg, {}, 60.0, &rule));
  CHECK(sel.Current() == 0);

  CHECK_FALSE(sel.Update(cfg, {"foobar2000.exe"}, 100.0, &rule));
  CHECK_FALSE(sel.Update(cfg, {"vlc.exe"}, 101.0, &rule));      // flapping back before the hold: the timer restarts
  CHECK_FALSE(sel.Update(cfg, {"foobar2000.exe"}, 102.0, &rule));
  CHECK_FALSE(sel.Update(cfg, {"foobar2000.exe"}, 104.5, &rule));
  CHECK(sel.Update(cfg, {"foobar2000.exe"}, 105.2, &rule));
  CHECK(rule == 1);

  AutoProfileSelector both;
  CHECK_FALSE(both.Update(cfg, {"foobar2000.exe", "vlc.exe"}, 0.0, &rule));
  CHECK(both.Update(cfg, {"foobar2000.exe", "vlc.exe"}, 3.5, &rule));
  CHECK(rule == 0);                                             // rule order decides, not the order of the apps

  // something plays that no rule knows: nothing happens, unless there is a default
  AutoProfileSelector other;
  CHECK_FALSE(other.Update(cfg, {"chrome.exe"}, 0.0, &rule));
  CHECK_FALSE(other.Update(cfg, {"chrome.exe"}, 10.0, &rule));
  cfg.defaultDsp = "Плоский";
  CHECK_FALSE(other.Update(cfg, {"chrome.exe"}, 20.0, &rule));
  CHECK(other.Update(cfg, {"chrome.exe"}, 23.5, &rule));
  CHECK(rule == AutoProfileSelector::kDefault);

  cfg.enabled = false;                                          // switched off: never
  AutoProfileSelector off;
  CHECK_FALSE(off.Update(cfg, {"vlc.exe"}, 0.0, &rule));
  CHECK_FALSE(off.Update(cfg, {"vlc.exe"}, 50.0, &rule));
}

TEST_CASE("auto profiles: composing the settings keeps the user's upmix tuning unless told otherwise")
{
  DspParams current;
  current.enabled = 1;
  current.upmix.centerFocus = 0.9f;
  current.upmix.surroundDelayMs = 25.0f;
  current.upmix.enabled = 1;

  DspParams withEq;                                  // a DSP profile made before the upmix existed: no "upmix" section
  withEq.enabled = 1;
  withEq.eq.preampDb = -4.0f;
  std::string profileText = FormatSettings(withEq);
  const size_t cut = profileText.find("  \"upmix\"");
  REQUIRE(cut != std::string::npos);
  std::string old = profileText.substr(0, cut);       // drop the upmix block ...
  old.erase(old.find_last_of(','));                   // ... and the comma that led to it
  old += "}\n";
  DspParams next;
  std::string err;
  REQUIRE(ComposeAutoSettings(current, true, old, false, "", next, &err));
  CHECK_NEAR(next.eq.preampDb, -4.0, 1e-3);          // the profile's equalizer is applied
  CHECK_NEAR(next.upmix.centerFocus, 0.9, 1e-4);     // the user's upmix tuning survives
  CHECK_NEAR(next.upmix.surroundDelayMs, 25.0, 1e-4);

  REQUIRE(ComposeAutoSettings(current, true, FormatSettings(withEq), false, "", next, &err));   // a profile WITH its own section wins
  CHECK_NEAR(next.upmix.centerFocus, 0.5, 1e-4);

  UpmixParams movie;
  movie.centerWidth = 0.6f;
  movie.surroundDelayMs = 12.0f;
  current.upmix.enabled = 0;
  REQUIRE(ComposeAutoSettings(current, false, "", true, FormatUpmixPreset(movie), next, &err));
  CHECK_NEAR(next.upmix.centerWidth, 0.6, 1e-4);     // the preset's values ...
  CHECK(next.upmix.enabled == 0);                    // ... but the on/off switch stays
  CHECK(next.enabled == 1);                          // and the rest of the settings are untouched

  CHECK_FALSE(ComposeAutoSettings(current, true, "{ broken", false, "", next, &err));
  CHECK(HasUpmixSection(FormatSettings(current)));
  CHECK_FALSE(HasUpmixSection("{ \"enabled\": true }"));
}

TEST_CASE("meter block: UTF-8 names are copied without cutting a character in half")
{
  char buf[11];                                         // room for 10 bytes plus the terminator
  CopyUtf8(buf, sizeof buf, "vlc.exe");
  CHECK(std::string(buf) == "vlc.exe");
  CopyUtf8(buf, sizeof buf, "Кино-профиль-длинный");   // "Кино-" is 9 bytes; the next letter (2 bytes) would not fit whole
  const std::string cut = buf;
  CHECK(cut.size() == 9);
  CHECK(cut == "Кино-");
  for (size_t i = cut.size(); i < sizeof buf; ++i)
    CHECK(buf[i] == '\0');
  CopyUtf8(buf, sizeof buf, "");
  CHECK(buf[0] == '\0');
}
