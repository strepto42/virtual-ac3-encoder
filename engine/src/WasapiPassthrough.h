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
#include "MeterSharedState.h"
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
    int64_t  dtsBitRate = 1536000;
    uint32_t safeFrames = 1536;    // target excess frames kept buffered (latency vs. safety)
    SpdifEncoder::Upmix upmix = SpdifEncoder::Upmix::Off;
    bool     upmixSurround = false; // legacy compatibility flag (mapped to upmix if true)
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

private:
  bool InitExclusive(int rate);
  bool EncodeIntoBuffer(BYTE* out); // fills one full WASAPI buffer with bursts (+ stuffing); returns true if non-underrun
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
  bool     primed_ = false;

  std::vector<uint8_t> staging_; // one packet of capture frames
  std::vector<uint8_t> silence_; // same, zeroed
  std::vector<uint8_t> burst_;   // one IEC 61937 burst
  MeterSharedMemory    sharedMem_;
};
