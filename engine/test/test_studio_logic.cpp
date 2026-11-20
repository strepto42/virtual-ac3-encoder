// test_studio_logic.cpp — the GUI-free parts of DSP Studio: shared-memory meter block, meter
// ballistics, the editing model, curve maths.
#include "doctest.h"
#include "dsp/MeterMath.h"
#include "dsp/MeterShm.h"
#include "dsp/StudioModel.h"

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

TEST_CASE("meter block: nothing published -> not readable; published values come back intact")
{
  DspMeterBlock shared;           // stands in for the shared-memory view
  MeterReader reader(&shared);
  DspMeterBlock got;
  CHECK_FALSE(reader.Read(got));  // magic missing

  MeterWriter writer(&shared);
  DspMeterBlock v;
  v.sampleRate = 48000;
  v.channels = 6;
  v.dspOn = 1;
  for (int c = 0; c < kMaxChannels; ++c)
  {
    v.inPeak[c] = 0.1f * static_cast<float>(c + 1);
    v.outPeak[c] = 0.05f * static_cast<float>(c + 1);
  }
  v.compReductionDb = 3.5f;
  v.limiterReductionDb = 1.25f;
  writer.Publish(v);
  REQUIRE(reader.Read(got));
  CHECK(got.updates == 1u);
  CHECK(got.sampleRate == 48000u);
  CHECK(got.dspOn == 1);
  CHECK(got.inPeak[2] == v.inPeak[2]);
  CHECK(got.outPeak[5] == v.outPeak[5]);
  CHECK(got.compReductionDb == 3.5f);
  CHECK(got.limiterReductionDb == 1.25f);
  writer.Publish(v);
  REQUIRE(reader.Read(got));
  CHECK(got.updates == 2u);        // the counter shows the engine is alive
  CHECK((shared.seq & 1u) == 0u);  // and the block is left readable
}

TEST_CASE("meter block: a reader never sees a torn block while the writer publishes")
{
  DspMeterBlock shared;
  MeterWriter writer(&shared);
  MeterReader reader(&shared);
  std::atomic<bool> stop{false};
  std::thread w([&] {
    DspMeterBlock v;
    float x = 1.0f;
    unsigned writes = 0;
    while (!stop.load())
    {
      for (int c = 0; c < kMaxChannels; ++c)
        v.inPeak[c] = v.outPeak[c] = x;
      v.compReductionDb = v.limiterReductionDb = x;
      writer.Publish(v);
      x += 1.0f;
      // Let the reader run regularly even on heavily contended CI runners.
      if (++writes % 64 == 0)
        std::this_thread::yield();
    }
  });
  bool torn = false;
  int reads = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while ((reads < 200000) && std::chrono::steady_clock::now() < deadline)
  {
    DspMeterBlock got;
    if (reader.Read(got))
    {
      ++reads;
      for (int c = 0; c < kMaxChannels; ++c)
        if (got.inPeak[c] != got.inPeak[0] || got.outPeak[c] != got.inPeak[0])
          torn = true;
      if (got.compReductionDb != got.inPeak[0] || got.limiterReductionDb != got.inPeak[0])
        torn = true;
    }
  }
  stop = true;
  w.join();
  CHECK_FALSE(torn);
  CHECK(reads > 1000);
}

TEST_CASE("meter math: dB conversion and ballistics")
{
  CHECK(PeakToDb(1.0f) == doctest::Approx(0.0f));
  CHECK_NEAR(PeakToDb(0.5f), -6.0206, 0.001);
  CHECK(PeakToDb(0.0f) == kMeterFloorDb);

  MeterBallistics m;
  m.Reset();
  CHECK(m.level() == kMeterFloorDb);
  m.Update(-6.0f, 0.033f);
  CHECK(m.level() == -6.0f);                // instant attack
  CHECK(m.peak() == -6.0f);
  m.Update(kMeterFloorDb, 0.5f);
  CHECK_NEAR(m.level(), -6.0f - 12.0f, 0.01);   // falls 24 dB/s
  CHECK(m.peak() == -6.0f);                 // held
  for (int i = 0; i < 25; ++i)              // 0.5 s + 0.83 s = 1.33 s after the peak: still inside the 1.5 s hold
    m.Update(kMeterFloorDb, 0.033f);
  CHECK(m.peak() == -6.0f);
  for (int i = 0; i < 60; ++i)              // hold over: the marker falls
    m.Update(kMeterFloorDb, 0.033f);
  CHECK(m.peak() < -10.0f);
  for (int i = 0; i < 1000; ++i)
    m.Update(kMeterFloorDb, 0.033f);
  CHECK(m.level() == kMeterFloorDb);
  CHECK(m.peak() == kMeterFloorDb);
}

