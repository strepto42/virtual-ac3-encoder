// WasapiPassthrough.cpp — see WasapiPassthrough.h.
#include "WasapiPassthrough.h"

#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <avrt.h>
#include <cstdio>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
}

namespace {

AVSampleFormat MapSampleFmt(const CaptureFormat& f)
{
  if (f.isFloat && f.bits == 32) return AV_SAMPLE_FMT_FLT;
  if (!f.isFloat && f.bits == 16) return AV_SAMPLE_FMT_S16;
  if (!f.isFloat && f.bits == 32) return AV_SAMPLE_FMT_S32;
  return AV_SAMPLE_FMT_NONE;
}

// Build the AC3-over-S/PDIF format. `extended` uses the full WAVEFORMATEXTENSIBLE_IEC61937
// (MSDN-canonical); otherwise a plain WAVEFORMATEXTENSIBLE (Kodi-style, broader compat).
void FillAc3Format(WAVEFORMATEXTENSIBLE_IEC61937& w, int rate, bool extended)
{
  ZeroMemory(&w, sizeof w);
  WAVEFORMATEXTENSIBLE& x = w.FormatExt;
  x.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
  x.Format.nChannels = 2;                       // IEC 60958 carrier is 2-channel
  x.Format.nSamplesPerSec = rate;
  x.Format.wBitsPerSample = 16;
  x.Format.nBlockAlign = 4;
  x.Format.nAvgBytesPerSec = rate * 4;
  x.Samples.wValidBitsPerSample = 16;
  x.dwChannelMask = KSAUDIO_SPEAKER_5POINT1;    // hints encoded 5.1 content
  x.SubFormat = KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_DIGITAL;
  if (extended)
  {
    x.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE_IEC61937) - sizeof(WAVEFORMATEX);
    w.dwEncodedSamplesPerSec = rate;
    w.dwEncodedChannelCount = 6;
    w.dwAverageBytesPerSec = 0;
  }
  else
  {
    x.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
  }
}

void FillDtsFormat(WAVEFORMATEXTENSIBLE_IEC61937& w, int rate, bool extended)
{
  ZeroMemory(&w, sizeof w);
  WAVEFORMATEXTENSIBLE& x = w.FormatExt;
  x.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
  x.Format.nChannels = 2;
  x.Format.nSamplesPerSec = rate;
  x.Format.wBitsPerSample = 16;
  x.Format.nBlockAlign = 4;
  x.Format.nAvgBytesPerSec = rate * 4;
  x.Samples.wValidBitsPerSample = 16;
  x.dwChannelMask = KSAUDIO_SPEAKER_5POINT1;
  x.SubFormat = KSDATAFORMAT_SUBTYPE_IEC61937_DTS;
  if (extended)
  {
    x.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE_IEC61937) - sizeof(WAVEFORMATEX);
    w.dwEncodedSamplesPerSec = rate;
    w.dwEncodedChannelCount = 6;
    w.dwAverageBytesPerSec = 0;
  }
  else
  {
    x.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
  }
}
} // namespace

WasapiPassthrough::~WasapiPassthrough()
{
  Stop();
  if (dataEvent_) CloseHandle(dataEvent_);
  if (stopEvent_) CloseHandle(stopEvent_);
}

bool WasapiPassthrough::ProbeAc3(IMMDevice* dev, int rate)
{
  ComPtr<IAudioClient> c;
  if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &c)))
    return false;
  WAVEFORMATEXTENSIBLE_IEC61937 w;
  FillAc3Format(w, rate, true);
  if (c->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &w.FormatExt.Format, nullptr) == S_OK)
    return true;
  FillAc3Format(w, rate, false);
  return c->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &w.FormatExt.Format, nullptr) == S_OK;
}

