// test_upmix.cpp — MEANINGFUL behavioral test for the stereo->5.1 surround upmix.
//
// Strategy: feed an anti-phase stereo signal (L = +s, R = -s). A matrix/steered upmixer sends
// the difference (L-R) component to the SURROUND channels. We encode to AC3, decode it back to
// 5.1 PCM, and measure per-channel RMS:
//   * upmix=surround  -> surround channels carry real energy.
//   * upmix=off       -> surround channels are ~silent (swr maps stereo to the fronts).
// The test asserts both, so it FAILS if the upmix is a no-op (i.e. it can't pass by accident).

#include "doctest.h"
#include "SpdifEncoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
#include <libavutil/samplefmt.h>
}

#include <cmath>
#include <cstring>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

struct MemReader { const uint8_t* data; int size; int pos; };

int ReadMem(void* opaque, uint8_t* buf, int bufsize)
{
  auto* r = static_cast<MemReader*>(opaque);
  int n = r->size - r->pos;
  if (n <= 0) return AVERROR_EOF;
  if (n > bufsize) n = bufsize;
  std::memcpy(buf, r->data + r->pos, static_cast<size_t>(n));
  r->pos += n;
  return n;
}

// Decode an in-memory IEC 61937 (S/PDIF) AC3 stream; return per-channel RMS of the first
// decoded frame (FFmpeg order: FL FR FC LFE SL/BL SR/BR), or empty on failure.
std::vector<double> DecodeSpdifAc3Rms(const uint8_t* data, int size)
{
  std::vector<double> rms;
  MemReader r{data, size, 0};
  unsigned char* avioBuf = static_cast<unsigned char*>(av_malloc(4096));
  AVIOContext* avio = avio_alloc_context(avioBuf, 4096, 0, &r, &ReadMem, nullptr, nullptr);
  AVFormatContext* fmt = avformat_alloc_context();
  fmt->pb = avio;
  fmt->flags |= AVFMT_FLAG_CUSTOM_IO;

  const AVInputFormat* spdif = av_find_input_format("spdif");
  AVCodecContext* dctx = nullptr;
  AVPacket* pkt = nullptr;
  AVFrame* frame = nullptr;
  bool opened = false;

  if (spdif && avformat_open_input(&fmt, nullptr, spdif, nullptr) == 0)
  {
    opened = true;
    if (avformat_find_stream_info(fmt, nullptr) >= 0)
    {
      int sidx = -1;
      for (unsigned i = 0; i < fmt->nb_streams; ++i)
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) { sidx = static_cast<int>(i); break; }
      if (sidx >= 0)
      {
        const AVCodec* dec = avcodec_find_decoder(fmt->streams[sidx]->codecpar->codec_id);
        if (dec)
        {
          dctx = avcodec_alloc_context3(dec);
          avcodec_parameters_to_context(dctx, fmt->streams[sidx]->codecpar);
          if (avcodec_open2(dctx, dec, nullptr) == 0)
          {
            pkt = av_packet_alloc();
            frame = av_frame_alloc();
            // Decode every frame and keep the LAST one's RMS. (The surround path prepends a
            // few frames of primed silence to cover FFT latency, so the first frame is not
            // representative; a late frame is steady-state real signal.)
            while (av_read_frame(fmt, pkt) >= 0)
            {
              if (pkt->stream_index == sidx && avcodec_send_packet(dctx, pkt) == 0)
              {
                while (avcodec_receive_frame(dctx, frame) == 0)
                {
                  if (frame->nb_samples > 0)
                  {
                    const int ch = frame->ch_layout.nb_channels;
                    rms.assign(static_cast<size_t>(ch), 0.0);
                    for (int c = 0; c < ch; ++c)
                    {
                      const float* p = reinterpret_cast<const float*>(frame->data[c]);
                      double s = 0;
                      for (int i = 0; i < frame->nb_samples; ++i) s += double(p[i]) * p[i];
                      rms[c] = std::sqrt(s / frame->nb_samples);
                    }
                  }
                }
              }
              av_packet_unref(pkt);
            }
          }
        }
      }
    }
  }

  if (frame) av_frame_free(&frame);
  if (pkt) av_packet_free(&pkt);
  if (dctx) avcodec_free_context(&dctx);
  if (opened) avformat_close_input(&fmt);
  else avformat_free_context(fmt);
  if (avio) { av_freep(&avio->buffer); avio_context_free(&avio); }
  return rms;
}

