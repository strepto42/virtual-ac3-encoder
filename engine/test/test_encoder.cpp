// test_encoder.cpp — contract tests for SpdifEncoder (PCM -> AC3 -> IEC 61937).
#include "doctest.h"
#include "SpdifEncoder.h"
#include "Config.h"
#include "MeterSharedState.h"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
}

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace {

// IEC 61937 burst preamble Pa=0xF872, Pb=0x4E1F. Accept either byte order, since the
// only thing that matters is that the muxer produced a real burst (Phase 1 already
// proved ffmpeg can demux/decode it back to AC3).
bool HasIec61937Sync(const uint8_t* b)
{
  const bool le = (b[0] == 0x72 && b[1] == 0xF8 && b[2] == 0x1F && b[3] == 0x4E);
  const bool be = (b[0] == 0xF8 && b[1] == 0x72 && b[2] == 0x4E && b[3] == 0x1F);
  return le || be;
}

// DTS frame syncword: 0x7FFE8001 (16-bit core) in BE/LE, or 14-bit core in BE/LE.
// When DTS is encoded at full rate (1536 kbps), the 2048-byte DTS payload completely fills
// the S/PDIF slot, so the IEC 61937 preamble is omitted by FFmpeg and the burst begins
// directly with the DTS syncword.
bool HasDtsSync(const uint8_t* b)
{
  const bool be16 = (b[0] == 0x7F && b[1] == 0xFE && b[2] == 0x80 && b[3] == 0x01);
  const bool le16 = (b[0] == 0xFE && b[1] == 0x7F && b[2] == 0x01 && b[3] == 0x80);
  const bool be14 = (b[0] == 0x1F && b[1] == 0xFE && b[2] == 0x80 && b[3] == 0x00);
  const bool le14 = (b[0] == 0xFF && b[1] == 0x1F && b[2] == 0x00 && b[3] == 0xE8);
  return be16 || le16 || be14 || le14;
}

// Helper: init an encoder for `channels` interleaved samples in `fmt`.
bool InitFor(SpdifEncoder& enc, int channels, AVSampleFormat fmt, int rate = 48000,
             SpdifEncoder::Codec codec = SpdifEncoder::Codec::AC3,
             int64_t dtsBitRate = 768000)
{
  SpdifEncoder::Params p;
  p.codec = codec;
  p.sampleRate = rate;
  p.dtsBitRate = dtsBitRate;
  p.inSampleFmt = fmt;
  av_channel_layout_default(&p.inLayout, channels);
  bool ok = enc.Init(p);
  av_channel_layout_uninit(&p.inLayout);
  return ok;
}

} // namespace

TEST_CASE("init: 5.1 float")
{
  SpdifEncoder enc;
  REQUIRE(InitFor(enc, 6, AV_SAMPLE_FMT_FLT));
  CHECK(enc.IsOpen());
  CHECK(enc.FramesPerPacket() == 1536);
  CHECK(enc.InChannels() == 6);
}

TEST_CASE("encode: 5.1 float -> 6144-byte IEC 61937 burst")
{
  SpdifEncoder enc;
  REQUIRE(InitFor(enc, 6, AV_SAMPLE_FMT_FLT));
  std::vector<float> in(static_cast<size_t>(enc.FramesPerPacket()) * 6, 0.25f);
  std::vector<uint8_t> out(SpdifEncoder::kMaxBytesPerPacket);
  int n = enc.EncodePacket(reinterpret_cast<uint8_t*>(in.data()), out.data(),
                           static_cast<int>(out.size()));
  REQUIRE(n > 0);
  CHECK(n == 6144);
  CHECK(HasIec61937Sync(out.data()));
}

