// SpdifEncoder.h
//
// Real-time PCM -> AC3 (Dolby Digital) -> IEC 61937 (S/PDIF) burst encoder.
//
// Ported from SoundPusher's SPDIFAudioEncoder (MIT, Daniel Vollmer) and Kodi's
// CAEEncoderFFmpeg (GPL), modernized for FFmpeg 7.x / 8.x:
//   - new AVChannelLayout API (instead of the removed channel_layout/channels fields)
//   - avcodec_send_frame / avcodec_receive_packet (instead of codec->encode2)
//   - swr_alloc_set_opts2
//
// Design (from SoundPusher): swresample converts/downmixes the interleaved input to the
// AC3 encoder's required planar-float 5.1 format; libavcodec encodes one 1536-frame AC3
// packet; libavformat's "spdif" muxer wraps it into an IEC 61937 burst via a custom AVIO
// write callback that drops the burst straight into the caller's output buffer.
#pragma once

#include <cstdint>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

class SpdifEncoder
{
public:
  SpdifEncoder() = default;
  ~SpdifEncoder();
  SpdifEncoder(const SpdifEncoder&) = delete;
  SpdifEncoder& operator=(const SpdifEncoder&) = delete;

  // Stereo->5.1 upmix mode. Only applies when the input has <= 2 channels; multichannel
  // input is always downmixed to 5.1 (by swr) regardless.
  enum class Upmix
  {
    Off,       // swr default rematrix (front channels only-ish)
    Surround,  // FFmpeg `surround` libavfilter (FFT-based steered upmix)
  };

  struct Params
  {
    int sampleRate = 48000;                       // AC3: 48000 / 44100 / 32000
    int64_t bitRate = 640000;                     // AC3-over-optical maximum
    AVSampleFormat inSampleFmt = AV_SAMPLE_FMT_FLT; // interleaved input from WASAPI/WAV
    AVChannelLayout inLayout{};                   // caller-owned; copied in Init().
                                                  // Any layout; downmixed to 5.1 by swr.
    Upmix upmix = Upmix::Off;                     // stereo->5.1 upmix mode
  };

  // Open the encoder. Returns false (and logs to stderr) on failure.
  bool Init(const Params& p);
  void Close();
  bool IsOpen() const { return codecCtx_ != nullptr; }

  // Input sample-frames consumed per EncodePacket() call (AC3 == 1536).
  int FramesPerPacket() const { return framesPerPacket_; }
  // Channels expected in the interleaved input (== inLayout.nb_channels).
  int InChannels() const { return inLayout_.nb_channels; }

  // An IEC 61937 AC3 burst is always 6144 bytes (1536 frames * 2ch * 16-bit).
  static constexpr int kMaxBytesPerPacket = 6144;

  // Encode exactly FramesPerPacket() interleaved input frames into one IEC 61937 burst.
  //   in      : FramesPerPacket() * InChannels() samples in Params::inSampleFmt
  //   outBuf  : destination for the S/PDIF burst
  //   outSize : capacity of outBuf (must be >= kMaxBytesPerPacket)
  // Returns bytes written (typically 6144), 0 if the encoder produced no packet, <0 on error.
  int EncodePacket(const uint8_t* in, uint8_t* outBuf, int outSize);

private:
  // FFmpeg >= 6.1 AVIO write callback signature uses const uint8_t*.
  static int WritePacketThunk(void* opaque, const uint8_t* buf, int buf_size);
  int OnWritePacket(const uint8_t* buf, int buf_size);

  bool BuildFilterGraph();              // surround-upmix path
  bool FeedFilter(const uint8_t* in);  // push one input packet, drain output into fifo_
  int  EncodeFrameToBurst(uint8_t* outBuf, int outSize); // frame_ -> AC3 -> IEC61937

  AVCodecContext*  codecCtx_ = nullptr;
  AVFormatContext* muxer_    = nullptr;
  SwrContext*      swr_      = nullptr;
  AVFrame*         frame_    = nullptr;  // planar-float scratch fed to the encoder
  AVPacket*        pkt_      = nullptr;

  // surround-upmix pipeline (used when upmix == Surround and input is <= 2ch)
  bool             useFilter_ = false;
  AVFilterGraph*   graph_     = nullptr;
  AVFilterContext* fsrc_      = nullptr; // abuffer
  AVFilterContext* fsink_     = nullptr; // abuffersink
  AVAudioFifo*     fifo_      = nullptr; // accumulates filtered 5.1 fltp samples
  AVFrame*         filtFrame_ = nullptr; // scratch for draining the sink

  AVChannelLayout  inLayout_{};
  AVSampleFormat   inSampleFmt_ = AV_SAMPLE_FMT_FLT;
  int              sampleRate_ = 48000;
  int              framesPerPacket_ = 0;
  int64_t          nextPts_ = 0;

  // Scratch state used by OnWritePacket() during a single EncodePacket() call.
  uint8_t* writeDst_ = nullptr;
  int      writeCap_ = 0;
  int      writeLen_ = 0;
};
