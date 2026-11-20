// WasapiCapture.h
//
// Captures PCM from a WASAPI endpoint (the virtual cable's recording side) in shared,
// event-driven mode and pushes interleaved samples into a RingBuffer. Runs on its own
// thread; this is the producer / input-clock side of the pipeline.
#pragma once

#include "ComUtil.h"
#include "RingBuffer.h"

#include <audioclient.h>
#include <mmdeviceapi.h>

#include <atomic>
#include <thread>

struct CaptureFormat
{
  unsigned channels = 0;
  unsigned sampleRate = 0;
  unsigned bits = 0;       // bits per sample in the captured stream
  bool     isFloat = false;
  uint32_t channelMask = 0; // WASAPI speaker mask (0 if unknown)

  size_t bytesPerFrame() const { return static_cast<size_t>(channels) * (bits / 8); }
};

class WasapiCapture
{
public:
  WasapiCapture() = default;
  ~WasapiCapture();

  // Opens `dev` for shared capture and learns its mix format.
  //   loopback == false: a real capture endpoint, event-driven.
  //   loopback == true : a RENDER endpoint captured via AUDCLNT_STREAMFLAGS_LOOPBACK (polled;
  //                      loopback streams don't support event-driven mode). This is how the
  //                      engine reads the virtual 5.1 sink's mix.
  bool Init(IMMDevice* dev, bool loopback = false);
  // Destination ring for captured samples (set before Start, once it can be sized).
  void SetRing(RingBuffer* ring) { ring_ = ring; }
  bool Start();
  void Stop();

  const CaptureFormat& Format() const { return fmt_; }

  // Diagnostics since the previous call. A discontinuity flag is also raised once when playback
  // starts after silence; many per minute mean the capture buffer overran (lost audio).
  uint32_t TakeDiscontinuities() { return discontinuities_.exchange(0, std::memory_order_relaxed); }
  // Times the ring was full and captured audio had to be dropped.
  uint32_t TakeRingOverflows() { return ringOverflows_.exchange(0, std::memory_order_relaxed); }

private:
  void ThreadProc();

  RingBuffer*               ring_ = nullptr;
  ComPtr<IAudioClient>      client_;
  ComPtr<IAudioCaptureClient> capture_;
  CaptureFormat             fmt_;

  bool                      loopback_ = false;
  HANDLE                    dataEvent_ = nullptr;
  HANDLE                    stopEvent_ = nullptr;
  std::thread               thread_;
  std::atomic_bool          running_{false};
  std::atomic<uint32_t>     discontinuities_{0};
  std::atomic<uint32_t>     ringOverflows_{0};
};
