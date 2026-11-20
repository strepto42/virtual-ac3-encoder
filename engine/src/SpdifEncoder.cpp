// SpdifEncoder.cpp  — see SpdifEncoder.h for design notes.

#include "SpdifEncoder.h"
#include "BootTrace.h"

#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>
#include <cmath>

extern "C" {
#include <libavutil/error.h>
#include <libavutil/samplefmt.h>
}

namespace {

void LogAv(const char* what, int err)
{
  char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
  av_strerror(err, buf, sizeof buf);
  std::fprintf(stderr, "[SpdifEncoder] %s failed: %s (%d)\n", what, buf, err);
}

} // namespace

SpdifEncoder::SpdifEncoder()
{
  for (auto& gain : channelGainLinear_)
    gain.store(1.0f, std::memory_order_relaxed);
}

void SpdifEncoder::SetChannelGainsDb(const std::array<float, 6>& gainsDb)
{
  for (size_t i = 0; i < channelGainLinear_.size(); ++i)
  {
    const float db = std::clamp(gainsDb[i], -60.0f, 0.0f);
    const float linear = (db <= -60.0f) ? 0.0f : std::pow(10.0f, db / 20.0f);
    channelGainLinear_[i].store(linear, std::memory_order_relaxed);
  }
}

void SpdifEncoder::ApplyChannelGains()
{
  if (!frame_ || !frame_->extended_data || !codecCtx_)
    return;
  // DSP first (equalizer + compressor): it sees the signal as it arrives, before the trims, the
  // headroom and the limiter, so the limiter stays the guard against anything the DSP boosts.
  static int traceCalls = 0;
  const bool trace = traceCalls < 3;
  if (trace) { ++traceCalls; VAC3_TRACE("[trace] ApplyChannelGains #%d: DSP begin (%d samples)\n", traceCalls, frame_->nb_samples); }
  dsp_.Process(reinterpret_cast<float* const*>(frame_->extended_data), frame_->nb_samples);
  if (trace) VAC3_TRACE("[trace] ApplyChannelGains #%d: DSP done\n", traceCalls);
  const int channels = std::min(6, codecCtx_->ch_layout.nb_channels);
  for (int ch = 0; ch < channels; ++ch)
  {
    // User trim (0..-60 dB, live from the GUI) times the fixed encoder headroom.
    const float gain = channelGainLinear_[static_cast<size_t>(ch)].load(std::memory_order_relaxed) *
                       headroomLinear_;
    if (gain >= 0.999999f)
      continue;
    float* samples = reinterpret_cast<float*>(frame_->extended_data[ch]);
    if (!samples)
      continue;
    if (gain <= 0.0f)
      std::fill_n(samples, frame_->nb_samples, 0.0f);
    else
      for (int i = 0; i < frame_->nb_samples; ++i)
        samples[i] *= gain;
  }

  // Last stage before the encoder: linked look-ahead limiter (never lets a sample exceed the
  // ceiling, so neither AC3 nor the float->S32 DTS conversion can clip).
  if (limiterOn_)
    limiter_.Process(reinterpret_cast<float* const*>(frame_->extended_data), frame_->nb_samples);

  // Level meters (control panel): the peak per channel of what finally goes to the encoder.
  for (int ch = 0; ch < channels; ++ch)
  {
    const float* s = reinterpret_cast<const float*>(frame_->extended_data[ch]);
    if (!s)
      continue;
    float m = 0.0f;
    for (int i = 0; i < frame_->nb_samples; ++i)
      m = std::max(m, std::fabs(s[i]));
    auto& peak = finalPeak_[static_cast<size_t>(ch)];
    if (m > peak.load(std::memory_order_relaxed))
      peak.store(m, std::memory_order_relaxed);
  }
}

SpdifEncoder::~SpdifEncoder()
{
  Close();
}