bool WasapiPassthrough::ProbeDts(IMMDevice* dev, int rate)
{
  ComPtr<IAudioClient> c;
  if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &c)))
    return false;
  WAVEFORMATEXTENSIBLE_IEC61937 w;
  FillDtsFormat(w, rate, true);
  if (c->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &w.FormatExt.Format, nullptr) == S_OK)
    return true;
  FillDtsFormat(w, rate, false);
  return c->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &w.FormatExt.Format, nullptr) == S_OK;
}
bool WasapiPassthrough::Init(IMMDevice* dev, RingBuffer* ring, const CaptureFormat& capFmt,
                             const Params& p)
{
  dev_ = dev;
  ring_ = ring;
  capFmt_ = capFmt;
  params_ = p;
  capBytesPerFrame_ = capFmt.bytesPerFrame();

  const int rate = static_cast<int>(capFmt.sampleRate);
  if (params_.codec == SpdifEncoder::Codec::DTS && rate != 48000)
  {
    std::fprintf(stderr, "[WasapiPassthrough] DTS mode currently requires 48000 Hz input.\n");
    return false;
  }
  if (params_.codec == SpdifEncoder::Codec::AC3 &&
      rate != 48000 && rate != 44100 && rate != 32000)
  {
    std::fprintf(stderr, "[WasapiPassthrough] AC3 capture rate %d unsupported; use 48000, 44100 or 32000 Hz.\n", rate);
    return false;
  }

  AVSampleFormat inFmt = MapSampleFmt(capFmt);
  if (inFmt == AV_SAMPLE_FMT_NONE)
  {
    std::fprintf(stderr, "[WasapiPassthrough] unsupported capture sample format (%u-bit %s)\n",
                 capFmt.bits, capFmt.isFloat ? "float" : "int");
    return false;
  }

  // Configure the encoder. Input = the capture layout; downmixed to 5.1 internally.
  SpdifEncoder::Params ep;
  ep.codec = params_.codec;
  ep.sampleRate = rate;
  ep.bitRate = params_.bitRate;
  ep.dtsBitRate = params_.dtsBitRate;
  ep.inSampleFmt = inFmt;
  ep.upmix = (params_.upmix != SpdifEncoder::Upmix::Off) ? params_.upmix
           : (params_.upmixSurround ? SpdifEncoder::Upmix::Surround : SpdifEncoder::Upmix::Off);
  if (capFmt.channelMask)
    av_channel_layout_from_mask(&ep.inLayout, capFmt.channelMask);
  else
    av_channel_layout_default(&ep.inLayout, static_cast<int>(capFmt.channels));

  bool encOk = enc_.Init(ep);
  av_channel_layout_uninit(&ep.inLayout);
  if (!encOk)
    return false;
  framesPerPacket_ = enc_.FramesPerPacket();
  burstBytes_ = static_cast<size_t>(framesPerPacket_) * kCarrierBytesPerFrame;
  if (framesPerPacket_ <= 0 || burstBytes_ == 0 || burstBytes_ > kMaxBurstBytes)
  {
    std::fprintf(stderr, "[WasapiPassthrough] invalid encoder frame/burst size: %d frames, %zu bytes\n",
                 framesPerPacket_, burstBytes_);
    return false;
  }

  if (!InitExclusive(rate))
    return false;

  const size_t pktBytes = static_cast<size_t>(framesPerPacket_) * capBytesPerFrame_;
  staging_.resize(pktBytes);
  silence_.assign(pktBytes, 0);
  burst_.resize(kMaxBurstBytes);
  sharedMem_.OpenProducer();
  return true;
}