TEST_CASE("studio model: linked edits move every channel, channel edits only one; values are clamped")
{
  StudioModel m;
  m.SetBand(10, 6.04f);
  for (int c = 0; c < kMaxChannels; ++c)
    CHECK(m.params.eq.bandDb[c][10] == 6.0f);   // rounded to 0.1 dB
  CHECK(m.Band(10) == 6.0f);
  CHECK_FALSE(m.ChannelsDiffer());

  m.channel = 2;                                // centre only
  m.SetBand(10, -3.0f);
  CHECK(m.params.eq.bandDb[2][10] == -3.0f);
  CHECK(m.params.eq.bandDb[0][10] == 6.0f);
  CHECK(m.Band(10) == -3.0f);                   // the shown curve is the centre's
  CHECK(m.ChannelsDiffer());

  m.channel = kAllChannels;
  CHECK(m.Band(10) == 6.0f);                    // linked view shows the front-left curve
  m.SetBand(11, 50.0f);                         // clamped to +12 on every channel
  CHECK(m.params.eq.bandDb[4][11] == kEqBandMaxDb);
  CHECK(m.params.eq.bandDb[2][10] == -3.0f);    // the other bands of channel C stay as they were

  m.ResetEq();
  CHECK_FALSE(m.ChannelsDiffer());
  CHECK(m.Band(10) == 0.0f);
  CHECK(m.params.eq.preampDb == 0.0f);
}

TEST_CASE("studio model: the drawn curve matches the real filter, and auto-preamp cancels the boost")
{
  StudioModel m;
  CHECK(BandLabel(0) == "20");
  CHECK(BandLabel(16) == "904");
  CHECK(BandLabel(17) == "1.1k");
  CHECK(BandLabel(29) == "20k");
  CHECK_NEAR(CurveFrequency(0), 20.0, 1e-9);
  CHECK_NEAR(CurveFrequency(kCurvePoints - 1), 20000.0, 1e-6);

  double flat[kCurvePoints];
  m.Curve(48000, flat);
  for (double v : flat)
    CHECK(v == 0.0);
  CHECK(m.AutoPreampDb(48000) == 0.0f);

  for (int b = 0; b < kBands; ++b)
    m.SetBand(b, 6.0f);
  double curve[kCurvePoints];
  m.Curve(48000, curve);
  double peak = 0.0;
  for (int i = 0; i < kCurvePoints; ++i)
  {
    peak = std::max(peak, curve[i]);
    CHECK_NEAR(curve[i], GraphicEq::ResponseDb(m.params.eq.bandDb[0], CurveFrequency(i), 48000), 1e-9);
  }
  CHECK(peak > 8.0);                            // neighbouring bands overlap: much more than +6 dB
  const float pre = m.AutoPreampDb(48000);
  CHECK(pre <= -peak);                          // cancels the peak ...
  CHECK(pre > -peak - 0.51f);                   // ... rounded up to the next 0.5 dB only
  CHECK(std::fmod(std::fabs(static_cast<double>(pre)) * 2.0, 1.0) == 0.0);

  StudioModel cut;                              // cuts only: no preamp needed
  for (int b = 0; b < kBands; ++b)
    cut.SetBand(b, -4.0f);
  CHECK(cut.AutoPreampDb(48000) == 0.0f);
}

