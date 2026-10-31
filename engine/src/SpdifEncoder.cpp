// SpdifEncoder.cpp  — see SpdifEncoder.h for design notes.

#include "SpdifEncoder.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

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
  for (size_t i = 0; i < 6; ++i)
  {
    peakLevels_[i].store(0.0f, std::memory_order_relaxed);
    rmsLevels_[i].store(0.0f, std::memory_order_relaxed);
  }
}

void SpdifEncoder::GetChannelLevels(float outPeaks[6], float outRms[6]) const
{
  for (size_t i = 0; i < 6; ++i)
  {
    outPeaks[i] = peakLevels_[i].load(std::memory_order_relaxed);
    outRms[i] = rmsLevels_[i].load(std::memory_order_relaxed);
  }
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
  const int channels = std::min(6, codecCtx_->ch_layout.nb_channels);
  for (int ch = 0; ch < channels; ++ch)
  {
    const float gain = channelGainLinear_[static_cast<size_t>(ch)].load(std::memory_order_relaxed);
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
}

void SpdifEncoder::ComputeLevels()
{
  if (!frame_ || !frame_->extended_data || !codecCtx_)
    return;
  const int channels = std::min(6, codecCtx_->ch_layout.nb_channels);
  for (int ch = 0; ch < channels; ++ch)
  {
    const float* s = reinterpret_cast<const float*>(frame_->extended_data[ch]);
    if (!s) continue;
    float peak = 0.0f;
    double sumSq = 0.0;
    for (int i = 0; i < frame_->nb_samples; ++i)
    {
      const float a = std::fabs(s[i]);
      if (a > peak) peak = a;
      sumSq += static_cast<double>(a) * a;
    }
    const float rms = frame_->nb_samples > 0 ? static_cast<float>(std::sqrt(sumSq / frame_->nb_samples)) : 0.0f;
    peakLevels_[static_cast<size_t>(ch)].store(peak, std::memory_order_relaxed);
    rmsLevels_[static_cast<size_t>(ch)].store(rms, std::memory_order_relaxed);
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
    std::fprintf(stderr, "[SpdifEncoder] %s encoder not found in this FFmpeg build\n",
                 useDts_ ? "DTS" : "AC3");
    return false;
  }

  codecCtx_ = avcodec_alloc_context3(codec);
  if (!codecCtx_)
    return false;

  codecCtx_->bit_rate = useDts_ ? p.dtsBitRate : p.bitRate;
  codecCtx_->sample_rate = p.sampleRate;
  codecCtx_->sample_fmt = useDts_ ? AV_SAMPLE_FMT_S32 : AV_SAMPLE_FMT_FLTP;
  codecCtx_->time_base = av_make_q(1, p.sampleRate);
  if (useDts_)
    codecCtx_->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;

  // The encoder (and S/PDIF) carry 5.1.
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

  framesPerPacket_ = codecCtx_->frame_size; // 1536 for AC3, 512 for DTS

  // Scratch frame holding the planar-float 5.1 samples. Both the swr path and the
  // surround-filter path fill this.
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
  // convert/downmix. When input is already multichannel but upmix is requested, use
  // "dynamic" mode: keep both paths live and detect stereo content at runtime.
  upmixMode_ = p.upmix;
  const bool wantUpmix = (p.upmix != Upmix::Off);
  alwaysUpmix_ = (p.upmix == Upmix::Always || p.upmix == Upmix::AlwaysFront);
  useFilter_ = (wantUpmix && inLayout_.nb_channels <= 2);
  dynamicUpmix_ = (wantUpmix && inLayout_.nb_channels > 2);

  if (useFilter_)
  {
    av_channel_layout_copy(&filterInLayout_, &inLayout_);
    if (!BuildFilterGraph())
    {
      std::fprintf(stderr, "[SpdifEncoder] surround upmix unavailable; using plain upmix/downmix\n");
      useFilter_ = false;
      av_channel_layout_uninit(&filterInLayout_);
    }
  }
  if (dynamicUpmix_)
  {
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    av_channel_layout_copy(&filterInLayout_, &stereo);
    if (!BuildFilterGraph())
    {
      std::fprintf(stderr, "[SpdifEncoder] dynamic upmix unavailable; multichannel passthrough only\n");
      dynamicUpmix_ = false;
      av_channel_layout_uninit(&filterInLayout_);
    }
    else
    {
      const int bps = av_get_bytes_per_sample(inSampleFmt_);
      stereoExtract_.resize(static_cast<size_t>(framesPerPacket_) * 2 * bps);
    }
  }
  if (!useFilter_)
  {
    rc = swr_alloc_set_opts2(&swr_, &codecCtx_->ch_layout, AV_SAMPLE_FMT_FLTP, sampleRate_,
                             &inLayout_, inSampleFmt_, sampleRate_, 0, nullptr);
    if (rc < 0 || !swr_) { LogAv("swr_alloc_set_opts2", rc); return false; }
    rc = swr_init(swr_);
    if (rc < 0) { LogAv("swr_init", rc); return false; }
  }

  pkt_ = av_packet_alloc();
  if (!pkt_) return false;

  rc = avformat_alloc_output_context2(&muxer_, nullptr, "spdif", nullptr);
  if (rc < 0 || !muxer_) { LogAv("avformat_alloc_output_context2(spdif)", rc); return false; }

  AVStream* st = avformat_new_stream(muxer_, nullptr);
  if (!st) { std::fprintf(stderr, "[SpdifEncoder] avformat_new_stream failed\n"); return false; }
  st->id = 0;
  st->time_base = codecCtx_->time_base;
  rc = avcodec_parameters_from_context(st->codecpar, codecCtx_);
  if (rc < 0) { LogAv("avcodec_parameters_from_context", rc); return false; }

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
  if (muxer_)
  {
    if (muxer_->pb)
      av_write_trailer(muxer_);
    if (muxer_->pb)
    {
      av_freep(&muxer_->pb->buffer);
      avio_context_free(&muxer_->pb);
    }
    avformat_free_context(muxer_);
    muxer_ = nullptr;
  }
  if (graph_)     avfilter_graph_free(&graph_);
  fsrc_ = fsink_ = nullptr;
  if (fifo_)      { av_audio_fifo_free(fifo_); fifo_ = nullptr; }
  if (filtFrame_) av_frame_free(&filtFrame_);
  useFilter_ = false;
  useDts_ = false;
  dynamicUpmix_ = false;
  alwaysUpmix_ = false;
  upmixMode_ = Upmix::Off;
  effectivelyStereo_ = true;
  multichannelHoldFrames_ = 0;
  av_channel_layout_uninit(&filterInLayout_);
  stereoExtract_.clear();
  if (dtsS32Swr_) swr_free(&dtsS32Swr_);
  if (swr_)       swr_free(&swr_);
  if (dtsFrame_)  av_frame_free(&dtsFrame_);
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
  char surArgs[256];
  const bool isFront31 = (upmixMode_ == Upmix::FrontStage || upmixMode_ == Upmix::AlwaysFront);
  const bool isFront30 = (upmixMode_ == Upmix::Front30);
  const bool isSub21 = (upmixMode_ == Upmix::StereoSub21);
  if (isFront30)
  {
    std::snprintf(surArgs, sizeof surArgs,
                  "chl_in=%s:chl_out=%s:fc_out=1.0:lfe_out=0.0:bl_out=0:br_out=0:sl_out=0:sr_out=0:bc_out=0",
                  inDesc, outDesc);
  }
  else if (isFront31 || isSub21)
  {
    std::snprintf(surArgs, sizeof surArgs,
                  "chl_in=%s:chl_out=%s:fc_out=1.0:lfe_out=1.2:lfe_low=140:bl_out=0:br_out=0:sl_out=0:sr_out=0:bc_out=0",
                  inDesc, outDesc);
  }
  else
  {
    std::snprintf(surArgs, sizeof surArgs,
                  "chl_in=%s:chl_out=%s:fc_out=1.0:lfe_out=1.2:lfe_low=140",
                  inDesc, outDesc);
  }
  char fmtArgs[256];
  std::snprintf(fmtArgs, sizeof fmtArgs, "sample_fmts=fltp:sample_rates=%d:channel_layouts=%s",
                sampleRate_, outDesc);

  AVFilterContext* surCtx = nullptr;
  AVFilterContext* fmtCtx = nullptr;
  int rc;
  if ((rc = avfilter_graph_create_filter(&fsrc_, abuffer, "in", srcArgs, nullptr, graph_)) < 0)
  { LogAv("create abuffer", rc); return false; }
  if ((rc = avfilter_graph_create_filter(&surCtx, surround, "surround", surArgs, nullptr, graph_)) < 0)
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

  // Prime the FIFO past the 2048-sample FFT hop of FFmpeg's `surround` filter so
  // both 1536-sample AC3 and 512-sample DTS packets never underrun between FFT hops.
  const int primeTarget = std::max(2 * framesPerPacket_, 2048);
  const int inBytes =
      framesPerPacket_ * filterInLayout_.nb_channels * av_get_bytes_per_sample(inSampleFmt_);
  std::vector<uint8_t> silence(static_cast<size_t>(inBytes), 0);
  for (int guard = 0; guard < 64 && av_audio_fifo_size(fifo_) < primeTarget; ++guard)
    if (!FeedFilter(silence.data()))
      break;

  const char* modeName = isFront30 ? "3.0 front stage" : (isSub21 ? "2.1 stereo+sub" : (isFront31 ? "3.1 front stage (Center+Sub)" : "surround"));
  std::printf("[SpdifEncoder] %s upmix enabled%s (%s -> %s)\n",
              modeName,
              dynamicUpmix_ ? " (dynamic: auto-detect stereo in multichannel)" : "",
              inDesc, outDesc);
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

  int rc = av_buffersrc_add_frame(fsrc_, f);
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

int SpdifEncoder::EncodePacket(const uint8_t* in, uint8_t* outBuf, int outSize)
{
  if (!codecCtx_)
    return -1;
  if (outSize < kMaxBytesPerPacket)
    return -1;

  bool usedUpmixFilter = false;
  if (useFilter_)
  {
    if (!FeedFilter(in))
      return -1;
    if (av_audio_fifo_size(fifo_) < framesPerPacket_)
      return 0;
    if (av_frame_make_writable(frame_) < 0)
      return -1;
    if (av_audio_fifo_read(fifo_, reinterpret_cast<void**>(frame_->data), framesPerPacket_) <
        framesPerPacket_)
      return 0;
    usedUpmixFilter = true;
  }
  else if (dynamicUpmix_)
  {
    ExtractStereo(in, stereoExtract_.data());
    if (!FeedFilter(stereoExtract_.data()))
      return -1;

    // FFmpeg's `surround` filter outputs in 2048-sample FFT hops (4x a 512-sample DTS packet).
    // Never drain below 2048 + framesPerPacket_ so 512-sample DTS packets in 5.1 loopback mode
    // never lose audio or underrun between 2048-sample FFT hops.
    const int maxActiveFifo = std::max(3 * framesPerPacket_, 2048 + framesPerPacket_);
    const int minPrimedFifo = std::max(2 * framesPerPacket_, 2048);

    if (alwaysUpmix_ || IsEffectivelyStereo(in))
    {
      const int excess = av_audio_fifo_size(fifo_) - maxActiveFifo;
      if (excess > 0)
        av_audio_fifo_drain(fifo_, excess);
      if (av_audio_fifo_size(fifo_) < framesPerPacket_)
        return 0;
      if (av_frame_make_writable(frame_) < 0)
        return -1;
      if (av_audio_fifo_read(fifo_, reinterpret_cast<void**>(frame_->data), framesPerPacket_) <
          framesPerPacket_)
        return 0;
      usedUpmixFilter = true;
    }
    else
    {
      const int excess = av_audio_fifo_size(fifo_) - minPrimedFifo;
      if (excess > 0)
        av_audio_fifo_drain(fifo_, excess);
      if (av_frame_make_writable(frame_) < 0)
        return -1;
      const uint8_t* inPlanes[1] = { in };
      int got = swr_convert(swr_, frame_->data, frame_->nb_samples, inPlanes, framesPerPacket_);
      if (got < 0) { LogAv("swr_convert", got); return -1; }
    }
  }
  else
  {
    if (av_frame_make_writable(frame_) < 0)
      return -1;
    const uint8_t* inPlanes[1] = { in };
    int got = swr_convert(swr_, frame_->data, frame_->nb_samples, inPlanes, framesPerPacket_);
    if (got < 0) { LogAv("swr_convert", got); return -1; }
  }

  if (usedUpmixFilter && codecCtx_->ch_layout.nb_channels >= 6)
  {
    if (upmixMode_ == Upmix::FrontStage || upmixMode_ == Upmix::AlwaysFront)
    {
      for (int ch : { 4, 5 })
      {
        float* p = reinterpret_cast<float*>(frame_->extended_data[ch]);
        if (p)
          std::fill_n(p, frame_->nb_samples, 0.0f);
      }
    }
    else if (upmixMode_ == Upmix::Front30)
    {
      for (int ch : { 3, 4, 5 })
      {
        float* p = reinterpret_cast<float*>(frame_->extended_data[ch]);
        if (p)
          std::fill_n(p, frame_->nb_samples, 0.0f);
      }
    }
    else if (upmixMode_ == Upmix::StereoSub21)
    {
      float* fl = reinterpret_cast<float*>(frame_->extended_data[0]);
      float* fr = reinterpret_cast<float*>(frame_->extended_data[1]);
      float* fc = reinterpret_cast<float*>(frame_->extended_data[2]);
      if (fl && fr && fc)
      {
        for (int i = 0; i < frame_->nb_samples; ++i)
        {
          fl[i] += 0.70710678f * fc[i];
          fr[i] += 0.70710678f * fc[i];
        }
      }
      for (int ch : { 2, 4, 5 })
      {
        float* p = reinterpret_cast<float*>(frame_->extended_data[ch]);
        if (p)
          std::fill_n(p, frame_->nb_samples, 0.0f);
      }
    }
  }

  // Per-channel trim applied to the 5.1 planar-float frame
  ApplyChannelGains();
  ComputeLevels();

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
    return 0;
  if (rc < 0) { LogAv("avcodec_receive_packet", rc); return -1; }

  pkt_->stream_index = 0;
  writeDst_ = outBuf;
  writeCap_ = outSize;
  writeLen_ = 0;

  rc = av_write_frame(muxer_, pkt_);
  av_packet_unref(pkt_);
  if (rc < 0) { LogAv("av_write_frame(spdif)", rc); return -1; }
  avio_flush(muxer_->pb);

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

bool SpdifEncoder::IsEffectivelyStereo(const uint8_t* in) const
{
  const int ch = inLayout_.nb_channels;
  if (ch <= 2)
    return true;

  // On a 5.1 / 7.1 endpoint, stereo applications (music players, browsers, Discord, stereo games)
  // only populate Front Left (ch 0) and Front Right (ch 1); Windows leaves all discrete
  // non-stereo channels (Center = ch 2, LFE = ch 3, Surrounds = ch 4..ch-1) at digital zero.
  // If ANY discrete multichannel channel (Center, LFE, or Rear/Side Surrounds, c >= 2) carries
  // signal (>= -66 dB), the source is native 5.1/7.1 and MUST pass through untouched.
  double discreteSumSq = 0.0;
  double frontSumSq = 0.0;
  const int discreteChannels = ch - 2;

  if (inSampleFmt_ == AV_SAMPLE_FMT_FLT)
  {
    const float* f = reinterpret_cast<const float*>(in);
    for (int i = 0; i < framesPerPacket_; ++i)
    {
      const double fl = f[i * ch + 0];
      const double fr = f[i * ch + 1];
      frontSumSq += fl * fl + fr * fr;
      for (int c = 2; c < ch; ++c)
      {
        const double v = f[i * ch + c];
        discreteSumSq += v * v;
      }
    }
  }
  else if (inSampleFmt_ == AV_SAMPLE_FMT_S16)
  {
    const int16_t* s = reinterpret_cast<const int16_t*>(in);
    for (int i = 0; i < framesPerPacket_; ++i)
    {
      const double fl = s[i * ch + 0] / 32768.0;
      const double fr = s[i * ch + 1] / 32768.0;
      frontSumSq += fl * fl + fr * fr;
      for (int c = 2; c < ch; ++c)
      {
        const double v = s[i * ch + c] / 32768.0;
        discreteSumSq += v * v;
      }
    }
  }
  else if (inSampleFmt_ == AV_SAMPLE_FMT_S32)
  {
    const int32_t* s = reinterpret_cast<const int32_t*>(in);
    for (int i = 0; i < framesPerPacket_; ++i)
    {
      const double fl = s[i * ch + 0] / 2147483648.0;
      const double fr = s[i * ch + 1] / 2147483648.0;
      frontSumSq += fl * fl + fr * fr;
      for (int c = 2; c < ch; ++c)
      {
        const double v = s[i * ch + c] / 2147483648.0;
        discreteSumSq += v * v;
      }
    }
  }

  const double discreteRms = std::sqrt(discreteSumSq / (framesPerPacket_ * discreteChannels));
  const double frontRms = std::sqrt(frontSumSq / (framesPerPacket_ * 2));
  constexpr double kThreshold = 0.0005; // ~ -66 dB

  if (discreteRms >= kThreshold)
  {
    // Active Center, LFE, or Surround audio detected -> native 5.1 passthrough.
    // Hold native 5.1 mode for ~3 seconds of active front-only audio so brief front-only moments
    // in a 5.1 movie, game, or speaker test never flip back into the stereo upmixer.
    multichannelHoldFrames_ = sampleRate_ * 3;
    effectivelyStereo_ = false;
    return false;
  }

  if (multichannelHoldFrames_ > 0)
  {
    if (frontRms >= kThreshold)
      multichannelHoldFrames_ = std::max(0, multichannelHoldFrames_ - framesPerPacket_);
    if (multichannelHoldFrames_ > 0)
    {
      effectivelyStereo_ = false;
      return false;
    }
  }

  effectivelyStereo_ = true;
  return true; // Only FL/FR carry audio -> Upmix to 5.1/3.1!
}

void SpdifEncoder::ExtractStereo(const uint8_t* in, uint8_t* out) const
{
  const int ch = inLayout_.nb_channels;
  const int bps = av_get_bytes_per_sample(inSampleFmt_);
  const int inStride = ch * bps;
  const int outStride = 2 * bps;
  for (int i = 0; i < framesPerPacket_; ++i)
    std::memcpy(out + i * outStride, in + i * inStride, static_cast<size_t>(outStride));
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
  return buf_size;
}
