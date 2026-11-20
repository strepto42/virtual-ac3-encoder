// WavDump.h — records the first N seconds of an audio stream in memory and writes a WAV file on
// Write(). For diagnostics only (e.g. to find out whether a crackle is already in the captured
// input or is created later by the encoder). Append() never allocates after Configure(), so it is
// safe to call from the audio thread; file I/O only happens in Write().
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

class WavDump
{
public:
  // formatTag: 1 = integer PCM, 3 = IEEE float.
  void Configure(const std::string& path, int channels, int sampleRate, int bitsPerSample,
                 int formatTag, double maxSeconds)
  {
    path_ = path;
    channels_ = std::max(1, channels);
    rate_ = sampleRate;
    bits_ = bitsPerSample;
    tag_ = formatTag;
    const size_t bytesPerFrame = static_cast<size_t>(channels_) * static_cast<size_t>(bits_ / 8);
    capacity_ = static_cast<size_t>(maxSeconds * sampleRate) * bytesPerFrame;
    data_.clear();
    data_.reserve(capacity_);
    enabled_ = !path_.empty() && capacity_ > 0;
  }

  bool enabled() const { return enabled_; }
  bool full() const { return data_.size() >= capacity_; }
  size_t bytes() const { return data_.size(); }

  void Append(const void* bytes, size_t n)
  {
    if (!enabled_ || n == 0)
      return;
    n = std::min(n, capacity_ - data_.size());
    if (n == 0)
      return;
    const uint8_t* p = static_cast<const uint8_t*>(bytes);
    data_.insert(data_.end(), p, p + n); // stays within the reserved capacity: no reallocation
  }

  // Writes the file (once) and clears the buffer. Returns true on success or when there is nothing
  // to write.
  bool Write()
  {
    if (!enabled_ || data_.empty())
      return true;
    std::ofstream f(path_, std::ios::binary | std::ios::trunc);
    if (!f)
      return false;
    const uint32_t dataBytes = static_cast<uint32_t>(data_.size());
    const uint16_t blockAlign = static_cast<uint16_t>(channels_ * (bits_ / 8));
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    f.write("RIFF", 4);
    u32(36 + dataBytes);
    f.write("WAVEfmt ", 8);
    u32(16);
    u16(static_cast<uint16_t>(tag_));
    u16(static_cast<uint16_t>(channels_));
    u32(static_cast<uint32_t>(rate_));
    u32(static_cast<uint32_t>(rate_) * blockAlign);
    u16(blockAlign);
    u16(static_cast<uint16_t>(bits_));
    f.write("data", 4);
    u32(dataBytes);
    f.write(reinterpret_cast<const char*>(data_.data()), static_cast<std::streamsize>(data_.size()));
    f.flush();
    const bool ok = static_cast<bool>(f);
    data_.clear();
    return ok;
  }

  const std::string& path() const { return path_; }

private:
  std::string path_;
  int channels_ = 2;
  int rate_ = 48000;
  int bits_ = 32;
  int tag_ = 3;
  size_t capacity_ = 0;
  bool enabled_ = false;
  std::vector<uint8_t> data_;
};