bool SpdifEncoder::Init(const Params& p)
{
  Close();

  av_channel_layout_copy(&inLayout_, &p.inLayout);
  inSampleFmt_ = p.inSampleFmt;
  sampleRate_ = p.sampleRate;
  nextPts_ = 0;

  useDts_ = (p.codec == Codec::DTS);
  const AVCodecID codecId = useDts_ ? AV_CODEC_ID_DTS : AV_CODEC_ID_AC3;
  const AVCodec* codec = avcodec_find_encoder(codecId);
  if (!codec)
  {
    std::fprintf(stderr, "[SpdifEncoder] %s encoder not found in this FFmpeg build\n", useDts_ ? "DTS" : "AC3");
    return false;
  }

  codecCtx_ = avcodec_alloc_context3(codec);
  if (!codecCtx_)
    return false;

  codecCtx_->bit_rate = useDts_ ? 768000 : p.bitRate;
  codecCtx_->sample_rate = p.sampleRate;
  codecCtx_->sample_fmt = useDts_ ? AV_SAMPLE_FMT_S32 : AV_SAMPLE_FMT_FLTP; // DTS requires packed S32
  codecCtx_->time_base = av_make_q(1, p.sampleRate);
  if (useDts_)
    codecCtx_->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;

  // The encoder (and S/PDIF) carry 5.1. Prefer the "back" variant to match Windows'
  // KSAUDIO_SPEAKER_5POINT1 (FL FR FC LFE BL BR); fall back to "side" 5.1 if the
  // encoder build rejects it.
  AVChannelLayout enc51back = AV_CHANNEL_LAYOUT_5POINT1_BACK;
  AVChannelLayout enc51side = AV_CHANNEL_LAYOUT_5POINT1;
  av_channel_layout_copy(&codecCtx_->ch_layout, useDts_ ? &enc51side : &enc51back);

  int rc = avcodec_open2(codecCtx_, codec, nullptr);
  if (rc < 0)
  {
    av_channel_layout_uninit(&codecCtx_->ch_layout);
    av_channel_layout_copy(&codecCtx_->ch_layout, useDts_ ? &enc51back : &enc51side);
    rc = avcodec_open2(codecCtx_, codec, nullptr);
    if (rc < 0) { LogAv("avcodec_open2", rc); return false; }
  }

  framesPerPacket_ = codecCtx_->frame_size; // 1536 for AC3

  // Anti-clipping: fixed headroom trim + linked peak limiter (see ApplyChannelGains()).
  headroomLinear_ = std::pow(10.0f, std::clamp(p.headroomDb, -24.0f, 0.0f) / 20.0f);
  limiterOn_ = p.limiter;
  if (limiterOn_)
    limiter_.Configure(codecCtx_->ch_layout.nb_channels, sampleRate_,
                       std::clamp(p.limiterCeilingDb, -12.0f, 0.0f));
  std::printf("[SpdifEncoder] headroom %.1f dB, limiter %s (ceiling %.1f dBFS)\n",
              std::clamp(p.headroomDb, -24.0f, 0.0f), limiterOn_ ? "on" : "off",
              std::clamp(p.limiterCeilingDb, -12.0f, 0.0f));
  // DSP chain (bypass until dsp-settings.json asks for something).
  dsp_.Configure(codecCtx_->ch_layout.nb_channels, sampleRate_);
  // Matrix upmix: the DSP chain turns the stereo that swr leaves in FL/FR into 5.1 (no filter graph, no
  // look-ahead delay). Stereo input is always upmixed; multichannel input as for Auto / Always.
  const bool matrixUpmix = p.matrixUpmix && p.upmix != Upmix::Off;
  dsp_.ConfigureUpmix(matrixUpmix, p.upmix == Upmix::Always || p.inLayout.nb_channels <= 2);
  if (matrixUpmix)
    std::printf("[SpdifEncoder] upmix: time-domain matrix (no look-ahead delay; settings in dsp-settings.json)\n");
  VAC3_TRACE("[trace] SpdifEncoder: DSP chain configured (%d ch, %d Hz)\n", codecCtx_->ch_layout.nb_channels, sampleRate_);

  // Scratch frame holding the planar-float 5.1 samples handed to the encoder. Both the swr
  // path and the surround-filter path fill this.
  frame_ = av_frame_alloc();
  if (!frame_) return false;
  frame_->format = AV_SAMPLE_FMT_FLTP;
  av_channel_layout_copy(&frame_->ch_layout, &codecCtx_->ch_layout);
  frame_->sample_rate = sampleRate_;
  frame_->nb_samples = framesPerPacket_;
  rc = av_frame_get_buffer(frame_, 0);
  if (rc < 0) { LogAv("av_frame_get_buffer", rc); return false; }
  if (useDts_)
  {
    dtsFrame_ = av_frame_alloc();
    if (!dtsFrame_) return false;
    dtsFrame_->format = AV_SAMPLE_FMT_S32;
    av_channel_layout_copy(&dtsFrame_->ch_layout, &codecCtx_->ch_layout);
    dtsFrame_->sample_rate = sampleRate_;
    dtsFrame_->nb_samples = framesPerPacket_;
    rc = av_frame_get_buffer(dtsFrame_, 0);
    if (rc < 0) { LogAv("av_frame_get_buffer(DTS)", rc); return false; }

    rc = swr_alloc_set_opts2(&dtsS32Swr_,
                             &codecCtx_->ch_layout, AV_SAMPLE_FMT_S32, sampleRate_,
                             &codecCtx_->ch_layout, AV_SAMPLE_FMT_FLTP, sampleRate_,
                             0, nullptr);
    if (rc < 0 || !dtsS32Swr_) { LogAv("swr_alloc_set_opts2(DTS)", rc); return false; }
    rc = swr_init(dtsS32Swr_);
    if (rc < 0) { LogAv("swr_init(DTS)", rc); return false; }
  }

  // Input conditioning: either the `surround` upmix filter (stereo->5.1) or plain swr
  // convert/downmix. Surround only applies to <= 2ch input; multichannel always downmixes.
  upmixWinSize_ = (p.upmixWinSize == 1024 || p.upmixWinSize == 2048) ? p.upmixWinSize : 0;
  const bool wantsUpmix = !matrixUpmix && (p.upmix == Upmix::Surround || p.upmix == Upmix::Auto ||
                                           p.upmix == Upmix::Always);
  useFilter_ = (wantsUpmix && inLayout_.nb_channels <= 2);
  if (useFilter_)
  {
    av_channel_layout_copy(&filterInLayout_, &inLayout_);
    if (!BuildFilterGraph())
    {
      std::fprintf(stderr, "[SpdifEncoder] surround upmix unavailable; using plain upmix/downmix\n");
      useFilter_ = false;
    }
  }
  if (!useFilter_)
  {
    // swr: interleaved input -> planar-float 5.1 (encoder layout), same sample rate.
    rc = swr_alloc_set_opts2(&swr_, &codecCtx_->ch_layout, AV_SAMPLE_FMT_FLTP, sampleRate_,
                             &inLayout_, inSampleFmt_, sampleRate_, 0, nullptr);
    if (rc < 0 || !swr_) { LogAv("swr_alloc_set_opts2", rc); return false; }
    rc = swr_init(swr_);
    if (rc < 0) { LogAv("swr_init", rc); return false; }
  }

  // Auto-upmix for multichannel input: keep swr as the pass-through path and add a surround
  // graph that is fed with FL/FR only (see EncodePacket()).
  if (!matrixUpmix && (p.upmix == Upmix::Auto || p.upmix == Upmix::Always) && inLayout_.nb_channels > 2 && !useFilter_)
  {
    const int fl = av_channel_layout_index_from_channel(&inLayout_, AV_CHAN_FRONT_LEFT);
    const int fr = av_channel_layout_index_from_channel(&inLayout_, AV_CHAN_FRONT_RIGHT);
    const bool fmtOk = inSampleFmt_ == AV_SAMPLE_FMT_FLT || inSampleFmt_ == AV_SAMPLE_FMT_S16 ||
                       inSampleFmt_ == AV_SAMPLE_FMT_S32;
    if (fl < 0 || fr < 0 || !fmtOk)
    {
      std::fprintf(stderr, "[SpdifEncoder] auto-upmix unavailable (input has no FL/FR or unsupported "
                           "sample format); passing the input through\n");
    }
    else
    {
      frontIdx_[0] = fl;
      frontIdx_[1] = fr;
      lfeIdx_ = av_channel_layout_index_from_channel(&inLayout_, AV_CHAN_LOW_FREQUENCY);
      av_channel_layout_default(&filterInLayout_, 2);
      upFrame_ = av_frame_alloc();
      bool ok = (upFrame_ != nullptr);
      if (ok)
      {
        upFrame_->format = AV_SAMPLE_FMT_FLTP;
        av_channel_layout_copy(&upFrame_->ch_layout, &codecCtx_->ch_layout);
        upFrame_->sample_rate = sampleRate_;
        upFrame_->nb_samples = framesPerPacket_;
        ok = av_frame_get_buffer(upFrame_, 0) >= 0;
      }
      stereoScratch_.assign(static_cast<size_t>(framesPerPacket_) * 2 *
                            static_cast<size_t>(av_get_bytes_per_sample(inSampleFmt_)), 0);
      if (ok && BuildFilterGraph())
      {
        autoUpmix_ = true;
        autoForce_ = (p.upmix == Upmix::Always);
        upmixActive_ = autoForce_;
        std::printf("[SpdifEncoder] auto-upmix %s: stereo-only 5.1 input is upmixed from FL/FR\n",
                    autoForce_ ? "ALWAYS on" : "armed");
      }
      else
      {
        std::fprintf(stderr, "[SpdifEncoder] auto-upmix unavailable; passing the input through\n");
        if (graph_) { avfilter_graph_free(&graph_); fsrc_ = fsink_ = nullptr; }
        if (fifo_) { av_audio_fifo_free(fifo_); fifo_ = nullptr; }
        if (filtFrame_) av_frame_free(&filtFrame_);
        if (upFrame_) av_frame_free(&upFrame_);
      }
    }
  }

  // Diagnostics: record the raw input and the encoder input to WAV files (first 20 s).
  if (!p.dumpPath.empty())
  {
    const int bits = inSampleFmt_ == AV_SAMPLE_FMT_S16 ? 16 : 32;
    const int tag = inSampleFmt_ == AV_SAMPLE_FMT_FLT ? 3 : 1;
    dumpIn_.Configure(p.dumpPath + "_input.wav", inLayout_.nb_channels, sampleRate_, bits, tag, 20.0);
    dumpEnc_.Configure(p.dumpPath + "_encoder_in.wav", codecCtx_->ch_layout.nb_channels, sampleRate_,
                       32, 3, 20.0);
    dumpScratch_.assign(static_cast<size_t>(framesPerPacket_) *
                        static_cast<size_t>(codecCtx_->ch_layout.nb_channels), 0.0f);
    std::printf("[SpdifEncoder] diagnostics: recording the first 20 s to %s_input.wav / "
                "%s_encoder_in.wav (written when the encoder stops)\n",
                p.dumpPath.c_str(), p.dumpPath.c_str());
  }

  // DTS bitstream input: the capture's FL/FR carry IEC 61937 DTS bursts (e.g. VLC passthrough into a
  // virtual cable). Detected per packet; PCM input keeps using the normal path.
  if (p.dtsBitstreamIn)
  {
    const int fl = av_channel_layout_index_from_channel(&inLayout_, AV_CHAN_FRONT_LEFT);
    const int fr = av_channel_layout_index_from_channel(&inLayout_, AV_CHAN_FRONT_RIGHT);
    const bool fmtOk = inSampleFmt_ == AV_SAMPLE_FMT_FLT || inSampleFmt_ == AV_SAMPLE_FMT_S16 ||
                       inSampleFmt_ == AV_SAMPLE_FMT_S32;
    if (useDts_)
      std::fprintf(stderr, "[SpdifEncoder] dts_in needs Dolby Digital output; ignored for DTS output\n");
    else if (fl < 0 || fr < 0 || !fmtOk)
      std::fprintf(stderr, "[SpdifEncoder] dts_in unavailable (input has no FL/FR or unsupported format)\n");
    else if (dtsDec_.Init(codecCtx_->ch_layout, sampleRate_))
    {
      dtsIn_ = true;
      dtsFlIdx_ = fl;
      dtsFrIdx_ = fr;
      dtsWords_.assign(static_cast<size_t>(framesPerPacket_) * 2, 0);
      std::printf("[SpdifEncoder] DTS bitstream input armed (detected automatically; PCM keeps working)\n");
    }
  }

  pkt_ = av_packet_alloc();
  if (!pkt_) return false;

  // S/PDIF muxer with a custom AVIO sink so each burst lands in the caller's buffer.
  rc = avformat_alloc_output_context2(&muxer_, nullptr, "spdif", nullptr);
  if (rc < 0 || !muxer_) { LogAv("avformat_alloc_output_context2(spdif)", rc); return false; }

  AVStream* st = avformat_new_stream(muxer_, nullptr);
  if (!st) { std::fprintf(stderr, "[SpdifEncoder] avformat_new_stream failed\n"); return false; }
  st->id = 0;
  st->time_base = codecCtx_->time_base;
  rc = avcodec_parameters_from_context(st->codecpar, codecCtx_);
  if (rc < 0) { LogAv("avcodec_parameters_from_context", rc); return false; }

  // The muxer never needs more than one burst at a time.
  unsigned char* avioBuf = static_cast<unsigned char*>(av_malloc(kMaxBytesPerPacket));
  if (!avioBuf) return false;
  muxer_->pb = avio_alloc_context(avioBuf, kMaxBytesPerPacket, /*write_flag=*/1, this,
                                  /*read=*/nullptr, &WritePacketThunk, /*seek=*/nullptr);
  if (!muxer_->pb) { av_free(avioBuf); return false; }
  muxer_->flags |= AVFMT_FLAG_CUSTOM_IO;

  rc = avformat_write_header(muxer_, nullptr);
  if (rc < 0) { LogAv("avformat_write_header(spdif)", rc); return false; }

  return true;
}