// Encode ~24 packets of anti-phase 440 Hz stereo (L = +s, R = -s) through the given upmix mode,
// decode the AC3 back, and return the per-channel RMS of a steady-state frame
// (FFmpeg 5.1 order: FL FR FC LFE SL SR). A matrix/steered upmixer sends the L-R (anti-phase)
// component to the surround channels; a non-upmixed path leaves the surrounds silent.
std::vector<double> AntiPhaseSurroundRms(SpdifEncoder::Upmix mode)
{
  SpdifEncoder::Params p;
  p.sampleRate = 48000;
  p.inSampleFmt = AV_SAMPLE_FMT_FLT;
  p.upmix = mode;
  av_channel_layout_default(&p.inLayout, 2);
  SpdifEncoder enc;
  bool ok = enc.Init(p);
  av_channel_layout_uninit(&p.inLayout);
  REQUIRE(ok);

  const int fpp = enc.FramesPerPacket();
  std::vector<float> in(static_cast<size_t>(fpp) * 2);
  std::vector<uint8_t> out(SpdifEncoder::kMaxBytesPerPacket);
  std::vector<uint8_t> stream;
  long long t = 0;
  for (int pk = 0; pk < 24; ++pk)
  {
    for (int i = 0; i < fpp; ++i)
    {
      float s = 0.3f * static_cast<float>(std::sin(2.0 * kPi * 440.0 * double(t + i) / 48000.0));
      in[2 * i] = s;       // L
      in[2 * i + 1] = -s;  // R (anti-phase)
    }
    t += fpp;
    int n = enc.EncodePacket(reinterpret_cast<uint8_t*>(in.data()), out.data(),
                             static_cast<int>(out.size()));
    if (n > 0) stream.insert(stream.end(), out.data(), out.data() + n);
  }
  REQUIRE(stream.size() > static_cast<size_t>(6144) * 4);

  std::vector<double> rms = DecodeSpdifAc3Rms(stream.data(), static_cast<int>(stream.size()));
  REQUIRE(rms.size() >= 6);
  return rms;
}

} // namespace

TEST_CASE("upmix=surround steers anti-phase stereo into the surround channels")
{
  std::vector<double> on = AntiPhaseSurroundRms(SpdifEncoder::Upmix::Surround);
  std::vector<double> off = AntiPhaseSurroundRms(SpdifEncoder::Upmix::Off);
  const double surrOn = 0.5 * (on[4] + on[5]);
  const double surrOff = 0.5 * (off[4] + off[5]);
  MESSAGE("surround-channel RMS: on=" << surrOn << "  off=" << surrOff);

  // Without upmix, swr maps stereo to the fronts -> surrounds are effectively silent.
  CHECK(surrOff < 0.001);
  // With upmix, the anti-phase component lands in the surrounds (~0.147 observed).
  CHECK(surrOn > 0.05);
  // ...dramatically more than the non-upmixed path -> fails if the upmix were a no-op.
  CHECK(surrOn > 50.0 * surrOff);
}

