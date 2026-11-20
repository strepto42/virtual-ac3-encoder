// DspChain.h — the DSP chain that sits in front of the AC3/DTS encoder: EQ and compressor (in a
// selectable order) with a click-free master/stage bypass and level meters.
//
//   control thread:  SetParams()            (any thread; sanitised; never blocks the audio thread)
//   audio thread:    Process()              (in place, planar float, <= 6 channels, never allocates)
//   any thread:      TakeMeters()
//
// The look-ahead limiter that already exists at the end of the encoder path stays the last stage, so
// whatever EQ boost or makeup gain does, nothing above the ceiling reaches the encoder.
#pragma once

#include "Compressor.h"
#include "DspParams.h"
#include "GraphicEq.h"
#include "Mailbox.h"
#include "MatrixUpmix.h"
#include "Spectrum.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace vac3dsp {

struct DspMeters
{
  float inPeak[kMaxChannels] = {};   // linear peak since the previous TakeMeters()
  float outPeak[kMaxChannels] = {};
  float compReductionDb = 0.0f;      // deepest compressor gain reduction (>= 0)
};

class DspChain
{
public:
  static constexpr int kChunk = GraphicEq::kSub;  // chain works in chunks of at most this many frames

  void Configure(int channels, int sampleRate)
  {
    channels_ = std::min(std::max(channels, 1), kMaxChannels);
    rate_ = std::max(8000, sampleRate);
    fadeStep_ = 1.0f / (0.010f * static_cast<float>(rate_));  // 10 ms bypass crossfade
    eq_.Configure(channels_, rate_);
    comp_.Configure(channels_, rate_);
    upmix_.Configure(rate_);
    upmixOn_ = false;
    tapIn_.Configure(channels_);
    tapOut_.Configure(channels_);
    active_ = DspParams{};
    eqStage_ = Stage{};
    compStage_ = Stage{};
    ApplyActive();
    mailbox_.Repost();  // whatever SetParams() last asked for is picked up again by the next Process()
    for (int c = 0; c < kMaxChannels; ++c)
    {
      inPk_[c].store(0.0f, std::memory_order_relaxed);
      outPk_[c].store(0.0f, std::memory_order_relaxed);
    }
  }

  // Thread-safe (any thread, also several writers). The new values reach the audio thread at the start
  // of its next Process() call; the audio thread never waits for anything (lock-free triple buffer,
  // no mutex, no allocation).
  void SetParams(const DspParams& p)
  {
    DspParams s = p;
    Sanitize(s);
    mailbox_.Post(s);
  }

  // Audio thread: take over parameters queued by SetParams() (Process() does this itself).
  void PullParams() { PullPending(); }

  // Audio thread: true when Process() would leave the audio untouched and nothing is fading.
  // Call PullParams() first if the answer must reflect the latest SetParams().
  bool Bypassed() const
  {
    const bool eqOff = active_.enabled == 0 || active_.eq.enabled == 0;
    const bool compOff = active_.enabled == 0 || active_.comp.enabled == 0;
    return (eqOff && eqStage_.mix == 0.0f) && (compOff && compStage_.mix == 0.0f);
  }

  // Stereo -> 5.1 matrix upmix in front of the EQ (engine option upmix_algo=matrix): it works on the 6
  // planes of the encoder frame, whatever the EQ / compressor master switch says. `force` = "always"
  // (otherwise it engages by itself when the centre and surround planes are silent, i.e. stereo material).
  // Call after Configure(), before audio runs.
  void ConfigureUpmix(bool active, bool force)
  {
    upmixOn_ = active && channels_ >= 6;
    upmix_.SetForce(force);
    upmix_.Reset();
  }
  bool UpmixActive() const { return upmixOn_; }

  // Spectrum taps (for DSP Studio): while on, every chunk is copied into a ring before and after the chain.
  void SetSpectrumTap(bool on) { tapOn_.store(on, std::memory_order_relaxed); }
  const SpectrumTap& TapIn() const { return tapIn_; }
  const SpectrumTap& TapOut() const { return tapOut_; }

