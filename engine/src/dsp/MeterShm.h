// MeterShm.h — level data from the engine to DSP Studio through shared memory.
//
// The engine publishes a small block every 100 ms (peaks before/after the DSP, compressor and limiter
// gain reduction); DSP Studio only reads it. The block is guarded by a sequence counter (seqlock): the
// reader never blocks the engine and never sees a half-written block. If Studio is not running nothing
// reads the block and nothing happens; if the engine is not running Studio sees the counter standing
// still. The Win32 wrappers at the bottom are the only platform-specific part.
#pragma once

#include "DspParams.h"
#include "Spectrum.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wtsan"
#endif

namespace vac3dsp {

constexpr uint32_t kMeterMagic = 0x4D335356u;   // "VS3M"
constexpr uint32_t kMeterVersion = 3u;   // 2: spectra added, 3: names of the playing app and the auto profile

struct DspMeterBlock
{
  uint32_t magic = 0;
  uint32_t version = 0;
  uint32_t seq = 0;          // sequence counter: odd while the engine is writing
  uint32_t updates = 0;      // grows with every publish; stands still when the engine is not running
  uint32_t sampleRate = 0;
  int32_t channels = 0;
  int32_t dspOn = 0;         // master switch as loaded from dsp-settings.json
  int32_t pcmMode = 0;       // 1: PCM stereo output (no limiter, only L/R)
  float inPeak[kMaxChannels] = {};    // linear peaks since the previous publish
  float outPeak[kMaxChannels] = {};
  float compReductionDb = 0.0f;       // >= 0
  float limiterReductionDb = 0.0f;    // >= 0
  int32_t spectrumValid = 0;          // 1: the spectra below are fresh (the engine only computes them while Studio asks)
  int32_t upmixMode = 0;              // 0 = no upmix, 1 = FFT `surround` filter, 2 = time-domain matrix
  float finalPeak[kMaxChannels] = {};   // peak per channel of what really goes to the encoder (after trims, headroom, limiter)
  char activeApp[40] = {};              // exe name of the application making sound now (UTF-8, empty: none / unknown)
  char autoProfile[48] = {};            // what the auto profiles last applied, e.g. "Кино + Movie" (UTF-8)
  float specIn[kMaxChannels][kSpecBands] = {};    // dBFS per band, before the DSP chain (valid when spectrumValid)
  float specOut[kMaxChannels][kSpecBands] = {};   // after it
};

// Copies a UTF-8 string into a fixed buffer without ever cutting a multi-byte character in half.
inline void CopyUtf8(char* dst, size_t capacity, const std::string& src)
{
  if (capacity == 0)
    return;
  size_t n = std::min(src.size(), capacity - 1);
  while (n > 0 && n < src.size() && (static_cast<unsigned char>(src[n]) & 0xC0) == 0x80)
    --n;
  std::memcpy(dst, src.data(), n);
  for (size_t i = n; i < capacity; ++i)
    dst[i] = '\0';
}

inline void ClearSpectra(DspMeterBlock& b)
{
  for (int c = 0; c < kMaxChannels; ++c)
    for (int i = 0; i < kSpecBands; ++i)
      b.specIn[c][i] = b.specOut[c][i] = kSpecFloorDb;
  b.spectrumValid = 0;
}

// What lives in the shared memory: the block the engine publishes, plus one word that Studio writes.
struct DspSharedLayout
{
  DspMeterBlock meters;
  uint32_t studioBeat = 0;   // Studio counts this up every ~250 ms while it wants a spectrum
};

static_assert(std::is_trivially_copyable<DspMeterBlock>::value, "shared memory block must be plain data");
static_assert(sizeof(DspMeterBlock) % sizeof(uint32_t) == 0, "the block is copied in 32-bit words");
static_assert(sizeof(std::atomic<uint32_t>) == sizeof(uint32_t), "seq is accessed as an atomic");

// The seqlock reads/writes the payload as plain words on purpose (that is how a seqlock works), so
// ThreadSanitizer is told to leave these copy loops alone; the sequence counter itself stays atomic.
#if defined(__SANITIZE_THREAD__)
#define VAC3_NO_TSAN __attribute__((no_sanitize("thread")))
#elif defined(__clang__)
#if __has_feature(thread_sanitizer)
#define VAC3_NO_TSAN __attribute__((no_sanitize("thread")))
#endif
#endif
#ifndef VAC3_NO_TSAN
#define VAC3_NO_TSAN
#endif

namespace detail {
VAC3_NO_TSAN inline void CopyWords(void* dst, const void* src, size_t bytes)
{
  volatile uint32_t* d = static_cast<volatile uint32_t*>(dst);
  const volatile uint32_t* s = static_cast<const volatile uint32_t*>(src);
  for (size_t i = 0; i < bytes / sizeof(uint32_t); ++i)
    d[i] = s[i];
}
inline std::atomic<uint32_t>& Seq(DspMeterBlock* b) { return *reinterpret_cast<std::atomic<uint32_t>*>(&b->seq); }
inline const std::atomic<uint32_t>& Seq(const DspMeterBlock* b)
{
  return *reinterpret_cast<const std::atomic<uint32_t>*>(&b->seq);
}
}  // namespace detail

// Single writer (the engine's main loop).
class MeterWriter
{
public:
  explicit MeterWriter(DspMeterBlock* block) : b_(block) {}