// Dynamic upmix: input is 5.1 but only FL+FR carry signal (simulates a stereo source played
// through a 5.1-configured virtual cable). With upmix=Surround, the encoder should detect the
// effectively-stereo content and route it through the surround filter, producing energy in the
// rear channels. Without upmix (Off), surrounds should stay silent.
TEST_CASE("dynamic upmix: stereo-in-5.1 gets upmixed to real surround")
{
  auto runDynamic = [](SpdifEncoder::Upmix mode) -> std::vector<double>
  {
    SpdifEncoder::Params p;
    p.sampleRate = 48000;
    p.inSampleFmt = AV_SAMPLE_FMT_FLT;
    p.upmix = mode;
    // 5.1 input layout (simulates a 5.1-configured virtual cable).
    AVChannelLayout lay51 = AV_CHANNEL_LAYOUT_5POINT1_BACK;
    av_channel_layout_copy(&p.inLayout, &lay51);
    SpdifEncoder enc;
    bool ok = enc.Init(p);
    av_channel_layout_uninit(&p.inLayout);
    REQUIRE(ok);

    const int fpp = enc.FramesPerPacket();
    const int ch = 6; // 5.1
    std::vector<float> in(static_cast<size_t>(fpp) * ch, 0.0f);
    std::vector<uint8_t> out(SpdifEncoder::kMaxBytesPerPacket);
    std::vector<uint8_t> stream;
    long long t = 0;
    for (int pk = 0; pk < 24; ++pk)
    {
      // Anti-phase stereo in FL+FR; all other channels zero (as Windows does for stereo
      // sources on a 5.1 endpoint).
      for (int i = 0; i < fpp; ++i)
      {
        float s = 0.3f * static_cast<float>(std::sin(2.0 * kPi * 440.0 * double(t + i) / 48000.0));
        in[static_cast<size_t>(i) * ch + 0] = s;   // FL
        in[static_cast<size_t>(i) * ch + 1] = -s;  // FR (anti-phase)
        in[static_cast<size_t>(i) * ch + 2] = 0.0f; // FC
        in[static_cast<size_t>(i) * ch + 3] = 0.0f; // LFE
        in[static_cast<size_t>(i) * ch + 4] = 0.0f; // BL
        in[static_cast<size_t>(i) * ch + 5] = 0.0f; // BR
      }
      t += fpp;
      int n = enc.EncodePacket(reinterpret_cast<uint8_t*>(in.data()), out.data(),
                               static_cast<int>(out.size()));
      if (n > 0) stream.insert(stream.end(), out.data(), out.data() + n);
    }
    REQUIRE(stream.size() > static_cast<size_t>(6144) * 4);

    std::vector<double> rms = DecodeSpdifAc3Rms(stream.data(), static_cast<int>(stream.size()));
    REQUIRE(rms.size() >= 6);
    return rms;
  };

  std::vector<double> on = runDynamic(SpdifEncoder::Upmix::Surround);
  std::vector<double> off = runDynamic(SpdifEncoder::Upmix::Off);
  const double surrOn = 0.5 * (on[4] + on[5]);
  const double surrOff = 0.5 * (off[4] + off[5]);
  MESSAGE("dynamic upmix surround-channel RMS: on=" << surrOn << "  off=" << surrOff);

  // Without upmix, surrounds are near-silent (swr just passes the zero channels through).
  CHECK(surrOff < 0.01);
  // With dynamic upmix, the anti-phase stereo content should be steered to surrounds.
  CHECK(surrOn > 0.05);
  // Dramatically more than the non-upmixed path.
  CHECK(surrOn > 10.0 * surrOff);
}

