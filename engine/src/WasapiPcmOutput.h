// WasapiPcmOutput.h — normal 2-channel PCM output for S/PDIF / Toslink.
#pragma once

#include "ComUtil.h"
#include "RingBuffer.h"
#include "WasapiCapture.h"

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

// Unlike WasapiPassthrough (AC-3 encoded as IEC 61937), this class writes ordinary
// stereo PCM through the Windows shared audio engine. The S/PDIF receiver should
// therefore report PCM / Stereo rather than Dolby Digital.
class WasapiPcmOutput
{
public:
  WasapiPcmOutput();
  ~WasapiPcmOutput();

  bool Init(IMMDevice* dev, RingBuffer* ring, const CaptureFormat& capFmt,
            uint32_t safeFrames = 1536);
  bool Start();
  void Stop();

  // Reuse the GUI's six trims. In PCM mode only FL and FR (indices 0 and 1)
  // apply; the four surround-channel controls are disabled in the UI.
  void SetChannelGainsDb(const std::array<float, 6>& gainsDb);

private:
  void ThreadProc();
  void ApplyStereoGains(uint8_t* data, uint32_t frames);

  ComPtr<IMMDevice> dev_;
  ComPtr<IAudioClient> client_;
  ComPtr<IAudioRenderClient> render_;
  RingBuffer* ring_ = nullptr;
  CaptureFormat capFmt_{};
  size_t capBytesPerFrame_ = 0;
  size_t outBytesPerFrame_ = 0;
  unsigned outRate_ = 0;
  unsigned outBits_ = 0;
  AVSampleFormat inSampleFmt_ = AV_SAMPLE_FMT_NONE;
  AVSampleFormat outSampleFmt_ = AV_SAMPLE_FMT_NONE;
  SwrContext* swr_ = nullptr;
  uint32_t safeFrames_ = 1536;
  UINT32 bufferFrames_ = 0;
  HANDLE stopEvent_ = nullptr;
  std::thread thread_;
  std::atomic_bool running_{false};
  std::array<std::atomic<float>, 2> gainsLinear_;
  std::vector<uint8_t> inputStaging_;
  uint32_t cycle_ = 0;
};