TEST_CASE("meter block v2: spectra travel intact, and publishing never touches the word Studio writes")
{
  DspSharedLayout shared;
  MeterWriter writer(&shared.meters);
  MeterReader reader(&shared.meters);
  shared.studioBeat = 41u;
  DspMeterBlock v;
  ClearSpectra(v);
  v.spectrumValid = 1;
  v.specIn[2][17] = -12.5f;
  v.specOut[5][119] = -3.25f;
  writer.Publish(v);
  DspMeterBlock got;
  REQUIRE(reader.Read(got));
  CHECK(got.version == kMeterVersion);
  CHECK(got.spectrumValid == 1);
  CHECK(got.specIn[2][17] == -12.5f);
  CHECK(got.specOut[5][119] == -3.25f);
  CHECK(got.specIn[0][0] == kSpecFloorDb);
  CHECK(shared.studioBeat == 41u);               // the engine never overwrites the Studio word
  shared.studioBeat = 42u;
  writer.Publish(v);
  CHECK(shared.studioBeat == 42u);
}

#include "dsp/UndoHistory.h"

TEST_CASE("undo history: steps, redo, redo cleared by a new edit, limit, rebase")
{
  UndoHistory<int> h([](const int& a, const int& b) { return a == b; }, 3);
  h.Reset(0);
  int cur = 0;
  CHECK_FALSE(h.CanUndo());
  CHECK_FALSE(h.Undo(cur));

  cur = 1;
  CHECK(h.Commit(cur));
  CHECK_FALSE(h.Commit(cur));        // nothing changed: no step
  cur = 2;
  CHECK(h.Commit(cur));
  CHECK(h.Undo(cur));
  CHECK(cur == 1);
  CHECK(h.Undo(cur));
  CHECK(cur == 0);
  CHECK_FALSE(h.Undo(cur));
  CHECK(h.Redo(cur));
  CHECK(cur == 1);
  CHECK(h.Redo(cur));
  CHECK(cur == 2);
  CHECK_FALSE(h.Redo(cur));

  CHECK(h.Undo(cur));                // back to 1
  cur = 5;                           // a new edit after an undo ...
  CHECK(h.Commit(cur));
  CHECK_FALSE(h.CanRedo());          // ... drops the redo branch
  CHECK(h.Undo(cur));
  CHECK(cur == 1);

  h.Reset(10);                       // limit 3: only the last three steps are kept
  cur = 10;
  for (int v = 11; v <= 16; ++v)
  {
    cur = v;
    h.Commit(cur);
  }
  CHECK(h.UndoDepth() == 3u);
  int steps = 0;
  while (h.Undo(cur))
    ++steps;
  CHECK(steps == 3);
  CHECK(cur == 13);

  h.Reset(20);
  cur = 20;
  h.Rebase(30);                      // an outside change is not an edit
  cur = 30;
  CHECK_FALSE(h.Commit(cur));
  cur = 31;
  CHECK(h.Commit(cur));
  CHECK(h.Undo(cur));
  CHECK(cur == 30);                  // undo goes back to the rebased state, not to 20
}

TEST_CASE("undo history with DspParams: one gesture = one step, float noise is not an edit")
{
  UndoHistory<DspParams> h(&SameParams);
  StudioModel m;
  h.Reset(m.params);
  for (int i = 1; i <= 40; ++i)      // a drag: many tiny moves
    m.SetBand(7, static_cast<float>(i) * 0.1f);
  CHECK(h.Commit(m.params));
  CHECK(h.UndoDepth() == 1u);
  m.params.eq.preampDb += 1e-7f;     // below the precision the file keeps
  CHECK_FALSE(h.Commit(m.params));
  m.SetType(7, kBandHighPass);
  CHECK(h.Commit(m.params));
  CHECK(h.Undo(m.params));
  CHECK(m.Shape(7).type == kBandBell);
  CHECK(m.Band(7) == 4.0f);
  CHECK(h.Undo(m.params));
  CHECK(m.Band(7) == 0.0f);
  CHECK(h.Redo(m.params));
  CHECK(m.Band(7) == 4.0f);
}