bool WasapiPassthrough::InitExclusive(int rate)
{
  HR_FAIL(dev_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client_),
          "Activate IAudioClient");

  WAVEFORMATEXTENSIBLE_IEC61937 w;
  const bool dtsMode = params_.codec == SpdifEncoder::Codec::DTS;
  auto fillFormat = [&](bool isExtended)
  {
    if (dtsMode) FillDtsFormat(w, rate, isExtended);
    else FillAc3Format(w, rate, isExtended);
  };
  bool extended = true;
  fillFormat(true);
  HRESULT hr = client_->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &w.FormatExt.Format, nullptr);
  if (hr != S_OK)
  {
    FillAc3Format(w, rate, false);
    if (dtsMode) FillDtsFormat(w, rate, false);
    extended = false;
    hr = client_->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &w.FormatExt.Format, nullptr);
  }
  if (hr != S_OK)
  {
    std::fprintf(stderr, "[WasapiPassthrough] device does not support %s passthrough (IsFormatSupported = %s).\n",
                 dtsMode ? "DTS" : "AC3", HrStr(hr).c_str());
    return false;
  }
  std::printf("[WasapiPassthrough] %s format accepted (%s WAVEFORMATEXTENSIBLE_IEC61937)\n",
              dtsMode ? "DTS" : "AC3", extended ? "extended" : "plain");

  REFERENCE_TIME defPeriod = 0;
  HR_FAIL(client_->GetDevicePeriod(&defPeriod, nullptr), "GetDevicePeriod");

  // One AC3 burst = 1536 carrier frames. Start from a one-burst buffer and let WASAPI's
  // alignment requirements adjust it.
  REFERENCE_TIME burstHns =
      static_cast<REFERENCE_TIME>(10000000.0 * framesPerPacket_ / rate + 0.5);
  REFERENCE_TIME bufferHns = burstHns > defPeriod ? burstHns : defPeriod;
  bool needNewClient = false;

  do
  {
    if (needNewClient)
      HR_FAIL(dev_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                             reinterpret_cast<void**>(client_.ReleaseAndGetAddressOf())),
              "re-Activate IAudioClient");

    hr = client_->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,
                             AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST,
                             bufferHns, bufferHns, &w.FormatExt.Format, nullptr);

    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED)
    {
      UINT32 n = 0;
      HR_FAIL(client_->GetBufferSize(&n), "GetBufferSize (align)");
      bufferHns = static_cast<REFERENCE_TIME>(10000.0 * 1000 / rate * n + 0.5);
      needNewClient = true;
    }
    else if (hr == AUDCLNT_E_BUFFER_SIZE_ERROR || hr == AUDCLNT_E_INVALID_DEVICE_PERIOD ||
             hr == E_OUTOFMEMORY)
    {
      bufferHns -= defPeriod;
      needNewClient = false;
    }
    else
    {
      break;
    }
  } while (bufferHns >= defPeriod);

  if (FAILED(hr))
  {
    if (hr == static_cast<HRESULT>(0x8889000A))
    {
      std::fprintf(stderr,
                   "[WasapiPassthrough] exclusive Initialize failed: 0x8889000a (AUDCLNT_E_DEVICE_IN_USE).\n"
                   "  -> Device in use by another exclusive application or duplicate engine instance.\n");
    }
    else
    {
      std::fprintf(stderr, "[WasapiPassthrough] exclusive Initialize failed: %s\n", HrStr(hr).c_str());
    }
    return false;
  }

  HR_FAIL(client_->GetBufferSize(&bufferFrames_), "GetBufferSize");
  burstsPerCycle_ = static_cast<int>(bufferFrames_ / framesPerPacket_);
  if (burstsPerCycle_ < 1)
  {
    std::fprintf(stderr, "[WasapiPassthrough] buffer (%u frames) smaller than one AC3 burst\n",
                 bufferFrames_);
    return false;
  }
  if (bufferFrames_ % framesPerPacket_ != 0)
    std::fprintf(stderr, "[WasapiPassthrough] note: buffer %u not a multiple of %d; "
                         "remainder will be zero-stuffed\n", bufferFrames_, framesPerPacket_);

  dataEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!dataEvent_ || !stopEvent_) return false;
  HR_FAIL(client_->SetEventHandle(dataEvent_), "SetEventHandle");
  HR_FAIL(client_->GetService(__uuidof(IAudioRenderClient), &render_), "GetService(IAudioRenderClient)");

  std::printf("[WasapiPassthrough] exclusive buffer = %u frames (%d burst(s)/cycle, ~%.1f ms)\n",
              bufferFrames_, burstsPerCycle_, 1000.0 * bufferFrames_ / rate);
  return true;
}

