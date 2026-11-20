// WasapiPassthrough.h
//
// The output / master-clock side of the engine. Opens the optical endpoint in EXCLUSIVE,
// event-driven IEC 61937 (Dolby Digital) mode and, on each render event, pulls PCM from the
// shared RingBuffer, encodes it to an AC3 / IEC 61937 burst (SpdifEncoder) and writes it to
// the device. Clock drift between the capture clock and this output clock is absorbed by the
// ring buffer and corrected SoundPusher-style: every ~64 cycles, excess buffered frames are
// trimmed to bound latency; underruns emit AC3 silence so the receiver stays locked.
#pragma once

#include "ComUtil.h"
#include "RingBuffer.h"
#include "SpdifEncoder.h"
#include "WasapiCapture.h" // CaptureFormat
#include <array>

#include <audioclient.h>
#include <mmdeviceapi.h>

#include <atomic>
#include <thread>
#include <vector>

class WasapiPassthrough
{
public:
  struct Params
  {
    SpdifEncoder::Codec codec = SpdifEncoder::Codec::AC3;
    int64_t  bitRate = 640000;
    uint32_t safeFrames = 1536;    // target excess frames kept buffered (latency vs. safety)
    bool     upmixSurround = false; // stereo->5.1 via the `surround` filter (else swr default)
    bool     upmixMatrix = false;   // do the upmix with the time-domain matrix (DspChain) instead of the filter
    int      upmixWinSize = 0;      // surround-upmix FFT window (0 = FFmpeg default 4096)
    std::string dumpPath;           // diagnostics: record input / encoder input to WAV (see Config::dumpAudio)
    bool     dtsIn = false;         // decode a DTS bitstream found in the input (Dolby Digital output only)
    bool     upmixAuto = false;     // multichannel input: upmix FL/FR when C/SL/SR are silent
    bool     upmixAlways = false;   // multichannel input: always upmix FL/FR
    float    headroomDb = -6.0f;    // fixed trim before the encoder
    bool     limiter = true;        // linked look-ahead peak limiter before the encoder
    float    limiterCeilingDb = -1.0f;
  };

  WasapiPassthrough() = default;
  ~WasapiPassthrough();

  // Non-intrusive capability check: does `dev` accept AC3 / IEC 61937 in exclusive mode at
  // `rate`? Uses IsFormatSupported only (does not seize the device).
  static bool ProbeAc3(IMMDevice* dev, int rate);
  static bool ProbeDts(IMMDevice* dev, int rate);

  // dev    : optical output endpoint (must support AC3 passthrough in exclusive mode)
  // ring   : shared input ring (filled by WasapiCapture), holding capFmt frames
  // capFmt : capture format — defines the encoder's input (channels/layout/sample fmt/rate)
  bool Init(IMMDevice* dev, RingBuffer* ring, const CaptureFormat& capFmt, const Params& p);
  bool Start();
  void Stop();
  // Thread-safe live trim for the six AC3 output channels, in dB.
  void SetChannelGainsDb(const std::array<float, 6>& gainsDb) { enc_.SetChannelGainsDb(gainsDb); }
  float TakeLimiterReductionDb() { return enc_.TakeLimiterReductionDb(); }
  // DSP chain (equalizer + compressor); thread-safe, see SpdifEncoder::SetDspParams().
  void SetDspParams(const vac3dsp::DspParams& p) { enc_.SetDspParams(p); }
  vac3dsp::DspMeters TakeDspMeters() { return enc_.TakeDspMeters(); }
  vac3dsp::DspChain& DspChainRef() { return enc_.DspChainRef(); }
  void TakeFinalPeaks(float* out) { enc_.TakeFinalPeaks(out); }
  // Diagnostics since the previous call (see the [stats] line in engine.log).
  uint32_t TakeInputFullScalePackets() { return enc_.TakeInputFullScalePackets(); }
  uint32_t TakeUnderruns() { return underruns_.exchange(0, std::memory_order_relaxed); }
  uint32_t TakeTrims() { return trims_.exchange(0, std::memory_order_relaxed); }
  uint32_t TakeTrimmedFrames() { return trimmedFrames_.exchange(0, std::memory_order_relaxed); }

private:
  bool InitExclusive(int rate);
  void EncodeIntoBuffer(BYTE* out); // fills one full WASAPI buffer with bursts (+ stuffing)
  void ThreadProc();

  ComPtr<IMMDevice>          dev_;
  ComPtr<IAudioClient>       client_;
  ComPtr<IAudioRenderClient> render_;

  RingBuffer*    ring_ = nullptr;
  CaptureFormat  capFmt_;
  size_t         capBytesPerFrame_ = 0;
  Params         params_;

  SpdifEncoder   enc_;
  int            framesPerPacket_ = 1536;
  size_t         burstBytes_ = 0;
  static constexpr size_t kMaxBurstBytes = SpdifEncoder::kMaxBytesPerPacket; // AC3 capacity; also fits DTS
  static constexpr int kCarrierBytesPerFrame = 4; // 2ch * 16-bit IEC60958

  UINT32 bufferFrames_ = 0; // exclusive buffer size, in carrier frames
  int    burstsPerCycle_ = 0;

  HANDLE dataEvent_ = nullptr;
  HANDLE stopEvent_ = nullptr;
  std::thread thread_;
  std::atomic_bool running_{false};

  // drift tracking (consumer thread only)
  uint32_t cycle_ = 0;
  uint32_t minAvail_ = 0xFFFFFFFFu;
  std::atomic<uint32_t> underruns_{0};      // silent AC3/DTS packets emitted because the ring ran dry
  std::atomic<uint32_t> trims_{0};          // drift corrections (frames discarded from the ring)
  std::atomic<uint32_t> trimmedFrames_{0};

  std::vector<uint8_t> staging_; // one packet of capture frames
  std::vector<uint8_t> silence_; // same, zeroed
  std::vector<uint8_t> burst_;   // one IEC 61937 burst
};