  void Process(float* const* planes, int frames)
  {
    if (channels_ <= 0 || frames <= 0)
      return;
    PullPending();
    const bool tap = tapOn_.load(std::memory_order_relaxed);

    float inMax[kMaxChannels] = {}, outMax[kMaxChannels] = {};
    for (int done = 0; done < frames; done += kChunk)
    {
      const int n = std::min(kChunk, frames - done);
      float* p[kMaxChannels];
      for (int c = 0; c < channels_; ++c)
        p[c] = planes[c] + done;
      if (upmixOn_)
        upmix_.Process(p, n);
      for (int c = 0; c < channels_; ++c)
        for (int i = 0; i < n; ++i)
          inMax[c] = std::max(inMax[c], std::fabs(p[c][i]));

      if (tap)
        tapIn_.Push(p, n);

      const bool eqWant = active_.enabled != 0 && active_.eq.enabled != 0;
      const bool compWant = active_.enabled != 0 && active_.comp.enabled != 0;
      if (active_.order == 0)
      {
        RunStage(eqStage_, eqWant, eq_, p, n);
        RunStage(compStage_, compWant, comp_, p, n);
      }
      else
      {
        RunStage(compStage_, compWant, comp_, p, n);
        RunStage(eqStage_, eqWant, eq_, p, n);
      }

      for (int c = 0; c < channels_; ++c)
        for (int i = 0; i < n; ++i)
          outMax[c] = std::max(outMax[c], std::fabs(p[c][i]));
      if (tap)
        tapOut_.Push(p, n);
    }

    for (int c = 0; c < channels_; ++c)
    {
      if (inMax[c] > inPk_[c].load(std::memory_order_relaxed))
        inPk_[c].store(inMax[c], std::memory_order_relaxed);
      if (outMax[c] > outPk_[c].load(std::memory_order_relaxed))
        outPk_[c].store(outMax[c], std::memory_order_relaxed);
    }
  }

  DspMeters TakeMeters()
  {
    DspMeters m;
    for (int c = 0; c < kMaxChannels; ++c)
    {
      m.inPeak[c] = inPk_[c].exchange(0.0f, std::memory_order_relaxed);
      m.outPeak[c] = outPk_[c].exchange(0.0f, std::memory_order_relaxed);
    }
    m.compReductionDb = comp_.TakeMaxReductionDb();
    return m;
  }

private:
  struct Stage
  {
    float mix = 0.0f;  // 0 = fully bypassed, 1 = fully processed
  };

  // Audio thread: if a writer published something newer, take it over (one atomic exchange).
  void PullPending()
  {
    if (const DspParams* fresh = mailbox_.Take())
    {
      active_ = *fresh;
      ApplyActive();
    }
  }

  void ApplyActive()
  {
    eq_.SetParams(active_.eq);
    comp_.SetParams(active_.comp);
    upmix_.SetParams(active_.upmix);
  }

  // Runs one stage on n <= kChunk frames. A stage that is fully off costs nothing; a stage that is
  // being switched crossfades dry/wet over 10 ms. A stage that has been off starts from clean state.
  template <class Module>
  void RunStage(Stage& st, bool want, Module& mod, float* const* p, int n)
  {
    const float target = want ? 1.0f : 0.0f;
    if (st.mix == 0.0f && target == 0.0f)
      return;
    if (st.mix == 0.0f)
      mod.Reset();
    if (st.mix == 1.0f && target == 1.0f)
    {
      mod.Process(p, n);
      return;
    }

    float dry[kMaxChannels][kChunk];
    for (int c = 0; c < channels_; ++c)
      std::copy(p[c], p[c] + n, dry[c]);
    mod.Process(p, n);

    float m = st.mix;
    for (int i = 0; i < n; ++i)
    {
      if (m < target)
        m = std::min(target, m + fadeStep_);
      else if (m > target)
        m = std::max(target, m - fadeStep_);
      for (int c = 0; c < channels_; ++c)
        p[c][i] = dry[c][i] + (p[c][i] - dry[c][i]) * m;
    }
    st.mix = m;
  }

  MatrixUpmix upmix_;
  bool upmixOn_ = false;
  SpectrumTap tapIn_, tapOut_;
  std::atomic<bool> tapOn_{false};
  int channels_ = 0;
  int rate_ = 48000;
  float fadeStep_ = 0.002f;

  GraphicEq eq_;
  LinkedCompressor comp_;
  Stage eqStage_, compStage_;
  DspParams active_;   // audio thread only

  Mailbox<DspParams> mailbox_;  // SetParams() (any thread) -> Process() (audio thread), lock-free

  std::atomic<float> inPk_[kMaxChannels] = {};
  std::atomic<float> outPk_[kMaxChannels] = {};
};

}  // namespace vac3dsp