void SpdifEncoder::Close()
{
  if (dumpIn_.enabled() || dumpEnc_.enabled())
  {
    const bool a = dumpIn_.Write();
    const bool b = dumpEnc_.Write();
    std::printf("[SpdifEncoder] diagnostics WAV files %s\n", (a && b) ? "written" : "could NOT be written");
  }
  if (muxer_)
  {
    if (muxer_->pb) // a header was written iff pb exists
      av_write_trailer(muxer_);
    if (muxer_->pb)
    {
      av_freep(&muxer_->pb->buffer);
      avio_context_free(&muxer_->pb);
    }
    avformat_free_context(muxer_);
    muxer_ = nullptr;
  }
  if (graph_)     avfilter_graph_free(&graph_); // also frees fsrc_ / fsink_
  fsrc_ = fsink_ = nullptr;
  if (fifo_)      { av_audio_fifo_free(fifo_); fifo_ = nullptr; }
  if (filtFrame_) av_frame_free(&filtFrame_);
  dtsDec_.Close();
  dtsIn_ = false;
  dtsPrimed_ = false;
  useFilter_ = false;
  autoUpmix_ = false;
  autoForce_ = false;
  upmixActive_ = false;
  upmixMix_ = 0.0f;
  quietSamples_ = 0;
  if (upFrame_) av_frame_free(&upFrame_);
  av_channel_layout_uninit(&filterInLayout_);
  useDts_ = false;
  limiterOn_ = false;
  if (dtsS32Swr_) swr_free(&dtsS32Swr_);
  if (swr_)       swr_free(&swr_);
  if (dtsFrame_) av_frame_free(&dtsFrame_);
  if (frame_)     av_frame_free(&frame_);
  if (pkt_)       av_packet_free(&pkt_);
  if (codecCtx_)  avcodec_free_context(&codecCtx_);
  av_channel_layout_uninit(&inLayout_);
  framesPerPacket_ = 0;
  nextPts_ = 0;
}