// Dynamic upmix should pass real 5.1 content through without mangling it: when all 6 channels
// carry signal, the swr multichannel path should be used (no stereo extraction/upmix).
TEST_CASE("dynamic upmix: real 5.1 content passes through unchanged")
{
  SpdifEncoder::Params p;
  p.sampleRate = 48000;
  p.inSampleFmt = AV_SAMPLE_FMT_FLT;
  p.upmix = SpdifEncoder::Upmix::Surround; // dynamic mode (input is 5.1)
  AVChannelLayout lay51 = AV_CHANNEL_LAYOUT_5POINT1_BACK;
  av_channel_layout_copy(&p.inLayout, &lay51);
  SpdifEncoder enc;
  bool ok = enc.Init(p);
  av_channel_layout_uninit(&p.inLayout);
  REQUIRE(ok);

  const int fpp = enc.FramesPerPacket();
  const int ch = 6;
  std::vector<float> in(static_cast<size_t>(fpp) * ch);
  std::vector<uint8_t> out(SpdifEncoder::kMaxBytesPerPacket);
  std::vector<uint8_t> stream;
  long long t = 0;
  for (int pk = 0; pk < 24; ++pk)
  {
    // All 6 channels carry a distinct tone (real 5.1 content — e.g. Netflix).
    for (int i = 0; i < fpp; ++i)
    {
      double base = 2.0 * kPi * double(t + i) / 48000.0;
      in[static_cast<size_t>(i) * ch + 0] = 0.2f * static_cast<float>(std::sin(440.0 * base)); // FL
      in[static_cast<size_t>(i) * ch + 1] = 0.2f * static_cast<float>(std::sin(554.0 * base)); // FR
      in[static_cast<size_t>(i) * ch + 2] = 0.2f * static_cast<float>(std::sin(660.0 * base)); // FC
      in[static_cast<size_t>(i) * ch + 3] = 0.1f * static_cast<float>(std::sin(110.0 * base)); // LFE
      in[static_cast<size_t>(i) * ch + 4] = 0.2f * static_cast<float>(std::sin(330.0 * base)); // BL
      in[static_cast<size_t>(i) * ch + 5] = 0.2f * static_cast<float>(std::sin(392.0 * base)); // BR
    }
    t += fpp;
    int n = enc.EncodePacket(reinterpret_cast<uint8_t*>(in.data()), out.data(),
                             static_cast<int>(out.size()));
    if (n > 0) stream.insert(stream.end(), out.data(), out.data() + n);
  }
  REQUIRE(stream.size() > static_cast<size_t>(6144) * 4);

  std::vector<double> rms = DecodeSpdifAc3Rms(stream.data(), static_cast<int>(stream.size()));
  REQUIRE(rms.size() >= 6);

  // All 6 channels should carry meaningful energy (the swr passthrough preserves them).
  MESSAGE("real 5.1 RMS: FL=" << rms[0] << " FR=" << rms[1] << " FC=" << rms[2]
          << " LFE=" << rms[3] << " BL=" << rms[4] << " BR=" << rms[5]);
  for (int c = 0; c < 6; ++c)
    CHECK(rms[c] > 0.01);
}

// Dynamic upmix: native 5.1 center channel dialogue passes through without bleeding to Front L/R
TEST_CASE("dynamic upmix: native 5.1 center channel dialogue passes through without bleeding to Front L/R")
{
  SpdifEncoder::Params p;
  p.sampleRate = 48000;
  p.inSampleFmt = AV_SAMPLE_FMT_FLT;
  p.upmix = SpdifEncoder::Upmix::Surround; // dynamic mode (input is 5.1)
  AVChannelLayout lay51 = AV_CHANNEL_LAYOUT_5POINT1_BACK;
  av_channel_layout_copy(&p.inLayout, &lay51);
  SpdifEncoder enc;
  bool ok = enc.Init(p);
  av_channel_layout_uninit(&p.inLayout);
  REQUIRE(ok);

  const int fpp = enc.FramesPerPacket();
  const int ch = 6;
  std::vector<float> in(static_cast<size_t>(fpp) * ch, 0.0f);
  std::vector<uint8_t> out(SpdifEncoder::kMaxBytesPerPacket);
  std::vector<uint8_t> stream;
  long long t = 0;
  for (int pk = 0; pk < 24; ++pk)
  {
    for (int i = 0; i < fpp; ++i)
    {
      float s = 0.3f * static_cast<float>(std::sin(2.0 * kPi * 440.0 * double(t + i) / 48000.0));
      in[static_cast<size_t>(i) * ch + 0] = 0.0f;
      in[static_cast<size_t>(i) * ch + 1] = 0.0f;
      in[static_cast<size_t>(i) * ch + 2] = s;
      in[static_cast<size_t>(i) * ch + 3] = 0.0f;
      in[static_cast<size_t>(i) * ch + 4] = 0.0f;
      in[static_cast<size_t>(i) * ch + 5] = 0.0f;
    }
    t += fpp;
    int n = enc.EncodePacket(reinterpret_cast<uint8_t*>(in.data()), out.data(),
                             static_cast<int>(out.size()));
    if (n > 0) stream.insert(stream.end(), out.data(), out.data() + n);
  }
  REQUIRE(stream.size() > static_cast<size_t>(6144) * 4);

  std::vector<double> rms = DecodeSpdifAc3Rms(stream.data(), static_cast<int>(stream.size()));
  REQUIRE(rms.size() >= 6);

  MESSAGE("native 5.1 Center dialogue RMS: FL=" << rms[0] << " FR=" << rms[1] << " FC=" << rms[2]
          << " LFE=" << rms[3] << " BL=" << rms[4] << " BR=" << rms[5]);
  CHECK(rms[2] > 0.10);
  CHECK(rms[0] < 0.005);
  CHECK(rms[1] < 0.005);
  CHECK(0.5 * (rms[4] + rms[5]) < 0.005);
}

