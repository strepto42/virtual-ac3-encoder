// Iec61937.h — extracts DTS bursts (IEC 61937-5, data types I/II/III) from a stream of 16-bit
// words. The words come from the L/R samples of a 16-bit stereo "carrier" (S/PDIF-style PCM):
//   L0 R0 L1 R1 ...  ==  Pa Pb Pc Pd <payload words> <stuffing zeros> Pa Pb ...
// Pa = 0xF872, Pb = 0x4E1F, Pc = burst info (bits 0-4 data type, bit 7 error flag),
// Pd = payload length in BITS for DTS types I-III.
// The payload words hold the DTS elementary stream as big-endian 16-bit words, so the bytes are
// recovered as (word >> 8, word & 0xFF), which is what libavcodec's DTS decoder expects.
//
// Pure C++17, no dependencies.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

struct Iec61937Burst
{
  int dataType = 0;              // Pc bits 0..4: 11 = DTS-I (512), 12 = DTS-II (1024), 13 = DTS-III (2048)
  std::vector<uint8_t> payload;  // DTS core frame bytes
};

class Iec61937Scanner
{
public:
  static constexpr uint16_t kPa = 0xF872;
  static constexpr uint16_t kPb = 0x4E1F;
  static constexpr int kTypeDts1 = 11;
  static constexpr int kTypeDts2 = 12;
  static constexpr int kTypeDts3 = 13;

  // Append `count` words in stream order. `count` must be even (whole L/R pairs) so that Pa
  // always sits on an even (left-channel) position.
  void Push(const int16_t* words, size_t count)
  {
    buf_.insert(buf_.end(), words, words + count);
  }

  // Returns true and fills `out` when a complete DTS burst is buffered; call until false.
  bool TakeBurst(Iec61937Burst& out)
  {
    for (;;)
    {
      size_t i = pos_ + (pos_ & 1u);
      bool found = false;
      for (; i + 1 < buf_.size(); i += 2)
      {
        if (static_cast<uint16_t>(buf_[i]) == kPa && static_cast<uint16_t>(buf_[i + 1]) == kPb)
        {
          found = true;
          break;
        }
      }
      if (!found)
      {
        pos_ = i;
        Compact();
        return false;
      }
      if (i + 4 > buf_.size())  // header not complete yet
      {
        pos_ = i;
        Compact();
        return false;
      }

      const uint16_t pc = static_cast<uint16_t>(buf_[i + 2]);
      const uint16_t bits = static_cast<uint16_t>(buf_[i + 3]);
      const int type = pc & 0x1F;
      const bool errorFlag = (pc & 0x80) != 0;

      if (type != kTypeDts1 && type != kTypeDts2 && type != kTypeDts3)
      {
        ++unsupported_;
        lastUnsupportedType_ = type;
        pos_ = i + 2;
        continue;
      }
      const size_t payloadWords = (static_cast<size_t>(bits) + 15) / 16;
      if (errorFlag || payloadWords == 0 || payloadWords > 4092)
      {
        ++rejected_;
        pos_ = i + 2;
        continue;
      }
      const size_t need = 4 + payloadWords;
      if (i + need > buf_.size())  // payload not complete yet: wait for more data
      {
        pos_ = i;
        Compact();
        return false;
      }

      out.dataType = type;
      out.payload.resize(payloadWords * 2);
      for (size_t k = 0; k < payloadWords; ++k)
      {
        const uint16_t w = static_cast<uint16_t>(buf_[i + 4 + k]);
        out.payload[2 * k] = static_cast<uint8_t>(w >> 8);
        out.payload[2 * k + 1] = static_cast<uint8_t>(w & 0xFF);
      }
      out.payload.resize((static_cast<size_t>(bits) + 7) / 8);
      pos_ = i + need;
      Compact();
      return true;
    }
  }

  uint64_t unsupportedBursts() const { return unsupported_; }
  int lastUnsupportedType() const { return lastUnsupportedType_; }
  uint64_t rejectedBursts() const { return rejected_; }

private:
  // Drop consumed words, always in whole L/R pairs so the left/right alignment is preserved.
  void Compact()
  {
    if (pos_ >= 16384)
    {
      const size_t drop = pos_ & ~static_cast<size_t>(1);
      buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(drop));
      pos_ -= drop;
    }
  }

  std::vector<int16_t> buf_;
  size_t pos_ = 0;
  uint64_t unsupported_ = 0;
  uint64_t rejected_ = 0;
  int lastUnsupportedType_ = 0;
};