bool SpdifEncoder::BuildFilterGraph()
{
  graph_ = avfilter_graph_alloc();
  if (!graph_) return false;

  char inDesc[64] = {0}, outDesc[64] = {0};
  av_channel_layout_describe(&filterInLayout_, inDesc, sizeof inDesc);
  av_channel_layout_describe(&codecCtx_->ch_layout, outDesc, sizeof outDesc);

  const AVFilter* abuffer = avfilter_get_by_name("abuffer");
  const AVFilter* surround = avfilter_get_by_name("surround");
  const AVFilter* aformat = avfilter_get_by_name("aformat");
  const AVFilter* abuffersink = avfilter_get_by_name("abuffersink");
  if (!abuffer || !surround || !aformat || !abuffersink)
  {
    std::fprintf(stderr, "[SpdifEncoder] a required avfilter is missing (surround/aformat/abuffer)\n");
    return false;
  }

  char srcArgs[256];
  std::snprintf(srcArgs, sizeof srcArgs,
                "sample_rate=%d:sample_fmt=%s:channel_layout=%s:time_base=1/%d",
                sampleRate_, av_get_sample_fmt_name(inSampleFmt_), inDesc, sampleRate_);
  char surArgs[160];
  char surArgsDefault[128];
  std::snprintf(surArgsDefault, sizeof surArgsDefault, "chl_in=%s:chl_out=%s", inDesc, outDesc);
  if (upmixWinSize_ > 0)
    std::snprintf(surArgs, sizeof surArgs, "%s:win_size=%d", surArgsDefault, upmixWinSize_);
  else
    std::snprintf(surArgs, sizeof surArgs, "%s", surArgsDefault);
  char fmtArgs[256];
  std::snprintf(fmtArgs, sizeof fmtArgs, "sample_fmts=fltp:sample_rates=%d:channel_layouts=%s",
                sampleRate_, outDesc);

  AVFilterContext* surCtx = nullptr;
  AVFilterContext* fmtCtx = nullptr;
  int rc;
  if ((rc = avfilter_graph_create_filter(&fsrc_, abuffer, "in", srcArgs, nullptr, graph_)) < 0)
  { LogAv("create abuffer", rc); return false; }
  rc = avfilter_graph_create_filter(&surCtx, surround, "surround", surArgs, nullptr, graph_);
  if (rc < 0 && upmixWinSize_ > 0)
  {
    // This FFmpeg build does not accept win_size: fall back to the default window.
    std::fprintf(stderr, "[SpdifEncoder] surround filter rejected win_size=%d; using the default window\n",
                 upmixWinSize_);
    upmixWinSize_ = 0;
    surCtx = nullptr;
    rc = avfilter_graph_create_filter(&surCtx, surround, "surround", surArgsDefault, nullptr, graph_);
  }
  if (rc < 0)
  { LogAv("create surround", rc); return false; }
  if ((rc = avfilter_graph_create_filter(&fmtCtx, aformat, "aformat", fmtArgs, nullptr, graph_)) < 0)
  { LogAv("create aformat", rc); return false; }
  if ((rc = avfilter_graph_create_filter(&fsink_, abuffersink, "out", nullptr, nullptr, graph_)) < 0)
  { LogAv("create abuffersink", rc); return false; }

  if ((rc = avfilter_link(fsrc_, 0, surCtx, 0)) < 0 ||
      (rc = avfilter_link(surCtx, 0, fmtCtx, 0)) < 0 ||
      (rc = avfilter_link(fmtCtx, 0, fsink_, 0)) < 0)
  { LogAv("avfilter_link", rc); return false; }

  if ((rc = avfilter_graph_config(graph_, nullptr)) < 0)
  { LogAv("avfilter_graph_config", rc); return false; }

  fifo_ = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP, codecCtx_->ch_layout.nb_channels, sampleRate_);
  filtFrame_ = av_frame_alloc();
  if (!fifo_ || !filtFrame_) return false;

  // Prime with silence to warm the FFT and build a >= 2-packet cushion, so the real-time
  // consumer doesn't starve mid-stream (the filter has inherent FFT latency).
  const int inBytes =
      framesPerPacket_ * filterInLayout_.nb_channels * av_get_bytes_per_sample(inSampleFmt_);
  std::vector<uint8_t> silence(static_cast<size_t>(inBytes), 0);
  for (int guard = 0; guard < 64 && av_audio_fifo_size(fifo_) < 2 * framesPerPacket_; ++guard)
    if (!FeedFilter(silence.data()))
      break;

  std::printf("[SpdifEncoder] surround upmix enabled (%s -> %s), FFT window %d\n", inDesc, outDesc,
              upmixWinSize_ > 0 ? upmixWinSize_ : 4096);
  return true;
}