// 3.0 Front Stage mode: 3.0 sends to Center while keeping LFE and rear surrounds quiet
TEST_CASE("upmix=front30: 3.0 sends to Center while keeping LFE and rear surrounds quiet")
{
  SpdifEncoder::Params p;
  p.sampleRate = 48000;
  p.inSampleFmt = AV_SAMPLE_FMT_FLT;
  p.upmix = SpdifEncoder::Upmix::Front30;
  AVChannelLayout lay = AV_CHANNEL_LAYOUT_STEREO;
  av_channel_layout_copy(&p.inLayout, &lay);
  SpdifEncoder enc;
  bool ok = enc.Init(p);
  av_channel_layout_uninit(&p.inLayout);
  REQUIRE(ok);

  const int fpp = enc.FramesPerPacket();
  std::vector<float> in(static_cast<size_t>(fpp) * 2);
  std::vector<uint8_t> out(SpdifEncoder::kMaxBytesPerPacket);
  std::vector<uint8_t> stream;
  long long t = 0;
  for (int pk = 0; pk < 24; ++pk)
  {
    for (int i = 0; i < fpp; ++i)
    {
      float s = 0.3f * static_cast<float>(std::sin(2.0 * kPi * 440.0 * double(t + i) / 48000.0));
      in[2 * i] = s;
      in[2 * i + 1] = s;
    }
    t += fpp;
    int n = enc.EncodePacket(reinterpret_cast<uint8_t*>(in.data()), out.data(),
                             static_cast<int>(out.size()));
    if (n > 0) stream.insert(stream.end(), out.data(), out.data() + n);
  }
  REQUIRE(stream.size() > static_cast<size_t>(6144) * 4);

  std::vector<double> rms = DecodeSpdifAc3Rms(stream.data(), static_cast<int>(stream.size()));
  REQUIRE(rms.size() >= 6);

  MESSAGE("FrontStage 3.0 RMS: FL=" << rms[0] << " FR=" << rms[1] << " FC=" << rms[2]
          << " LFE=" << rms[3] << " BL=" << rms[4] << " BR=" << rms[5]);
  CHECK(rms[2] > 0.05);
  CHECK(rms[3] < 0.005);
  CHECK(0.5 * (rms[4] + rms[5]) < 0.005);
}

