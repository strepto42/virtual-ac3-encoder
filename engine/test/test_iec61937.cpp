// test_iec61937.cpp — IEC 61937 DTS burst scanner (pure C++), plus a full round trip:
// 5.1 PCM -> SpdifEncoder (DTS, IEC 61937 bursts) -> DtsBitstreamDecoder -> 5.1 PCM.
#include "doctest.h"
#include "Iec61937.h"
#include "DtsBitstream.h"
#include "SpdifEncoder.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {
constexpr double kPi2 = 3.14159265358979323846;

// Build one IEC 61937 DTS burst as int16 words: Pa Pb Pc Pd payload(BE words) + zero stuffing up to
// `periodFrames` stereo frames.
std::vector<int16_t> MakeBurst(const std::vector<uint8_t>& es, int type, size_t periodFrames)
{
  std::vector<int16_t> w;
  w.push_back(static_cast<int16_t>(Iec61937Scanner::kPa));
  w.push_back(static_cast<int16_t>(Iec61937Scanner::kPb));
  w.push_back(static_cast<int16_t>(type));
  w.push_back(static_cast<int16_t>(es.size() * 8));
  for (size_t i = 0; i + 1 < es.size() + 1; i += 2)
  {
    const uint8_t hi = es[i];
    const uint8_t lo = (i + 1 < es.size()) ? es[i + 1] : 0;
    w.push_back(static_cast<int16_t>((hi << 8) | lo));
  }
  w.resize(periodFrames * 2, 0);
  return w;
}
} // namespace

TEST_CASE("scanner extracts a DTS burst and restores the big-endian payload bytes")
{
  std::vector<uint8_t> es;
  for (int i = 0; i < 1000; ++i) es.push_back(static_cast<uint8_t>(i * 7 + 3));
  Iec61937Scanner sc;
  const std::vector<int16_t> burst = MakeBurst(es, Iec61937Scanner::kTypeDts1, 512);
  sc.Push(burst.data(), burst.size());
  Iec61937Burst b;
  REQUIRE(sc.TakeBurst(b));
  CHECK(b.dataType == Iec61937Scanner::kTypeDts1);
  REQUIRE(b.payload.size() == es.size());
  CHECK(std::memcmp(b.payload.data(), es.data(), es.size()) == 0);
  CHECK_FALSE(sc.TakeBurst(b));
}

TEST_CASE("scanner handles bursts split across pushes, odd word counts and back-to-back bursts")
{
  std::vector<uint8_t> es1(601), es2(1500);
  for (size_t i = 0; i < es1.size(); ++i) es1[i] = static_cast<uint8_t>(i);
  for (size_t i = 0; i < es2.size(); ++i) es2[i] = static_cast<uint8_t>(255 - (i & 0xFF));
  std::vector<int16_t> stream;
  const auto a = MakeBurst(es1, Iec61937Scanner::kTypeDts1, 512);
  const auto c = MakeBurst(es2, Iec61937Scanner::kTypeDts2, 1024);
  stream.insert(stream.end(), 64, 0);  // leading silence
  stream.insert(stream.end(), a.begin(), a.end());
  stream.insert(stream.end(), c.begin(), c.end());
  stream.insert(stream.end(), a.begin(), a.end());

  Iec61937Scanner sc;
  int got = 0;
  Iec61937Burst b;
  for (size_t pos = 0; pos < stream.size(); pos += 2 * 333) // odd-sized chunks of whole L/R pairs
  {
    const size_t n = std::min<size_t>(2 * 333, stream.size() - pos);
    sc.Push(stream.data() + pos, n);
    while (sc.TakeBurst(b))
    {
      ++got;
      if (got == 2) { CHECK(b.payload.size() == es2.size()); CHECK(b.payload[0] == es2[0]); }
      else          { CHECK(b.payload.size() == es1.size()); }
    }
  }
  CHECK(got == 3);
}

TEST_CASE("scanner ignores ordinary PCM and non-DTS bursts")
{
  std::vector<int16_t> pcm(2 * 48000);
  for (size_t i = 0; i < pcm.size(); ++i)
    pcm[i] = static_cast<int16_t>(12000.0 * std::sin(kPi2 * 440.0 * double(i / 2) / 48000.0));
  Iec61937Scanner sc;
  sc.Push(pcm.data(), pcm.size());
  Iec61937Burst b;
  CHECK_FALSE(sc.TakeBurst(b));

  std::vector<uint8_t> es(100, 0x55);
  const auto ac3 = MakeBurst(es, 1 /* AC3 */, 1536);
  sc.Push(ac3.data(), ac3.size());
  CHECK_FALSE(sc.TakeBurst(b));
  CHECK(sc.unsupportedBursts() == 1);
  CHECK(sc.lastUnsupportedType() == 1);
}