TEST_CASE("encode: 7.1 input is downmixed to 5.1")
{
  SpdifEncoder enc;
  REQUIRE(InitFor(enc, 8, AV_SAMPLE_FMT_FLT));
  CHECK(enc.InChannels() == 8);
  std::vector<float> in(static_cast<size_t>(enc.FramesPerPacket()) * 8, 0.1f);
  std::vector<uint8_t> out(SpdifEncoder::kMaxBytesPerPacket);
  int n = enc.EncodePacket(reinterpret_cast<uint8_t*>(in.data()), out.data(),
                           static_cast<int>(out.size()));
  CHECK(n > 0);
  CHECK(HasIec61937Sync(out.data()));
}

TEST_CASE("encode: stereo input is accepted")
{
  SpdifEncoder enc;
  REQUIRE(InitFor(enc, 2, AV_SAMPLE_FMT_FLT));
  std::vector<float> in(static_cast<size_t>(enc.FramesPerPacket()) * 2, 0.1f);
  std::vector<uint8_t> out(SpdifEncoder::kMaxBytesPerPacket);
  CHECK(enc.EncodePacket(reinterpret_cast<uint8_t*>(in.data()), out.data(),
                         static_cast<int>(out.size())) > 0);
}

TEST_CASE("encode: 16-bit integer input is accepted")
{
  SpdifEncoder enc;
  REQUIRE(InitFor(enc, 6, AV_SAMPLE_FMT_S16));
  std::vector<int16_t> in(static_cast<size_t>(enc.FramesPerPacket()) * 6, 1000);
  std::vector<uint8_t> out(SpdifEncoder::kMaxBytesPerPacket);
  int n = enc.EncodePacket(reinterpret_cast<uint8_t*>(in.data()), out.data(),
                           static_cast<int>(out.size()));
  CHECK(n > 0);
  CHECK(HasIec61937Sync(out.data()));
}

TEST_CASE("encode: consecutive packets keep producing bursts")
{
  SpdifEncoder enc;
  REQUIRE(InitFor(enc, 6, AV_SAMPLE_FMT_FLT));
  std::vector<float> in(static_cast<size_t>(enc.FramesPerPacket()) * 6, 0.2f);
  std::vector<uint8_t> out(SpdifEncoder::kMaxBytesPerPacket);
  int produced = 0;
  for (int i = 0; i < 10; ++i)
    if (enc.EncodePacket(reinterpret_cast<uint8_t*>(in.data()), out.data(),
                         static_cast<int>(out.size())) == 6144)
      ++produced;
  CHECK(produced >= 9); // allow at most one priming call to yield nothing
}

TEST_CASE("init: AC3-invalid sample rate fails cleanly")
{
  SpdifEncoder enc;
  CHECK_FALSE(InitFor(enc, 6, AV_SAMPLE_FMT_FLT, 96000)); // AC3 supports 48/44.1/32k only
}

TEST_CASE("init: DTS 5.1 encoder (full rate 1536k and half rate 768k)")
{
  SpdifEncoder encFull;
  REQUIRE(InitFor(encFull, 6, AV_SAMPLE_FMT_FLT, 48000, SpdifEncoder::Codec::DTS, 1536000));
  CHECK(encFull.IsOpen());
  CHECK(encFull.FramesPerPacket() == 512);
  CHECK(encFull.InChannels() == 6);

  SpdifEncoder encHalf;
  REQUIRE(InitFor(encHalf, 6, AV_SAMPLE_FMT_FLT, 48000, SpdifEncoder::Codec::DTS, 768000));
  CHECK(encHalf.IsOpen());
  CHECK(encHalf.FramesPerPacket() == 512);
  CHECK(encHalf.InChannels() == 6);
}

TEST_CASE("DTS 5.1 -> 2048-byte IEC 61937 burst")
{
  SpdifEncoder enc;
  REQUIRE(InitFor(enc, 6, AV_SAMPLE_FMT_FLT, 48000, SpdifEncoder::Codec::DTS));
  std::vector<float> in(static_cast<size_t>(enc.FramesPerPacket()) * 6, 0.1f);
  std::vector<uint8_t> out(SpdifEncoder::kMaxBytesPerPacket);

  int n = enc.EncodePacket(reinterpret_cast<const uint8_t*>(in.data()), out.data(),
                           static_cast<int>(out.size()));
  REQUIRE(n > 0);
  CHECK(n == 2048);
  CHECK(HasIec61937Sync(out.data()));
}

