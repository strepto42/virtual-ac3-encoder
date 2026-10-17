// WasapiPcmOutput.cpp — shared-mode stereo PCM renderer.
#include "WasapiPcmOutput.h"

#include <mmreg.h>
#include <ksmedia.h>
#include <avrt.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace {

AVSampleFormat MapCaptureSampleFormat(const CaptureFormat& f)
{
  if (f.isFloat && f.bits == 32) return AV_SAMPLE_FMT_FLT;
  if (!f.isFloat && f.bits == 16) return AV_SAMPLE_FMT_S16;
  if (!f.isFloat && f.bits == 32) return AV_SAMPLE_FMT_S32;
  return AV_SAMPLE_FMT_NONE;
}

AVSampleFormat MapWaveSampleFormat(const WAVEFORMATEX* f)
{
  if (!f) return AV_SAMPLE_FMT_NONE;
  bool isFloat = false;
  bool isPcm = false;

  if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
    if (f->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
      return AV_SAMPLE_FMT_NONE;
    const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f);
    isFloat = IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != FALSE;
    isPcm = IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) != FALSE;
  } else {
    isFloat = f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    isPcm = f->wFormatTag == WAVE_FORMAT_PCM;
  }

  if (isFloat && f->wBitsPerSample == 32) return AV_SAMPLE_FMT_FLT;
  if (isPcm && f->wBitsPerSample == 16) return AV_SAMPLE_FMT_S16;
  if (isPcm && f->wBitsPerSample == 32) return AV_SAMPLE_FMT_S32;
  return AV_SAMPLE_FMT_NONE;
}

float DbToLinear(float db)
{
  db = std::clamp(db, -60.0f, 0.0f);
  return db <= -60.0f ? 0.0f : std::pow(10.0f, db / 20.0f);
}

} // namespace

WasapiPcmOutput::WasapiPcmOutput()
{
  gainsLinear_[0].store(1.0f, std::memory_order_relaxed);
  gainsLinear_[1].store(1.0f, std::memory_order_relaxed);
}

WasapiPcmOutput::~WasapiPcmOutput()
{
  Stop();
  if (stopEvent_) CloseHandle(stopEvent_);
  if (swr_) swr_free(&swr_);
}