// 2.1 Stereo + Sub: sends bass to LFE while keeping Center and rear surrounds quiet
TEST_CASE("upmix=2.1: 2.1 sends bass to LFE while keeping Center and rear surrounds quiet")
{
  SpdifEncoder::Params p;
  p.sampleRate = 48000;
  p.inSampleFmt = AV_SAMPLE_FMT_FLT;
  p.upmix = SpdifEncoder::Upmix::StereoSub21;
  AVChannelLayout lay = AV_CHANNEL_LAYOUT_STEREO;
  av_channel_layout_copy(&p.inLayout, &lay);
  SpdifEncoder enc;
  bool ok = enc.Init(p);
  av_channel_layout_uninit(&p.inLayout);
  REQUIRE(ok);

  const int fpp = enc.FramesPerPacket();
  std::vector<float> in(static_cast<size_t>(fpp) * 2);
  std::vector<uint8_t> out(SpdifEncoder::kMaxBytesPerPacket);
  std::vector<uint8_t> stream;
  long long t = 0;
  for (int pk = 0; pk < 24; ++pk)
  {
    for (int i = 0; i < fpp; ++i)
    {
      float s = 0.3f * static_cast<float>(std::sin(2.0 * kPi * 80.0 * double(t + i) / 48000.0));
      in[2 * i] = s;
      in[2 * i + 1] = s;
    }
    t += fpp;
    int n = enc.EncodePacket(reinterpret_cast<uint8_t*>(in.data()), out.data(),
                             static_cast<int>(out.size()));
    if (n > 0) stream.insert(stream.end(), out.data(), out.data() + n);
  }
  REQUIRE(stream.size() > static_cast<size_t>(6144) * 4);

  std::vector<double> rms = DecodeSpdifAc3Rms(stream.data(), static_cast<int>(stream.size()));
  REQUIRE(rms.size() >= 6);

  MESSAGE("StereoSub 2.1 RMS: FL=" << rms[0] << " FR=" << rms[1] << " FC=" << rms[2]
          << " LFE=" << rms[3] << " BL=" << rms[4] << " BR=" << rms[5]);
  CHECK(rms[0] > 0.05);
  CHECK(rms[1] > 0.05);
  CHECK(rms[3] > 0.02);
  CHECK(rms[2] < 0.005);
  CHECK(0.5 * (rms[4] + rms[5]) < 0.005);
}

// Dynamic upmix DTS 512-frame loopback: zero packet dropouts across 64 packets
TEST_CASE("dynamic upmix DTS 512-frame loopback: zero packet dropouts across 64 packets")
{
  SpdifEncoder::Params p;
  p.codec = SpdifEncoder::Codec::DTS;
  p.sampleRate = 48000;
  p.inSampleFmt = AV_SAMPLE_FMT_FLT;
  p.upmix = SpdifEncoder::Upmix::Surround;
  AVChannelLayout lay51 = AV_CHANNEL_LAYOUT_5POINT1_BACK;
  av_channel_layout_copy(&p.inLayout, &lay51);
  SpdifEncoder enc;
  bool ok = enc.Init(p);
  av_channel_layout_uninit(&p.inLayout);
  REQUIRE(ok);

  const int fpp = enc.FramesPerPacket();
  CHECK(fpp == 512);
  const int ch = 6;
  std::vector<float> in(static_cast<size_t>(fpp) * ch, 0.0f);
  std::vector<uint8_t> out(SpdifEncoder::kMaxBytesPerPacket);
  long long t = 0;
  for (int pk = 0; pk < 64; ++pk)
  {
    for (int i = 0; i < fpp; ++i)
    {
      float s = 0.3f * static_cast<float>(std::sin(2.0 * kPi * 440.0 * double(t + i) / 48000.0));
      in[static_cast<size_t>(i) * ch + 0] = s;
      in[static_cast<size_t>(i) * ch + 1] = -s;
      in[static_cast<size_t>(i) * ch + 2] = 0.0f;
      in[static_cast<size_t>(i) * ch + 3] = 0.0f;
      in[static_cast<size_t>(i) * ch + 4] = 0.0f;
      in[static_cast<size_t>(i) * ch + 5] = 0.0f;
    }
    t += fpp;
    int n = enc.EncodePacket(reinterpret_cast<uint8_t*>(in.data()), out.data(),
                             static_cast<int>(out.size()));
    if (pk >= 2)
    {
      CHECK(n == 2048);
    }
  }
}

