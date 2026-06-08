// SpdifEncoder.cpp  — see SpdifEncoder.h for design notes.

#include "SpdifEncoder.h"

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

  const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AC3);
  if (!codec)
  {
    std::fprintf(stderr, "[SpdifEncoder] AC3 encoder not found in this FFmpeg build\n");
    return false;
  }

  codecCtx_ = avcodec_alloc_context3(codec);
  if (!codecCtx_)
    return false;

  codecCtx_->bit_rate = p.bitRate;
  codecCtx_->sample_rate = p.sampleRate;
  codecCtx_->sample_fmt = AV_SAMPLE_FMT_FLTP; // FFmpeg's AC3 encoder takes planar float
  codecCtx_->time_base = av_make_q(1, p.sampleRate);

  // The encoder (and S/PDIF) carry 5.1. Prefer the "back" variant to match Windows'
  // KSAUDIO_SPEAKER_5POINT1 (FL FR FC LFE BL BR); fall back to "side" 5.1 if the
  // encoder build rejects it.
  AVChannelLayout enc51back = AV_CHANNEL_LAYOUT_5POINT1_BACK;
  AVChannelLayout enc51side = AV_CHANNEL_LAYOUT_5POINT1;
  av_channel_layout_copy(&codecCtx_->ch_layout, &enc51back);

  int rc = avcodec_open2(codecCtx_, codec, nullptr);
  if (rc < 0)
  {
    av_channel_layout_uninit(&codecCtx_->ch_layout);
    av_channel_layout_copy(&codecCtx_->ch_layout, &enc51side);
    rc = avcodec_open2(codecCtx_, codec, nullptr);
    if (rc < 0) { LogAv("avcodec_open2", rc); return false; }
  }

  framesPerPacket_ = codecCtx_->frame_size; // 1536 for AC3

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

  // Input conditioning: either the `surround` upmix filter (stereo->5.1) or plain swr
  // convert/downmix. Surround only applies to <= 2ch input; multichannel always downmixes.
  useFilter_ = (p.upmix == Upmix::Surround && inLayout_.nb_channels <= 2);
  if (useFilter_ && !BuildFilterGraph())
  {
    std::fprintf(stderr, "[SpdifEncoder] surround upmix unavailable; using plain upmix/downmix\n");
    useFilter_ = false;
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
  useFilter_ = false;
  if (swr_)       swr_free(&swr_);
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
  av_channel_layout_describe(&inLayout_, inDesc, sizeof inDesc);
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
  char surArgs[128];
  std::snprintf(surArgs, sizeof surArgs, "chl_in=%s:chl_out=%s", inDesc, outDesc);
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

  // Prime with silence to warm the FFT and build a >= 2-packet cushion, so the real-time
  // consumer doesn't starve mid-stream (the filter has inherent FFT latency).
  const int inBytes =
      framesPerPacket_ * inLayout_.nb_channels * av_get_bytes_per_sample(inSampleFmt_);
  std::vector<uint8_t> silence(static_cast<size_t>(inBytes), 0);
  for (int guard = 0; guard < 64 && av_audio_fifo_size(fifo_) < 2 * framesPerPacket_; ++guard)
    if (!FeedFilter(silence.data()))
      break;

  std::printf("[SpdifEncoder] surround upmix enabled (%s -> %s)\n", inDesc, outDesc);
  return true;
}

bool SpdifEncoder::FeedFilter(const uint8_t* in)
{
  const int inBytes =
      framesPerPacket_ * inLayout_.nb_channels * av_get_bytes_per_sample(inSampleFmt_);

  AVFrame* f = av_frame_alloc();
  if (!f) return false;
  f->format = inSampleFmt_;
  av_channel_layout_copy(&f->ch_layout, &inLayout_);
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

int SpdifEncoder::EncodePacket(const uint8_t* in, uint8_t* outBuf, int outSize)
{
  if (!codecCtx_)
    return -1;
  if (outSize < kMaxBytesPerPacket)
    return -1;

  if (useFilter_)
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
  }

  frame_->pts = nextPts_;
  nextPts_ += framesPerPacket_;
  return EncodeFrameToBurst(outBuf, outSize);
}

int SpdifEncoder::EncodeFrameToBurst(uint8_t* outBuf, int outSize)
{
  int rc = avcodec_send_frame(codecCtx_, frame_);
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
