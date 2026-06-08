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