// Test Upmix::Always forces upmixing on all inputs
TEST_CASE("upmix always: forces surround upmix unconditionally")
{
  SpdifEncoder::Params p;
  p.sampleRate = 48000;
  p.inSampleFmt = AV_SAMPLE_FMT_FLT;
  p.upmix = SpdifEncoder::Upmix::Always;
  AVChannelLayout lay51 = AV_CHANNEL_LAYOUT_5POINT1_BACK;
  av_channel_layout_copy(&p.inLayout, &lay51);
  SpdifEncoder enc;
  bool ok = enc.Init(p);
  av_channel_layout_uninit(&p.inLayout);
  REQUIRE(ok);

  const int fpp = enc.FramesPerPacket();
  const int ch = 6;
  std::vector<float> in(static_cast<size_t>(fpp) * ch, 0.0f);
  std::vector<uint8_t> out(SpdifEncoder::kMaxBytesPerPacket);
  std::vector<uint8_t> stream;
  long long t = 0;
  for (int pk = 0; pk < 24; ++pk)
  {
    for (int i = 0; i < fpp; ++i)
    {
      float s = 0.3f * static_cast<float>(std::sin(2.0 * kPi * 440.0 * double(t + i) / 48000.0));
      in[static_cast<size_t>(i) * ch + 0] = s;
      in[static_cast<size_t>(i) * ch + 1] = -s;
      in[static_cast<size_t>(i) * ch + 2] = s;
      in[static_cast<size_t>(i) * ch + 3] = 0.0f;
      in[static_cast<size_t>(i) * ch + 4] = 0.05f * s; // Even with slight rear signal
      in[static_cast<size_t>(i) * ch + 5] = 0.05f * s;
    }
    t += fpp;
    int n = enc.EncodePacket(reinterpret_cast<uint8_t*>(in.data()), out.data(),
                             static_cast<int>(out.size()));
    if (n > 0) stream.insert(stream.end(), out.data(), out.data() + n);
  }
  REQUIRE(stream.size() > static_cast<size_t>(6144) * 4);

  std::vector<double> rms = DecodeSpdifAc3Rms(stream.data(), static_cast<int>(stream.size()));
  REQUIRE(rms.size() >= 6);
  CHECK(0.5 * (rms[4] + rms[5]) > 0.03);
}

// 3.1 Front Stage mode: sends dialogue/vocals to Center and bass to Subwoofer,
// while keeping rear surround speakers completely quiet (sur_out=0).
TEST_CASE("upmix=frontstage: 3.1 sends to Center while keeping rear surrounds quiet")
{
  SpdifEncoder::Params p;
  p.sampleRate = 48000;
  p.inSampleFmt = AV_SAMPLE_FMT_FLT;
  p.upmix = SpdifEncoder::Upmix::FrontStage;
  AVChannelLayout lay = AV_CHANNEL_LAYOUT_STEREO;
  av_channel_layout_copy(&p.inLayout, &lay);
  SpdifEncoder enc;
  bool ok = enc.Init(p);
  av_channel_layout_uninit(&p.inLayout);
  REQUIRE(ok);

  const int fpp = enc.FramesPerPacket();
  std::vector<float> in(static_cast<size_t>(fpp) * 2);
  std::vector<uint8_t> out(SpdifEncoder::kMaxBytesPerPacket);
  std::vector<uint8_t> stream;
  long long t = 0;
  for (int pk = 0; pk < 24; ++pk)
  {
    for (int i = 0; i < fpp; ++i)
    {
      float s = 0.3f * static_cast<float>(std::sin(2.0 * kPi * 440.0 * double(t + i) / 48000.0));
      in[2 * i] = s;     // L
      in[2 * i + 1] = s; // R (in-phase mono/vocal dialogue)
    }
    t += fpp;
    int n = enc.EncodePacket(reinterpret_cast<uint8_t*>(in.data()), out.data(),
                             static_cast<int>(out.size()));
    if (n > 0) stream.insert(stream.end(), out.data(), out.data() + n);
  }
  REQUIRE(stream.size() > static_cast<size_t>(6144) * 4);

  std::vector<double> rms = DecodeSpdifAc3Rms(stream.data(), static_cast<int>(stream.size()));
  REQUIRE(rms.size() >= 6);

  MESSAGE("FrontStage 3.1 RMS: FL=" << rms[0] << " FR=" << rms[1] << " FC=" << rms[2]
          << " LFE=" << rms[3] << " BL=" << rms[4] << " BR=" << rms[5]);
  // Center speaker must receive strong dialogue audio
  CHECK(rms[2] > 0.05);
  // Rear surrounds must be quiet (sur_out=0)
  CHECK(0.5 * (rms[4] + rms[5]) < 0.005);
}