TEST_CASE("DTS round trip: PCM 5.1 -> DTS bursts (SpdifEncoder) -> DtsBitstreamDecoder -> PCM 5.1")
{
  // 1. Encode 5.1 PCM to DTS (IEC 61937 bursts). FL = 440 Hz @0.4, FC = 220 Hz @0.3, rest silent.
  SpdifEncoder::Params p;
  p.codec = SpdifEncoder::Codec::DTS;
  p.sampleRate = 48000;
  p.inSampleFmt = AV_SAMPLE_FMT_FLT;
  p.headroomDb = 0.0f;
  p.limiter = false;
  REQUIRE(av_channel_layout_from_string(&p.inLayout, "5.1") == 0);
  SpdifEncoder enc;
  bool ok = enc.Init(p);
  av_channel_layout_uninit(&p.inLayout);
  REQUIRE(ok);

  const int fpp = enc.FramesPerPacket();
  std::vector<float> in(static_cast<size_t>(fpp) * 6);
  std::vector<uint8_t> out(SpdifEncoder::kMaxBytesPerPacket);
  std::vector<uint8_t> carrier; // the IEC 61937 byte stream (16-bit stereo words, little endian)
  long long t = 0;
  for (int pk = 0; pk < 200; ++pk)
  {
    for (int i = 0; i < fpp; ++i)
    {
      const double time = double(t + i) / 48000.0;
      float* f = &in[static_cast<size_t>(i) * 6];
      f[0] = 0.4f * static_cast<float>(std::sin(2.0 * kPi2 * 440.0 * time));
      f[2] = 0.3f * static_cast<float>(std::sin(2.0 * kPi2 * 220.0 * time));
      f[1] = f[3] = f[4] = f[5] = 0.0f;
    }
    t += fpp;
    const int n = enc.EncodePacket(reinterpret_cast<uint8_t*>(in.data()), out.data(), static_cast<int>(out.size()));
    if (n > 0) carrier.insert(carrier.end(), out.data(), out.data() + n);
  }
  REQUIRE(carrier.size() > 20000);

  // 2. Decode it again.
  AVChannelLayout outLayout{};
  REQUIRE(av_channel_layout_from_string(&outLayout, "5.1") == 0);
  DtsBitstreamDecoder dec;
  REQUIRE(dec.Init(outLayout, 48000));
  const int totalFrames = static_cast<int>(carrier.size() / 4);
  const int16_t* words = reinterpret_cast<const int16_t*>(carrier.data());
  for (int pos = 0; pos < totalFrames; pos += 1536)
    dec.Push(words + 2 * pos, std::min(1536, totalFrames - pos));
  CHECK(dec.Active());

  // 3. Measure the last 8192 decoded frames.
  const int avail = dec.Available();
  REQUIRE(avail > 8192 + 4096);
  std::vector<std::vector<float>> ch(6, std::vector<float>(static_cast<size_t>(avail)));
  uint8_t* planes[6];
  for (int c = 0; c < 6; ++c) planes[c] = reinterpret_cast<uint8_t*>(ch[static_cast<size_t>(c)].data());
  REQUIRE(dec.Read(reinterpret_cast<void**>(planes), avail));
  double rms[6] = {};
  for (int c = 0; c < 6; ++c)
  {
    double s = 0;
    for (int i = avail - 8192; i < avail; ++i) s += double(ch[static_cast<size_t>(c)][static_cast<size_t>(i)]) * ch[static_cast<size_t>(c)][static_cast<size_t>(i)];
    rms[c] = std::sqrt(s / 8192.0);
  }
  MESSAGE("decoded RMS FL=" << rms[0] << " FR=" << rms[1] << " FC=" << rms[2] << " LFE=" << rms[3] << " SL=" << rms[4] << " SR=" << rms[5]);
  CHECK(rms[0] == doctest::Approx(0.4 / std::sqrt(2.0)).epsilon(0.15));  // lossy codec: within 15 %
  CHECK(rms[2] == doctest::Approx(0.3 / std::sqrt(2.0)).epsilon(0.15));
  CHECK(rms[1] < 0.01);
  CHECK(rms[4] < 0.01);
  CHECK(rms[5] < 0.01);
  av_channel_layout_uninit(&outLayout);
}