bool WasapiPassthrough::EncodeIntoBuffer(BYTE* out)
{
  const size_t pktBytes = staging_.size();
  const uint32_t required = static_cast<uint32_t>(burstsPerCycle_) * framesPerPacket_;
  const uint32_t primeTargetFrames = required + params_.safeFrames;
  const size_t primeTargetBytes = static_cast<size_t>(primeTargetFrames) * capBytesPerFrame_;

  // Pre-roll priming: do not begin consuming real audio until the ring buffer has
  // accumulated safeFrames of cushion. This completely eliminates micro-starvation
  // caused by mismatched chunk boundaries (e.g. WASAPI 480-sample loopback vs 512-sample DTS).
  if (!primed_)
  {
    if (ring_->BytesAvailable() >= primeTargetBytes)
    {
      primed_ = true;
    }
  }

  size_t outOff = 0;
  bool hadAudio = false;
  for (int b = 0; b < burstsPerCycle_; ++b)
  {
    const uint8_t* in;
    if (primed_ && ring_->BytesAvailable() >= pktBytes)
    {
      ring_->Read(staging_.data(), pktBytes);
      in = staging_.data();
      hadAudio = true;
    }
    else
    {
      // If the buffer underruns, reset primed state only if the ring buffer is completely starved.
      if (ring_->BytesAvailable() == 0)
        primed_ = false;
      in = silence_.data(); // underrun: emit AC3/DTS silence, leave the ring to refill
    }

    const size_t slotBytes = burstBytes_;
    const int n = enc_.EncodePacket(in, burst_.data(), static_cast<int>(burst_.size()));
    if (n <= 0 || static_cast<size_t>(n) != slotBytes)
    {
      static int mismatchLogs = 0;
      if (mismatchLogs < 10)
      {
        std::fprintf(stderr,
          "[WasapiPassthrough] burst size mismatch: actual=%d expected=%llu\n",
          n, static_cast<unsigned long long>(slotBytes));
        ++mismatchLogs;
      }
      // A missing or incorrectly sized burst must not corrupt the carrier alignment.
      std::memset(burst_.data(), 0, slotBytes);
    }
    std::memcpy(out + outOff, burst_.data(), slotBytes);
    outOff += slotBytes;
  }

  const size_t totalBytes = static_cast<size_t>(bufferFrames_) * kCarrierBytesPerFrame;
  if (outOff < totalBytes)
    std::memset(out + outOff, 0, totalBytes - outOff); // inter-burst stuffing
  return hadAudio;
}

bool WasapiPassthrough::Start()
{
  if (running_.exchange(true))
    return true;
  ResetEvent(stopEvent_);
  cycle_ = 0;
  minAvail_ = 0xFFFFFFFFu;
  primed_ = false;

  // Pre-fill the first buffer (primes the encoder and avoids an initial underrun) before Start.
  BYTE* out = nullptr;
  if (SUCCEEDED(render_->GetBuffer(bufferFrames_, &out)))
  {
    const bool hadAudio = EncodeIntoBuffer(out);
    render_->ReleaseBuffer(bufferFrames_, 0);

    if (sharedMem_.IsOpen())
    {
      MeterSharedState* ms = sharedMem_.Get();
      if (ms)
      {
        if (hadAudio)
        {
          enc_.GetChannelLevels(ms->peakLevels, ms->rmsLevels);
        }
        ms->active = 1;
        ms->frameCounter++;
      }
    }
  }

  HR_FAIL(client_->Start(), "client Start");
  if (sharedMem_.IsOpen())
  {
    MeterSharedState* ms = sharedMem_.Get();
    if (ms)
    {
      ms->active = 1;
      ms->sampleRate = static_cast<uint32_t>(capFmt_.sampleRate);
      ms->channels = 6;
      ms->isDts = (params_.codec == SpdifEncoder::Codec::DTS ? 1 : 0);
      ms->safeFrames = params_.safeFrames;
    }
  }
  thread_ = std::thread(&WasapiPassthrough::ThreadProc, this);
  return true;
}