bool WasapiPcmOutput::Init(IMMDevice* dev, RingBuffer* ring, const CaptureFormat& capFmt,
                           uint32_t safeFrames)
{
  if (!dev || !ring) {
    std::fprintf(stderr, "[WasapiPcmOutput] invalid output device or ring buffer\n");
    return false;
  }
  dev_ = dev;
  ring_ = ring;
  capFmt_ = capFmt;
  capBytesPerFrame_ = capFmt_.bytesPerFrame();
  safeFrames_ = safeFrames ? safeFrames : 1536;
  inSampleFmt_ = MapCaptureSampleFormat(capFmt_);
  if (!capBytesPerFrame_ || inSampleFmt_ == AV_SAMPLE_FMT_NONE || capFmt_.sampleRate == 0) {
    std::fprintf(stderr,
      "[WasapiPcmOutput] unsupported capture format (%u ch, %u Hz, %u-bit %s); PCM mode supports float32, s16 or s32 input\n",
      capFmt_.channels, capFmt_.sampleRate, capFmt_.bits, capFmt_.isFloat ? "float" : "int");
    return false;
  }

  HR_FAIL(dev_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client_),
          "Activate PCM IAudioClient");

  WAVEFORMATEX* mix = nullptr;
  HRESULT hr = client_->GetMixFormat(&mix);
  if (FAILED(hr) || !mix) {
    std::fprintf(stderr, "[WasapiPcmOutput] GetMixFormat failed: %s\n", HrStr(hr).c_str());
    if (mix) CoTaskMemFree(mix);
    return false;
  }

  const unsigned mixChannels = mix->nChannels;
  const unsigned mixRate = mix->nSamplesPerSec;
  const unsigned mixBits = mix->wBitsPerSample;
  outSampleFmt_ = MapWaveSampleFormat(mix);
  if (mixChannels != 2) {
    std::fprintf(stderr, "[WasapiPcmOutput] output mix format has %u channels; PCM S/PDIF mode requires 2 channels\n",
                 mixChannels);
    CoTaskMemFree(mix);
    return false;
  }
  if (mixRate != capFmt_.sampleRate) {
    std::fprintf(stderr, "[WasapiPcmOutput] sample-rate mismatch: capture=%u Hz, output mix=%u Hz; set both to the same rate (normally 48000 Hz)\n",
                 capFmt_.sampleRate, mixRate);
    CoTaskMemFree(mix);
    return false;
  }
  if (outSampleFmt_ == AV_SAMPLE_FMT_NONE || (mixBits != 16 && mixBits != 32)) {
    std::fprintf(stderr, "[WasapiPcmOutput] unsupported Windows output mix format (%u-bit, tag=0x%04X)\n",
                 mixBits, mix->wFormatTag);
    CoTaskMemFree(mix);
    return false;
  }

  outRate_ = mixRate;
  outBits_ = mixBits;
  outBytesPerFrame_ = static_cast<size_t>(mixChannels) * (mixBits / 8);

  AVChannelLayout inLayout{};
  AVChannelLayout outLayout{};
  int layoutResult = 0;
  if (capFmt_.channelMask != 0)
    layoutResult = av_channel_layout_from_mask(&inLayout, capFmt_.channelMask);
  if (capFmt_.channelMask == 0 || layoutResult < 0)
    av_channel_layout_default(&inLayout, static_cast<int>(capFmt_.channels));
  av_channel_layout_default(&outLayout, 2);

  int swrResult = swr_alloc_set_opts2(&swr_, &outLayout, outSampleFmt_, static_cast<int>(outRate_),
                                      &inLayout, inSampleFmt_, static_cast<int>(capFmt_.sampleRate),
                                      0, nullptr);
  av_channel_layout_uninit(&inLayout);
  av_channel_layout_uninit(&outLayout);
  if (swrResult < 0 || !swr_ || swr_init(swr_) < 0) {
    std::fprintf(stderr, "[WasapiPcmOutput] could not initialize PCM format converter\n");
    CoTaskMemFree(mix);
    return false;
  }

  // Shared-mode format is ordinary PCM/float, not IEC 61937. A receiver should switch
  // from its Dolby Digital indicator to PCM/Stereo when this stream becomes active.
  hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_NOPERSIST,
                           0, 0, mix, nullptr);
  CoTaskMemFree(mix);
  if (FAILED(hr)) {
    std::fprintf(stderr, "[WasapiPcmOutput] shared-mode PCM Initialize failed: %s\n", HrStr(hr).c_str());
    return false;
  }

  HR_FAIL(client_->GetBufferSize(&bufferFrames_), "PCM GetBufferSize");
  HR_FAIL(client_->GetService(__uuidof(IAudioRenderClient), &render_), "PCM GetService(IAudioRenderClient)");
  stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!stopEvent_) {
    std::fprintf(stderr, "[WasapiPcmOutput] CreateEvent failed: %lu\n", GetLastError());
    return false;
  }

  std::printf("[WasapiPcmOutput] initialized normal PCM output: 2 ch, %u Hz, %u-bit %s (shared mode, not IEC 61937)\n",
              outRate_, outBits_, outSampleFmt_ == AV_SAMPLE_FMT_FLT ? "float" : "integer");
  return true;
}

void WasapiPcmOutput::SetChannelGainsDb(const std::array<float, 6>& gainsDb)
{
  gainsLinear_[0].store(DbToLinear(gainsDb[0]), std::memory_order_relaxed);
  gainsLinear_[1].store(DbToLinear(gainsDb[1]), std::memory_order_relaxed);
}

bool WasapiPcmOutput::Start()
{
  if (running_.exchange(true)) return true;
  if (!client_ || !render_ || !stopEvent_ || !swr_) {
    running_.store(false);
    std::fprintf(stderr, "[WasapiPcmOutput] Start requested before successful Init\n");
    return false;
  }
  ResetEvent(stopEvent_);
  HRESULT hr = client_->Start();
  if (FAILED(hr)) {
    running_.store(false);
    std::fprintf(stderr, "[WasapiPcmOutput] client Start failed: %s\n", HrStr(hr).c_str());
    return false;
  }
  thread_ = std::thread(&WasapiPcmOutput::ThreadProc, this);
  return true;
}

void WasapiPcmOutput::Stop()
{
  running_.store(false, std::memory_order_relaxed);
  if (stopEvent_) SetEvent(stopEvent_);
  if (thread_.joinable()) thread_.join();
  if (client_) client_->Stop();
}