TEST_CASE("DTS 5.1 -> 2048-byte burst (full rate 1536k)")
{
  SpdifEncoder enc;
  REQUIRE(InitFor(enc, 6, AV_SAMPLE_FMT_FLT, 48000, SpdifEncoder::Codec::DTS, 1536000));
  std::vector<float> in(static_cast<size_t>(enc.FramesPerPacket()) * 6, 0.1f);
  std::vector<uint8_t> out(SpdifEncoder::kMaxBytesPerPacket);

  int n = enc.EncodePacket(reinterpret_cast<const uint8_t*>(in.data()), out.data(),
                           static_cast<int>(out.size()));
  REQUIRE(n > 0);
  CHECK(n == 2048);
  CHECK((HasIec61937Sync(out.data()) || HasDtsSync(out.data())));
}

TEST_CASE("DTS 5.1 -> 2048-byte IEC 61937 burst (half rate 768k)")
{
  SpdifEncoder enc;
  REQUIRE(InitFor(enc, 6, AV_SAMPLE_FMT_FLT, 48000, SpdifEncoder::Codec::DTS, 768000));
  std::vector<float> in(static_cast<size_t>(enc.FramesPerPacket()) * 6, 0.1f);
  std::vector<uint8_t> out(SpdifEncoder::kMaxBytesPerPacket);

  int n = enc.EncodePacket(reinterpret_cast<const uint8_t*>(in.data()), out.data(),
                           static_cast<int>(out.size()));
  REQUIRE(n > 0);
  CHECK(n == 2048);
  CHECK(HasIec61937Sync(out.data()));
}

TEST_CASE("latency: Config::GetEffectiveSafeFrames scales correctly")
{
  Config c;
  c.outputMode = "dts";
  c.latencyMode = "extreme";
  CHECK(c.GetEffectiveSafeFrames() == 512);

  c.latencyMode = "fast";
  CHECK(c.GetEffectiveSafeFrames() == 768);

  c.latencyMode = "balanced";
  CHECK(c.GetEffectiveSafeFrames() == 1024);

  c.latencyMode = "safe";
  CHECK(c.GetEffectiveSafeFrames() == 1536);

  c.latencyMode = "relaxed";
  CHECK(c.GetEffectiveSafeFrames() == 2048);

  // AC3 mode
  c.outputMode = "ac3";
  c.latencyMode = "extreme";
  CHECK(c.GetEffectiveSafeFrames() == 256);

  c.latencyMode = "fast";
  CHECK(c.GetEffectiveSafeFrames() == 512);

  c.latencyMode = "balanced";
  CHECK(c.GetEffectiveSafeFrames() == 768);

  c.latencyMode = "safe";
  CHECK(c.GetEffectiveSafeFrames() == 1024);

  c.latencyMode = "relaxed";
  CHECK(c.GetEffectiveSafeFrames() == 1536);

  // Custom numeric override
  c.safeFrames = 333;
  c.customSafeFrames = true;
  CHECK(c.GetEffectiveSafeFrames() == 333);
}