bool SpdifEncoder::FeedFilter(const uint8_t* in)
{
  const int inBytes =
      framesPerPacket_ * filterInLayout_.nb_channels * av_get_bytes_per_sample(inSampleFmt_);

  AVFrame* f = av_frame_alloc();
  if (!f) return false;
  f->format = inSampleFmt_;
  av_channel_layout_copy(&f->ch_layout, &filterInLayout_);
  f->sample_rate = sampleRate_;
  f->nb_samples = framesPerPacket_;
  if (av_frame_get_buffer(f, 0) < 0) { av_frame_free(&f); return false; }
  std::memcpy(f->data[0], in, static_cast<size_t>(inBytes));

  int rc = av_buffersrc_add_frame(fsrc_, f); // takes ownership of f's buffers, resets f
  av_frame_free(&f);
  if (rc < 0) { LogAv("av_buffersrc_add_frame", rc); return false; }

  for (;;)
  {
    rc = av_buffersink_get_frame(fsink_, filtFrame_);
    if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF)
      break;
    if (rc < 0) { LogAv("av_buffersink_get_frame", rc); return false; }
    int w = av_audio_fifo_write(fifo_, reinterpret_cast<void**>(filtFrame_->data),
                                filtFrame_->nb_samples);
    av_frame_unref(filtFrame_);
    if (w < 0) { LogAv("av_audio_fifo_write", w); return false; }
  }
  return true;
}