void WasapiPcmOutput::ApplyStereoGains(uint8_t* data, uint32_t frames)
{
  if (!data || frames == 0) return;
  const float left = gainsLinear_[0].load(std::memory_order_relaxed);
  const float right = gainsLinear_[1].load(std::memory_order_relaxed);
  if (left >= 0.999999f && right >= 0.999999f) return;

  if (outSampleFmt_ == AV_SAMPLE_FMT_FLT) {
    auto* s = reinterpret_cast<float*>(data);
    for (uint32_t i = 0; i < frames; ++i) {
      if (left < 0.999999f) s[2 * i] *= left;
      if (right < 0.999999f) s[2 * i + 1] *= right;
    }
  } else if (outSampleFmt_ == AV_SAMPLE_FMT_S16) {
    auto* s = reinterpret_cast<int16_t*>(data);
    for (uint32_t i = 0; i < frames; ++i) {
      const long l = std::lround(static_cast<double>(s[2 * i]) * left);
      const long r = std::lround(static_cast<double>(s[2 * i + 1]) * right);
      s[2 * i] = static_cast<int16_t>(std::clamp(l, -32768L, 32767L));
      s[2 * i + 1] = static_cast<int16_t>(std::clamp(r, -32768L, 32767L));
    }
  } else if (outSampleFmt_ == AV_SAMPLE_FMT_S32) {
    auto* s = reinterpret_cast<int32_t*>(data);
    for (uint32_t i = 0; i < frames; ++i) {
      const double l = std::round(static_cast<double>(s[2 * i]) * left);
      const double r = std::round(static_cast<double>(s[2 * i + 1]) * right);
      s[2 * i] = static_cast<int32_t>(std::clamp(l,
          static_cast<double>(std::numeric_limits<int32_t>::min()),
          static_cast<double>(std::numeric_limits<int32_t>::max())));
      s[2 * i + 1] = static_cast<int32_t>(std::clamp(r,
          static_cast<double>(std::numeric_limits<int32_t>::min()),
          static_cast<double>(std::numeric_limits<int32_t>::max())));
    }
  }
}

void WasapiPcmOutput::ThreadProc()
{
  DWORD taskIndex = 0;
  HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
  const size_t stagingBytes = static_cast<size_t>(bufferFrames_) * capBytesPerFrame_;
  inputStaging_.assign(stagingBytes, 0);

  while (running_.load(std::memory_order_relaxed)) {
    if (WaitForSingleObject(stopEvent_, 3) == WAIT_OBJECT_0) break;

    UINT32 padding = 0;
    HRESULT hr = client_->GetCurrentPadding(&padding);
    if (FAILED(hr)) {
      std::fprintf(stderr, "[WasapiPcmOutput] GetCurrentPadding failed: %s\n", HrStr(hr).c_str());
      break;
    }
    if (padding >= bufferFrames_) continue;
    const UINT32 writableFrames = bufferFrames_ - padding;
    if (writableFrames == 0) continue;

    BYTE* output = nullptr;
    hr = render_->GetBuffer(writableFrames, &output);
    if (FAILED(hr) || !output) {
      std::fprintf(stderr, "[WasapiPcmOutput] GetBuffer failed: %s\n", HrStr(hr).c_str());
      break;
    }

    const size_t wantedBytes = static_cast<size_t>(writableFrames) * capBytesPerFrame_;
    std::fill(inputStaging_.begin(), inputStaging_.begin() + wantedBytes, 0);
    const size_t availableBytes = ring_->BytesAvailable();
    size_t readBytes = std::min(wantedBytes, (availableBytes / capBytesPerFrame_) * capBytesPerFrame_);
    if (readBytes > 0) {
      const size_t got = ring_->Read(inputStaging_.data(), readBytes);
      readBytes = (got / capBytesPerFrame_) * capBytesPerFrame_;
      if (readBytes < wantedBytes)
        std::fill(inputStaging_.begin() + readBytes, inputStaging_.begin() + wantedBytes, 0);
    }

    const uint8_t* inPlanes[1] = { inputStaging_.data() };
    uint8_t* outPlanes[1] = { output };
    const int produced = swr_convert(swr_, outPlanes, static_cast<int>(writableFrames),
                                     inPlanes, static_cast<int>(writableFrames));
    if (produced < 0) {
      std::memset(output, 0, static_cast<size_t>(writableFrames) * outBytesPerFrame_);
      std::fprintf(stderr, "[WasapiPcmOutput] swr_convert failed; outputting silence\n");
    } else {
      if (static_cast<UINT32>(produced) < writableFrames)
        std::memset(output + static_cast<size_t>(produced) * outBytesPerFrame_, 0,
                    static_cast<size_t>(writableFrames - static_cast<UINT32>(produced)) * outBytesPerFrame_);
      ApplyStereoGains(output, static_cast<uint32_t>(produced));
    }

    hr = render_->ReleaseBuffer(writableFrames, 0);
    if (FAILED(hr)) {
      std::fprintf(stderr, "[WasapiPcmOutput] ReleaseBuffer failed: %s\n", HrStr(hr).c_str());
      break;
    }

    // Bound latency if the capture and render clocks drift apart over long sessions.
    if ((cycle_++ % 64) == 0) {
      const size_t queuedFrames = ring_->BytesAvailable() / capBytesPerFrame_;
      const size_t target = std::max<size_t>(safeFrames_, static_cast<size_t>(bufferFrames_)) * 2;
      if (queuedFrames > target)
        ring_->Discard((queuedFrames - target) * capBytesPerFrame_);
    }
  }

  if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
  running_.store(false, std::memory_order_relaxed);
}
