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
#include <array>
#include <atomic>

#include <string>

#include "DtsBitstream.h"
#include "Limiter.h"
#include "WavDump.h"
#include "dsp/DspChain.h"

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
  enum class Codec { AC3, DTS };

  SpdifEncoder();
  ~SpdifEncoder();
  SpdifEncoder(const SpdifEncoder&) = delete;
  SpdifEncoder& operator=(const SpdifEncoder&) = delete;

  // Stereo->5.1 upmix mode.
  // Off/Surround only apply when the input has <= 2 channels; multichannel input is otherwise
  // downmixed/passed through to 5.1 by swr.
  // Auto/Always are for multichannel input (e.g. a 5.1 virtual device such as VB-CABLE): stereo
  // material (browser, music player) then only fills FL/FR while C/LFE/SL/SR stay silent.
  enum class Upmix
  {
    Off,       // swr default rematrix (front channels only-ish)
    Surround,  // FFmpeg `surround` libavfilter (FFT-based steered upmix)
    Auto,      // Surround for <= 2ch input; for multichannel input: pass 5.1 through, but upmix
               // FL/FR when the centre and surround channels are silent (stereo-only content)
    Always,    // Surround for <= 2ch input; for multichannel input: always upmix FL/FR only
  };

  struct Params
  {
    Codec codec = Codec::AC3;
    int sampleRate = 48000;                       // AC3: 48000 / 44100 / 32000
    int64_t bitRate = 640000;                     // AC3-over-optical maximum
    AVSampleFormat inSampleFmt = AV_SAMPLE_FMT_FLT; // interleaved input from WASAPI/WAV
    AVChannelLayout inLayout{};                   // caller-owned; copied in Init().
                                                  // Any layout; downmixed to 5.1 by swr.
    Upmix upmix = Upmix::Off;                     // stereo->5.1 upmix mode
    float headroomDb = -6.0f;                     // fixed trim before the encoder (-24..0 dB)
    bool  limiter = true;                         // linked look-ahead peak limiter before the encoder
    float limiterCeilingDb = -1.0f;               // limiter ceiling (-12..0 dBFS)
    int   upmixWinSize = 0;                       // surround filter FFT window: 0 = default (4096), 2048, 1024
    bool  matrixUpmix = false;                    // upmix (unless Upmix::Off) with the time-domain matrix in the
                                                  // DSP chain instead of the FFT filter (no extra delay)
    std::string dumpPath;                         // diagnostics: "<path>_input.wav" (raw input) and
                                                  // "<path>_encoder_in.wav" (what the encoder gets),
                                                  // first 20 s, written when the encoder is closed
    bool  dtsBitstreamIn = false;                 // Dolby Digital output only: if the input carries a
                                                  // DTS bitstream (IEC 61937 in FL/FR), decode it
                                                  // with FFmpeg and encode the 5.1 to AC3
  };

  // Open the encoder. Returns false (and logs to stderr) on failure.
  bool Init(const Params& p);
  // Per-output-channel trim. Values are clamped to -60..0 dB; -60 dB is digital mute.
  void SetChannelGainsDb(const std::array<float, 6>& gainsDb);
  // DSP (30-band equalizer + compressor), applied to the 5.1 frame before the trims and the limiter.
  // Thread-safe; the default parameters are an exact bypass.
  void SetDspParams(const vac3dsp::DspParams& p) { dsp_.SetParams(p); }
  vac3dsp::DspMeters TakeDspMeters() { return dsp_.TakeMeters(); }
  vac3dsp::DspChain& DspChainRef() { return dsp_; }   // spectrum taps for DSP Studio
  // Peak of the final signal per channel (after trims, headroom and limiter) since the last call, for the
  // level meters of the control panel. `out` has 6 values.
  void TakeFinalPeaks(float* out)
  {
    for (size_t c = 0; c < finalPeak_.size(); ++c)
      out[c] = finalPeak_[c].exchange(0.0f, std::memory_order_relaxed);
  }
  // Deepest limiter gain reduction (dB, >= 0) since the last call; 0 when it never engaged.
  float TakeLimiterReductionDb() { return limiterOn_ ? limiter_.TakeMaxReductionDb() : 0.0f; }
  // Packets (since the last call) whose input already touched 0 dBFS: the source is clipped before
  // it reaches the encoder (Windows mixer / player volume), which no encoder-side headroom can fix.
  uint32_t TakeInputFullScalePackets() { return inputFullScale_.exchange(0, std::memory_order_relaxed); }
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

  // Auto-upmix helpers (multichannel input + Upmix::Auto / Always)
  void AnalyzeChannels(const uint8_t* in, float& frontPeak, float& restPeak) const;
  void UpdateAutoUpmixState(const uint8_t* in);
  void ExtractFrontPair(const uint8_t* in);
  void MixAutoFrames(bool upReady);

  // DTS bitstream input helpers
  void ExtractDtsCarrierWords(const uint8_t* in);
  float InputPeak(const uint8_t* in) const;
  int  PullDtsFrame(const uint8_t* in); // 0 = not DTS (use PCM path), 1 = frame_ filled, 2 = wait
  int  EncodeFrameToBurst(uint8_t* outBuf, int outSize); // frame_ -> AC3 -> IEC61937
  void ApplyChannelGains();

  AVCodecContext*  codecCtx_ = nullptr;
  AVFormatContext* muxer_    = nullptr;
  SwrContext*      swr_      = nullptr;
  SwrContext*      dtsS32Swr_ = nullptr; // FLTP to packed S32 for DTS
  AVFrame*         frame_    = nullptr;  // planar-float processing frame
  AVFrame*         dtsFrame_  = nullptr;  // packed S32 frame for the DTS encoder
  AVPacket*        pkt_      = nullptr;

  // surround-upmix pipeline (used when upmix == Surround and input is <= 2ch)
  bool             useFilter_ = false;
  bool             useDts_ = false;
  AVFilterGraph*   graph_     = nullptr;
  AVFilterContext* fsrc_      = nullptr; // abuffer
  AVFilterContext* fsink_     = nullptr; // abuffersink
  AVAudioFifo*     fifo_      = nullptr; // accumulates filtered 5.1 fltp samples
  AVFrame*         filtFrame_ = nullptr; // scratch for draining the sink

  // Auto-upmix: the surround graph is fed with FL/FR only and runs on every packet (always warm);
  // the encoder gets either the pass-through 5.1 (frame_) or the upmixed 5.1 (upFrame_), with a
  // 10 ms crossfade when the decision changes.
  bool             autoUpmix_ = false;
  bool             autoForce_ = false;       // Upmix::Always
  bool             upmixActive_ = false;     // current decision: false = pass-through, true = upmix
  float            upmixMix_ = 0.0f;         // crossfade position 0 (pass-through) .. 1 (upmix)
  int              quietSamples_ = 0;        // consecutive stereo-only samples seen
  int              frontIdx_[2] = {0, 1};    // interleaved index of FL / FR in the input
  int              lfeIdx_ = -1;             // interleaved index of LFE (ignored by the detector)
  AVChannelLayout  filterInLayout_{};        // layout fed to the surround graph
  AVFrame*         upFrame_ = nullptr;       // upmixed planar-float 5.1 of the current packet
  std::vector<uint8_t> stereoScratch_;       // FL/FR extracted from the multichannel input

  // DTS bitstream input (see Params::dtsBitstreamIn)
  int              upmixWinSize_ = 0;
  std::atomic<uint32_t> inputFullScale_{0};
  WavDump          dumpIn_;                  // raw interleaved input
  WavDump          dumpEnc_;                 // interleaved float 5.1 handed to the encoder
  std::vector<float> dumpScratch_;
  bool             dtsIn_ = false;
  bool             dtsPrimed_ = false;       // >= 2 packets decoded, steady-state reading
  int              dtsFlIdx_ = 0;            // interleaved index of FL / FR in the input
  int              dtsFrIdx_ = 1;
  DtsBitstreamDecoder dtsDec_;
  std::vector<int16_t> dtsWords_;            // FL/FR of the current packet as 16-bit carrier words

  AVChannelLayout  inLayout_{};
  AVSampleFormat   inSampleFmt_ = AV_SAMPLE_FMT_FLT;
  int              sampleRate_ = 48000;
  int              framesPerPacket_ = 0;
  std::array<std::atomic<float>, 6> channelGainLinear_;
  std::array<std::atomic<float>, 6> finalPeak_{};
  float            headroomLinear_ = 0.5011872f; // -6 dB, set in Init()
  bool             limiterOn_ = false;
  LookaheadLimiter limiter_;
  vac3dsp::DspChain dsp_;
  int64_t          nextPts_ = 0;

  // Scratch state used by OnWritePacket() during a single EncodePacket() call.
  uint8_t* writeDst_ = nullptr;
  int      writeCap_ = 0;
  int      writeLen_ = 0;
};