// Peak level (0..1) of the front pair and of the "rest" (centre + surrounds, LFE ignored) in one
// interleaved input packet.
void SpdifEncoder::AnalyzeChannels(const uint8_t* in, float& frontPeak, float& restPeak) const
{
  const int channels = inLayout_.nb_channels;
  const int frames = framesPerPacket_;
  auto scan = [&](auto toFloat, const auto* data) {
    for (int f = 0; f < frames; ++f)
    {
      const auto* s = data + static_cast<size_t>(f) * static_cast<size_t>(channels);
      for (int c = 0; c < channels; ++c)
      {
        if (c == lfeIdx_)
          continue;
        const float v = std::fabs(toFloat(s[c]));
        if (c == frontIdx_[0] || c == frontIdx_[1])
          frontPeak = std::max(frontPeak, v);
        else
          restPeak = std::max(restPeak, v);
      }
    }
  };
  switch (inSampleFmt_)
  {
  case AV_SAMPLE_FMT_FLT:
    scan([](float v) { return v; }, reinterpret_cast<const float*>(in));
    break;
  case AV_SAMPLE_FMT_S16:
    scan([](int16_t v) { return static_cast<float>(v) / 32768.0f; },
         reinterpret_cast<const int16_t*>(in));
    break;
  case AV_SAMPLE_FMT_S32:
    scan([](int32_t v) { return static_cast<float>(v) / 2147483648.0f; },
         reinterpret_cast<const int32_t*>(in));
    break;
  default:
    restPeak = 1.0f; // unknown format: never treat as stereo-only
    break;
  }
}

// Stereo-only detection with hysteresis:
//  - centre/surround peak above -60 dBFS            -> real 5.1: leave upmix immediately
//  - centre/surround peak below -70 dBFS while FL/FR carry signal for ~0.2 s -> enter upmix
//  - anything in between, or digital silence on every channel -> keep the current decision
void SpdifEncoder::UpdateAutoUpmixState(const uint8_t* in)
{
  if (autoForce_)
  {
    upmixActive_ = true;
    return;
  }
  constexpr float kActivePeak = 1.0e-3f;   // -60 dBFS
  constexpr float kQuietPeak = 3.1623e-4f; // -70 dBFS
  constexpr float kFrontMin = 1.0e-4f;     // -80 dBFS: FL/FR carry something
  float frontPeak = 0.0f, restPeak = 0.0f;
  AnalyzeChannels(in, frontPeak, restPeak);

  if (restPeak > kActivePeak)
  {
    quietSamples_ = 0;
    if (upmixActive_)
    {
      upmixActive_ = false;
      std::printf("[SpdifEncoder] auto-upmix: real 5.1 detected -> pass-through\n");
    }
  }
  else if (restPeak < kQuietPeak)
  {
    if (frontPeak > kFrontMin)
    {
      quietSamples_ = std::min(quietSamples_ + framesPerPacket_, sampleRate_);
      if (!upmixActive_ && quietSamples_ >= sampleRate_ / 5)
      {
        upmixActive_ = true;
        std::printf("[SpdifEncoder] auto-upmix: stereo-only input detected -> upmix\n");
      }
    }
  }
  else
  {
    quietSamples_ = 0;
  }
}

// Copy the FL/FR samples of the multichannel interleaved packet into stereoScratch_.
void SpdifEncoder::ExtractFrontPair(const uint8_t* in)
{
  const size_t bps = static_cast<size_t>(av_get_bytes_per_sample(inSampleFmt_));
  const size_t channels = static_cast<size_t>(inLayout_.nb_channels);
  uint8_t* dst = stereoScratch_.data();
  for (int f = 0; f < framesPerPacket_; ++f)
  {
    const uint8_t* src = in + static_cast<size_t>(f) * channels * bps;
    std::memcpy(dst, src + static_cast<size_t>(frontIdx_[0]) * bps, bps);
    std::memcpy(dst + bps, src + static_cast<size_t>(frontIdx_[1]) * bps, bps);
    dst += 2 * bps;
  }
}

// frame_ holds the pass-through 5.1, upFrame_ the upmixed 5.1. Leave the result in frame_.
// The mix position glides to its target over 10 ms so a decision change never clicks.
void SpdifEncoder::MixAutoFrames(bool upReady)
{
  const float target = (upReady && upmixActive_) ? 1.0f : 0.0f;
  const int samples = frame_->nb_samples;
  const int channels = frame_->ch_layout.nb_channels;

  if (upmixMix_ == target)
  {
    if (target == 1.0f)
      for (int c = 0; c < channels; ++c)
        std::memcpy(frame_->extended_data[c], upFrame_->extended_data[c],
                    static_cast<size_t>(samples) * sizeof(float));
    return; // target 0: frame_ already is the pass-through signal
  }

  const float step = 1.0f / static_cast<float>(std::max(1, sampleRate_ / 100));
  for (int i = 0; i < samples; ++i)
  {
    if (upmixMix_ < target)      upmixMix_ = std::min(target, upmixMix_ + step);
    else if (upmixMix_ > target) upmixMix_ = std::max(target, upmixMix_ - step);
    const float m = upmixMix_;
    for (int c = 0; c < channels; ++c)
    {
      float* pass = reinterpret_cast<float*>(frame_->extended_data[c]);
      const float* up = reinterpret_cast<const float*>(upFrame_->extended_data[c]);
      pass[i] = pass[i] * (1.0f - m) + up[i] * m;
    }
  }
}