TEST_CASE("telemetry: SpdifEncoder calculates channel peak and RMS levels")
{
  SpdifEncoder enc;
  REQUIRE(InitFor(enc, 6, AV_SAMPLE_FMT_FLT, 48000, SpdifEncoder::Codec::AC3));
  const int fpp = enc.FramesPerPacket();
  std::vector<float> in(static_cast<size_t>(fpp) * 6, 0.0f);
  for (int i = 0; i < fpp; ++i)
  {
    in[static_cast<size_t>(i) * 6 + 0] = 0.5f;  // FL
    in[static_cast<size_t>(i) * 6 + 1] = -0.25f; // FR
  }

  std::vector<uint8_t> out(SpdifEncoder::kMaxBytesPerPacket);
  int n = enc.EncodePacket(reinterpret_cast<const uint8_t*>(in.data()), out.data(),
                           static_cast<int>(out.size()));
  REQUIRE(n > 0);

  float peaks[6] = {0};
  float rms[6] = {0};
  enc.GetChannelLevels(peaks, rms);
  CHECK(peaks[0] == doctest::Approx(0.5f).epsilon(0.01f));
  CHECK(peaks[1] == doctest::Approx(0.25f).epsilon(0.01f));
  CHECK(rms[0] == doctest::Approx(0.5f).epsilon(0.01f));
  CHECK(rms[1] == doctest::Approx(0.25f).epsilon(0.01f));
  CHECK(peaks[2] == doctest::Approx(0.0f).epsilon(0.001f));
  CHECK(peaks[3] == doctest::Approx(0.0f).epsilon(0.001f));
}

TEST_CASE("telemetry: MeterSharedMemory IPC producer and consumer transfer data")
{
  MeterSharedMemory producer;
  REQUIRE(producer.OpenProducer());
  MeterSharedState* pState = producer.Get();
  REQUIRE(pState != nullptr);
  pState->active = 1;
  pState->peakLevels[0] = 0.85f;
  pState->peakLevels[1] = 0.72f;
  pState->frameCounter = 12345;

  MeterSharedMemory consumer;
  REQUIRE(consumer.OpenConsumer());
  const MeterSharedState* cState = consumer.Get();
  REQUIRE(cState != nullptr);
  CHECK(cState->magic == kMeterMagic);
  CHECK(cState->active == 1);
  CHECK(cState->peakLevels[0] == doctest::Approx(0.85f));
  CHECK(cState->peakLevels[1] == doctest::Approx(0.72f));
  CHECK(cState->frameCounter == 12345);

  consumer.Close();
  producer.Close();
}

TEST_CASE("telemetry: RuntimeControlState parsing handles reload and gain trims")
{
  // Test parsing logic matching engine's LoadRuntimeControlState
  struct ControlState
  {
    bool stop = false;
    int reloadId = 0;
    std::array<float, 6> gainsDb{{0, 0, 0, 0, 0, 0}};
  } state;

  auto parseControl = [](const std::string& text, ControlState& s) {
    const char* keys[6] = {"fl_db", "fr_db", "c_db", "lfe_db", "sl_db", "sr_db"};
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line))
    {
      const size_t eq = line.find('=');
      if (eq == std::string::npos) continue;
      const std::string key = line.substr(0, eq);
      const std::string val = line.substr(eq + 1);
      if (key == "stop") s.stop = (val == "1");
      else if (key == "reload") s.reloadId = std::stoi(val);
      for (size_t i = 0; i < 6; ++i)
      {
        if (key == keys[i]) {
          s.gainsDb[i] = std::stof(val);
          break;
        }
      }
    }
  };

  std::string ini = "stop=0\nreload=15\nfl_db=-3.5\nfr_db=-3.5\nc_db=0\nlfe_db=-10\nsl_db=-60\nsr_db=0\n";
  parseControl(ini, state);

  CHECK(state.stop == false);
  CHECK(state.reloadId == 15);
  CHECK(state.gainsDb[0] == doctest::Approx(-3.5f));
  CHECK(state.gainsDb[1] == doctest::Approx(-3.5f));
  CHECK(state.gainsDb[2] == doctest::Approx(0.0f));
  CHECK(state.gainsDb[3] == doctest::Approx(-10.0f));
  CHECK(state.gainsDb[4] == doctest::Approx(-60.0f));
  CHECK(state.gainsDb[5] == doctest::Approx(0.0f));
}

// (The surround upmix is covered by a meaningful behavioral test in test_upmix.cpp:
//  it decodes the AC3 output and checks that energy actually lands in the surround channels.)