  void Publish(const DspMeterBlock& v)
  {
    if (!b_)
      return;
    auto& seq = detail::Seq(b_);
    const uint32_t s = seq.load(std::memory_order_relaxed);
    seq.store(s | 1u, std::memory_order_relaxed);          // odd: writing
    std::atomic_thread_fence(std::memory_order_release);
    const uint32_t updates = Updates() + 1u;
    detail::CopyWords(reinterpret_cast<char*>(b_) + kSkip, reinterpret_cast<const char*>(&v) + kSkip,
                      sizeof(DspMeterBlock) - kSkip);
    SetHeader(updates);
    seq.store((s | 1u) + 1u, std::memory_order_release);   // even again: readable
  }

private:
  static constexpr size_t kSkip = 3 * sizeof(uint32_t);    // magic, version, seq are managed here
  VAC3_NO_TSAN uint32_t Updates() const { return *static_cast<const volatile uint32_t*>(&b_->updates); }
  VAC3_NO_TSAN void SetHeader(uint32_t updates)
  {
    *static_cast<volatile uint32_t*>(&b_->magic) = kMeterMagic;
    *static_cast<volatile uint32_t*>(&b_->version) = kMeterVersion;
    *static_cast<volatile uint32_t*>(&b_->updates) = updates;
  }
  DspMeterBlock* b_;
};

// Any number of readers (DSP Studio).
class MeterReader
{
public:
  explicit MeterReader(const DspMeterBlock* block) : b_(block) {}

  // Returns false when nothing valid has been published yet (or the writer kept the block busy).
  bool Read(DspMeterBlock& out) const
  {
    if (!b_)
      return false;
    for (int attempt = 0; attempt < 16; ++attempt)
    {
      const uint32_t s1 = detail::Seq(b_).load(std::memory_order_acquire);
      if (s1 & 1u)
        continue;
      DspMeterBlock copy;
      detail::CopyWords(&copy, b_, sizeof(copy));
      std::atomic_thread_fence(std::memory_order_acquire);
      const uint32_t s2 = detail::Seq(b_).load(std::memory_order_relaxed);
      if (s1 != s2)
        continue;
      if (copy.magic != kMeterMagic || copy.version != kMeterVersion)
        return false;
      out = copy;
      return true;
    }
    return false;
  }

private:
  const DspMeterBlock* b_;
};

}  // namespace vac3dsp

// ---------------------------------------------------------------------------------------------
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace vac3dsp {

constexpr const wchar_t* kMeterMappingName = L"Local\\VirtualAc3DspMeters";

// Engine side: creates the mapping and publishes.
class MeterShmServer
{
public:
  ~MeterShmServer() { Close(); }

  bool Create()
  {
    Close();
    handle_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(DspSharedLayout),
                                 kMeterMappingName);
    if (!handle_)
      return false;
    view_ = static_cast<DspSharedLayout*>(MapViewOfFile(handle_, FILE_MAP_WRITE, 0, 0, sizeof(DspSharedLayout)));
    if (!view_)
    {
      Close();
      return false;
    }
    writer_ = MeterWriter(&view_->meters);
    return true;
  }

  void Publish(const DspMeterBlock& v)
  {
    if (view_)
      writer_.Publish(v);
  }

  // Counts up while a DSP Studio window wants the spectrum (0 until the first time).
  uint32_t StudioBeat() const
  {
    return view_ ? static_cast<const std::atomic<uint32_t>*>(static_cast<const void*>(&view_->studioBeat))->load(std::memory_order_relaxed) : 0u;
  }

  void Close()
  {
    if (view_)
      UnmapViewOfFile(view_);
    if (handle_)
      CloseHandle(handle_);
    view_ = nullptr;
    handle_ = nullptr;
    writer_ = MeterWriter(nullptr);
  }

private:
  HANDLE handle_ = nullptr;
  DspSharedLayout* view_ = nullptr;
  MeterWriter writer_{nullptr};
};

// Studio side: opens the mapping read-only (when the engine is not up yet Open() simply fails).
class MeterShmClient
{
public:
  ~MeterShmClient() { Close(); }

  bool IsOpen() const { return view_ != nullptr; }

  bool Open()
  {
    if (view_)
      return true;
    handle_ = OpenFileMappingW(FILE_MAP_WRITE, FALSE, kMeterMappingName);   // write access: only for the heartbeat word
    if (!handle_)
      return false;
    view_ = static_cast<DspSharedLayout*>(MapViewOfFile(handle_, FILE_MAP_WRITE, 0, 0, sizeof(DspSharedLayout)));
    if (!view_)
    {
      Close();
      return false;
    }
    return true;
  }

  bool Read(DspMeterBlock& out) const { return view_ && MeterReader(&view_->meters).Read(out); }

  // "Studio wants a spectrum": call every ~250 ms while the spectrum is shown.
  void Heartbeat()
  {
    if (view_)
      reinterpret_cast<std::atomic<uint32_t>*>(&view_->studioBeat)->fetch_add(1u, std::memory_order_relaxed);
  }

  void Close()
  {
    if (view_)
      UnmapViewOfFile(view_);
    if (handle_)
      CloseHandle(handle_);
    view_ = nullptr;
    handle_ = nullptr;
  }

private:
  HANDLE handle_ = nullptr;
  DspSharedLayout* view_ = nullptr;
};

}  // namespace vac3dsp
#endif  // _WIN32