// Largest absolute sample (0..1) of the interleaved input packet, over all channels.
float SpdifEncoder::InputPeak(const uint8_t* in) const
{
  const size_t count = static_cast<size_t>(framesPerPacket_) * static_cast<size_t>(inLayout_.nb_channels);
  float peak = 0.0f;
  switch (inSampleFmt_)
  {
  case AV_SAMPLE_FMT_FLT:
  {
    const float* s = reinterpret_cast<const float*>(in);
    for (size_t i = 0; i < count; ++i) peak = std::max(peak, std::fabs(s[i]));
    break;
  }
  case AV_SAMPLE_FMT_S16:
  {
    const int16_t* s = reinterpret_cast<const int16_t*>(in);
    int m = 0;
    for (size_t i = 0; i < count; ++i) m = std::max(m, std::abs(static_cast<int>(s[i])));
    peak = static_cast<float>(m) / 32768.0f;
    break;
  }
  case AV_SAMPLE_FMT_S32:
  {
    const int32_t* s = reinterpret_cast<const int32_t*>(in);
    int64_t m = 0;
    for (size_t i = 0; i < count; ++i) m = std::max<int64_t>(m, std::llabs(static_cast<int64_t>(s[i])));
    peak = static_cast<float>(static_cast<double>(m) / 2147483648.0);
    break;
  }
  default:
    break;
  }
  return peak;
}

// FL/FR of the interleaved input packet as 16-bit carrier words (L0 R0 L1 R1 ...).
void SpdifEncoder::ExtractDtsCarrierWords(const uint8_t* in)
{
  const size_t channels = static_cast<size_t>(inLayout_.nb_channels);
  const size_t fl = static_cast<size_t>(dtsFlIdx_);
  const size_t fr = static_cast<size_t>(dtsFrIdx_);
  int16_t* dst = dtsWords_.data();
  const size_t frames = static_cast<size_t>(framesPerPacket_);

  auto fromFloat = [](float v) -> int16_t {
    const float s = v * 32768.0f;
    if (s >= 32767.0f) return 32767;
    if (s <= -32768.0f) return -32768;
    return static_cast<int16_t>(std::lrint(s));
  };
  auto fromS32 = [](int32_t v) -> int16_t {
    const int64_t s = (static_cast<int64_t>(v) + 0x8000) >> 16;
    return static_cast<int16_t>(std::min<int64_t>(s, 32767));
  };

  switch (inSampleFmt_)
  {
  case AV_SAMPLE_FMT_FLT:
  {
    const float* s = reinterpret_cast<const float*>(in);
    for (size_t f = 0; f < frames; ++f, s += channels)
    {
      dst[2 * f] = fromFloat(s[fl]);
      dst[2 * f + 1] = fromFloat(s[fr]);
    }
    break;
  }
  case AV_SAMPLE_FMT_S16:
  {
    const int16_t* s = reinterpret_cast<const int16_t*>(in);
    for (size_t f = 0; f < frames; ++f, s += channels)
    {
      dst[2 * f] = s[fl];
      dst[2 * f + 1] = s[fr];
    }
    break;
  }
  default: // AV_SAMPLE_FMT_S32
  {
    const int32_t* s = reinterpret_cast<const int32_t*>(in);
    for (size_t f = 0; f < frames; ++f, s += channels)
    {
      dst[2 * f] = fromS32(s[fl]);
      dst[2 * f + 1] = fromS32(s[fr]);
    }
    break;
  }
  }
}

// Feed the carrier to the DTS decoder; once it is active, hand out one packet of decoded 5.1.
// Returns 0 = no DTS (use the PCM path), 1 = frame_ is filled, 2 = DTS active but not enough
// decoded audio yet (start-up cushion of 2 packets, or an underrun).
int SpdifEncoder::PullDtsFrame(const uint8_t* in)
{
  ExtractDtsCarrierWords(in);
  dtsDec_.Push(dtsWords_.data(), framesPerPacket_);
  if (!dtsDec_.Active())
  {
    dtsPrimed_ = false;
    return 0;
  }
  if (!dtsPrimed_)
  {
    if (dtsDec_.Available() < 2 * framesPerPacket_)
      return 2;
    dtsPrimed_ = true;
  }
  if (dtsDec_.Available() < framesPerPacket_)
  {
    dtsPrimed_ = false; // underrun: rebuild the cushion
    return 2;
  }
  if (av_frame_make_writable(frame_) < 0)
    return 2;
  if (!dtsDec_.Read(reinterpret_cast<void**>(frame_->data), framesPerPacket_))
    return 2;
  return 1;
}