void WasapiPassthrough::Stop()
{
  if (!running_.exchange(false))
    return;
  primed_ = false;
  if (sharedMem_.IsOpen())
  {
    MeterSharedState* ms = sharedMem_.Get();
    if (ms)
    {
      ms->active = 0;
      for (int i = 0; i < 6; ++i) { ms->peakLevels[i] = 0.0f; ms->rmsLevels[i] = 0.0f; }
    }
  }
  if (stopEvent_) SetEvent(stopEvent_);
  if (thread_.joinable()) thread_.join();
  if (client_) {
    client_->Stop();
    client_.Reset();
  }
  render_.Reset();
  dev_.Reset();
  if (dataEvent_) { CloseHandle(dataEvent_); dataEvent_ = nullptr; }
  if (stopEvent_) { CloseHandle(stopEvent_); stopEvent_ = nullptr; }
}

void WasapiPassthrough::ThreadProc()
{
  DWORD taskIndex = 0;
  HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

  const uint32_t required = static_cast<uint32_t>(burstsPerCycle_) * framesPerPacket_;
  const uint32_t desired = required + params_.safeFrames;
  HANDLE waits[2] = { dataEvent_, stopEvent_ };

  while (running_.load(std::memory_order_relaxed))
  {
    DWORD wr = WaitForMultipleObjects(2, waits, FALSE, 2000);
    if (wr == WAIT_OBJECT_0 + 1)
      break;
    if (wr == WAIT_TIMEOUT)
    {
      std::fprintf(stderr, "[WasapiPassthrough] render event timed out\n");
      continue;
    }

    // --- drift control (SoundPusher style with hysteresis & gentle step-trimming) ---
    // Only apply drift trimming when the stream is actively playing audio (primed_).
    if (primed_)
    {
      const uint32_t avail =
          static_cast<uint32_t>(ring_->BytesAvailable() / capBytesPerFrame_);
      if (avail < minAvail_)
        minAvail_ = avail;

      // Evaluate drift over a ~2-second window (e.g. 64 cycles for AC3 @ 32ms, 192 cycles for DTS @ 10.6ms)
      const uint32_t driftCycles = (framesPerPacket_ <= 512) ? 192 : 64;
      if (cycle_++ % driftCycles == 0)
      {
        // Only trim if the MINIMUM available buffer over the entire window has exceeded desired + hysteresis.
        // This deadband ensures that normal OS thread scheduling jitter (+-3-5 ms) never causes an underrun.
        const uint32_t hysteresis = std::max<uint32_t>(static_cast<uint32_t>(framesPerPacket_) * 2, 1024u);
        if (minAvail_ != 0xFFFFFFFFu && minAvail_ > desired + hysteresis)
        {
          const uint32_t excess = minAvail_ - (desired + hysteresis / 2);
          const uint32_t maxTrim = framesPerPacket_ / 2; // Trim gently in small steps
          const uint32_t trim = std::min(excess, maxTrim);
          ring_->Discard(static_cast<size_t>(trim) * capBytesPerFrame_);
        }
        minAvail_ = 0xFFFFFFFFu;
      }
    }
    else
    {
      minAvail_ = 0xFFFFFFFFu;
      cycle_ = 0;
    }

    BYTE* out = nullptr;
    HRESULT hr = render_->GetBuffer(bufferFrames_, &out);
    if (FAILED(hr))
    {
      std::fprintf(stderr, "[WasapiPassthrough] GetBuffer failed: %s\n", HrStr(hr).c_str());
      break;
    }
    const bool hadAudio = EncodeIntoBuffer(out);
    render_->ReleaseBuffer(bufferFrames_, 0);

    if (sharedMem_.IsOpen())
    {
      MeterSharedState* ms = sharedMem_.Get();
      if (ms)
      {
        if (hadAudio)
        {
          enc_.GetChannelLevels(ms->peakLevels, ms->rmsLevels);
        }
        ms->active = 1;
        ms->frameCounter++;
      }
    }
  }

  if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
}
