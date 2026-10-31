// WasapiCapture.cpp — see WasapiCapture.h.
#include "WasapiCapture.h"

#include <mmreg.h>
#include <ksmedia.h>

#include <avrt.h>
#include <vector>

WasapiCapture::~WasapiCapture()
{
  Stop();
  if (dataEvent_) CloseHandle(dataEvent_);
  if (stopEvent_) CloseHandle(stopEvent_);
}

bool WasapiCapture::Init(IMMDevice* dev, bool loopback, REFERENCE_TIME bufferHns)
{
  loopback_ = loopback;
  bufferHns_ = (bufferHns > 0) ? bufferHns : 100000;
  if (loopback_ && bufferHns_ < 200000)
    bufferHns_ = 200000;

  HR_FAIL(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client_), "Activate IAudioClient");

  WAVEFORMATEX* mix = nullptr;
  HR_FAIL(client_->GetMixFormat(&mix), "GetMixFormat");

  fmt_.channels = mix->nChannels;
  fmt_.sampleRate = mix->nSamplesPerSec;
  fmt_.bits = mix->wBitsPerSample;
  fmt_.isFloat = (mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
  if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix->cbSize >= 22)
  {
    auto* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix);
    fmt_.channelMask = ext->dwChannelMask;
    fmt_.isFloat = (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
  }

  isEventDriven_ = !loopback_;
  DWORD flags = loopback_ ? AUDCLNT_STREAMFLAGS_LOOPBACK : AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
  HRESULT hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, bufferHns_, 0, mix, nullptr);
  CoTaskMemFree(mix);
  if (FAILED(hr)) { std::fprintf(stderr, "[WasapiCapture] Initialize failed: %s\n", HrStr(hr).c_str()); return false; }

  stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr); // manual-reset
  if (!stopEvent_) return false;
  if (isEventDriven_)
  {
    dataEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!dataEvent_) return false;
    hr = client_->SetEventHandle(dataEvent_);
    if (FAILED(hr))
    {
      HR_FAIL(hr, "SetEventHandle");
    }
  }
  HR_FAIL(client_->GetService(__uuidof(IAudioCaptureClient), &capture_), "GetService(IAudioCaptureClient)");

  std::printf("[WasapiCapture] %s (%s): %u ch, %u Hz, %u-bit %s, mask=0x%X\n",
              loopback_ ? "loopback" : "capture", isEventDriven_ ? "event-driven" : "polled",
              fmt_.channels, fmt_.sampleRate, fmt_.bits,
              fmt_.isFloat ? "float" : "int", fmt_.channelMask);
  return true;
}

bool WasapiCapture::Start()
{
  if (!ring_)
  {
    std::fprintf(stderr, "[WasapiCapture] SetRing() not called before Start()\n");
    return false;
  }
  if (running_.exchange(true))
    return true;
  ResetEvent(stopEvent_);
  HR_FAIL(client_->Start(), "client Start");
  thread_ = std::thread(&WasapiCapture::ThreadProc, this);
  return true;
}

void WasapiCapture::Stop()
{
  if (!running_.exchange(false))
    return;
  if (stopEvent_) SetEvent(stopEvent_);
  if (thread_.joinable()) thread_.join();
  if (client_) {
    client_->Stop();
    client_.Reset();
  }
  capture_.Reset();
  if (dataEvent_) { CloseHandle(dataEvent_); dataEvent_ = nullptr; }
  if (stopEvent_) { CloseHandle(stopEvent_); stopEvent_ = nullptr; }
}

void WasapiCapture::ThreadProc()
{
  DWORD taskIndex = 0;
  HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

  const size_t bpf = fmt_.bytesPerFrame();

  auto drain = [&]() {
    UINT32 packet = 0;
    while (SUCCEEDED(capture_->GetNextPacketSize(&packet)) && packet > 0)
    {
      BYTE* data = nullptr;
      UINT32 frames = 0;
      DWORD flags = 0;
      if (FAILED(capture_->GetBuffer(&data, &frames, &flags, nullptr, nullptr)))
        break;

      const size_t bytes = static_cast<size_t>(frames) * bpf;
      if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
      {
        // Push silence so the timeline stays continuous.
        static thread_local std::vector<uint8_t> zeros;
        if (zeros.size() < bytes) zeros.assign(bytes, 0);
        ring_->Write(zeros.data(), bytes);
      }
      else if (data)
      {
        ring_->Write(data, bytes); // overflow is dropped by the ring (consumer trims drift)
      }
      capture_->ReleaseBuffer(frames);
    }
  };

  if (isEventDriven_ && dataEvent_)
  {
    // 5 ms timeout ensures erratic Windows loopback events or quiet pauses never stall the drain loop
    HANDLE waits[2] = { dataEvent_, stopEvent_ };
    while (running_.load(std::memory_order_relaxed))
    {
      DWORD w = WaitForMultipleObjects(2, waits, FALSE, 5);
      if (w == WAIT_OBJECT_0 + 1) // stop
        break;
      drain();
    }
  }
  else
  {
    // Polled loopback fallback with 1 ms high-resolution timer
    while (running_.load(std::memory_order_relaxed))
    {
      if (WaitForSingleObject(stopEvent_, 1) == WAIT_OBJECT_0)
        break;
      drain();
    }
  }

  if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
}