int SpdifEncoder::EncodePacket(const uint8_t* in, uint8_t* outBuf, int outSize)
{
  if (!codecCtx_)
    return -1;
  if (outSize < kMaxBytesPerPacket)
    return -1;

  if (dumpIn_.enabled())
    dumpIn_.Append(in, static_cast<size_t>(framesPerPacket_) * static_cast<size_t>(inLayout_.nb_channels) *
                           static_cast<size_t>(av_get_bytes_per_sample(inSampleFmt_)));

  // DTS bitstream in the carrier? Then the decoded 5.1 replaces the PCM path for this packet.
  bool dtsFrame = false;
  if (dtsIn_)
  {
    const int r = PullDtsFrame(in);
    if (r == 2)
      return 0; // DTS is active but the decoder has not produced enough yet: caller emits silence
    dtsFrame = (r == 1);
  }

  if (!dtsFrame && InputPeak(in) >= 0.9999f)
    inputFullScale_.fetch_add(1, std::memory_order_relaxed);

  if (dtsFrame)
  {
    // frame_ was filled from the DTS decoder
  }
  else if (useFilter_)
  {
    // Push this input packet through the surround graph, then take one packet of upmixed
    // 5.1 from the FIFO. During startup (FFT latency) the FIFO may not yet hold a full
    // packet -> return 0 so the caller emits silence until it fills.
    if (!FeedFilter(in))
      return -1;
    if (av_audio_fifo_size(fifo_) < framesPerPacket_)
      return 0;
    if (av_frame_make_writable(frame_) < 0)
      return -1;
    if (av_audio_fifo_read(fifo_, reinterpret_cast<void**>(frame_->data), framesPerPacket_) <
        framesPerPacket_)
      return 0;
  }
  else
  {
    if (av_frame_make_writable(frame_) < 0)
      return -1;
    // Convert/downmix the interleaved input into the planar-float encoder frame.
    const uint8_t* inPlanes[1] = { in };
    int got = swr_convert(swr_, frame_->data, frame_->nb_samples, inPlanes, framesPerPacket_);
    if (got < 0) { LogAv("swr_convert", got); return -1; }

    if (autoUpmix_)
    {
      // frame_ now holds the pass-through 5.1. Decide stereo-only vs real 5.1, keep the surround
      // graph warm with FL/FR on every packet, and blend in the upmixed packet when needed.
      UpdateAutoUpmixState(in);
      ExtractFrontPair(in);
      bool upReady = false;
      if (FeedFilter(stereoScratch_.data()) && av_audio_fifo_size(fifo_) >= framesPerPacket_ &&
          av_frame_make_writable(upFrame_) >= 0 &&
          av_audio_fifo_read(fifo_, reinterpret_cast<void**>(upFrame_->data), framesPerPacket_) >=
              framesPerPacket_)
        upReady = true;
      MixAutoFrames(upReady);
    }
  }

  // The encoder frame is always planar-float 5.1 (FL, FR, C, LFE, SL/BL, SR/BR).
  // Atomic per-channel gains let the UI adjust levels without locking the audio thread.
  ApplyChannelGains();
  if (dumpEnc_.enabled() && !dumpEnc_.full())
  {
    const int chans = frame_->ch_layout.nb_channels;
    for (int c = 0; c < chans; ++c)
    {
      const float* plane = reinterpret_cast<const float*>(frame_->extended_data[c]);
      for (int i = 0; i < framesPerPacket_; ++i)
        dumpScratch_[static_cast<size_t>(i) * static_cast<size_t>(chans) + static_cast<size_t>(c)] = plane[i];
    }
    dumpEnc_.Append(dumpScratch_.data(), dumpScratch_.size() * sizeof(float));
  }
  frame_->pts = nextPts_;
  nextPts_ += framesPerPacket_;
  return EncodeFrameToBurst(outBuf, outSize);
}

int SpdifEncoder::EncodeFrameToBurst(uint8_t* outBuf, int outSize)
{
  AVFrame* encodeFrame = frame_;
  if (useDts_)
  {
    if (av_frame_make_writable(dtsFrame_) < 0)
      return -1;

    const uint8_t* inData[AV_NUM_DATA_POINTERS] = {};
    for (int ch = 0; ch < codecCtx_->ch_layout.nb_channels; ++ch)
      inData[ch] = frame_->extended_data[ch];

    const int converted = swr_convert(dtsS32Swr_, dtsFrame_->data,
                                       dtsFrame_->nb_samples, inData, frame_->nb_samples);
    if (converted < 0)
    {
      LogAv("swr_convert(DTS)", converted);
      return -1;
    }
    if (converted != framesPerPacket_)
    {
      std::fprintf(stderr, "[SpdifEncoder] unexpected DTS sample count: %d\n", converted);
      return -1;
    }
    dtsFrame_->pts = frame_->pts;
    encodeFrame = dtsFrame_;
  }

  int rc = avcodec_send_frame(codecCtx_, encodeFrame);
  if (rc < 0) { LogAv("avcodec_send_frame", rc); return -1; }

  rc = avcodec_receive_packet(codecCtx_, pkt_);
  if (rc == AVERROR(EAGAIN))
    return 0; // encoder buffering; no burst this call (AC3 is normally 1-in/1-out)
  if (rc < 0) { LogAv("avcodec_receive_packet", rc); return -1; }

  pkt_->stream_index = 0;
  writeDst_ = outBuf;
  writeCap_ = outSize;
  writeLen_ = 0;

  rc = av_write_frame(muxer_, pkt_);
  av_packet_unref(pkt_);
  if (rc < 0) { LogAv("av_write_frame(spdif)", rc); return -1; }
  avio_flush(muxer_->pb); // force the muxer to emit the full burst through OnWritePacket

  int n = writeLen_;
  writeDst_ = nullptr;
  writeCap_ = 0;
  writeLen_ = 0;
  return n;
}

int SpdifEncoder::WritePacketThunk(void* opaque, const uint8_t* buf, int buf_size)
{
  return static_cast<SpdifEncoder*>(opaque)->OnWritePacket(buf, buf_size);
}

int SpdifEncoder::OnWritePacket(const uint8_t* buf, int buf_size)
{
  int n = buf_size;
  if (writeLen_ + n > writeCap_)
    n = writeCap_ - writeLen_;
  if (n > 0)
  {
    std::memcpy(writeDst_ + writeLen_, buf, static_cast<size_t>(n));
    writeLen_ += n;
  }
  return buf_size; // report full consumption to the muxer even if we clamped
}
