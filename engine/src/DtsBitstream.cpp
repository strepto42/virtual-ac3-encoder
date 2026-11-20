#include "DtsBitstream.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

bool DtsBitstreamDecoder::Init(const AVChannelLayout& outLayout, int outSampleRate)
{
  Close();
  const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_DTS);
  if (!codec)
  {
    std::fprintf(stderr, "[DtsIn] this FFmpeg build has no DTS decoder\n");
    return false;
  }
  ctx_ = avcodec_alloc_context3(codec);
  pkt_ = av_packet_alloc();
  frame_ = av_frame_alloc();
  if (!ctx_ || !pkt_ || !frame_ || avcodec_open2(ctx_, codec, nullptr) < 0)
  {
    std::fprintf(stderr, "[DtsIn] cannot open the DTS decoder\n");
    Close();
    return false;
  }
  if (av_channel_layout_copy(&outLayout_, &outLayout) < 0)
  {
    Close();
    return false;
  }
  outRate_ = outSampleRate;
  fifo_ = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP, outLayout_.nb_channels, std::max(1024, outRate_ / 10));
  if (!fifo_)
  {
    Close();
    return false;
  }
  return true;
}

void DtsBitstreamDecoder::Close()
{
  if (swr_) swr_free(&swr_);
  if (fifo_) { av_audio_fifo_free(fifo_); fifo_ = nullptr; }
  if (frame_) av_frame_free(&frame_);
  if (pkt_) av_packet_free(&pkt_);
  if (ctx_) avcodec_free_context(&ctx_);
  av_channel_layout_uninit(&outLayout_);
  av_channel_layout_uninit(&swrInLayout_);
  swrInFmt_ = AV_SAMPLE_FMT_NONE;
  swrInRate_ = 0;
  active_ = false;
  loggedStream_ = false;
  goodBursts_ = 0;
  framesSinceBurst_ = 0;
  scanner_ = Iec61937Scanner();
}

bool DtsBitstreamDecoder::Read(void** planes, int frames)
{
  if (!fifo_ || av_audio_fifo_size(fifo_) < frames)
    return false;
  return av_audio_fifo_read(fifo_, planes, frames) == frames;
}

void DtsBitstreamDecoder::Deactivate()
{
  active_ = false;
  loggedStream_ = false;
  goodBursts_ = 0;
  if (fifo_) av_audio_fifo_reset(fifo_);
  if (ctx_) avcodec_flush_buffers(ctx_);
}

void DtsBitstreamDecoder::Push(const int16_t* lrWords, int frames)
{
  if (!ctx_ || frames <= 0)
    return;
  scanner_.Push(lrWords, static_cast<size_t>(frames) * 2);
  framesSinceBurst_ = std::min(framesSinceBurst_ + frames, 1 << 28);

  Iec61937Burst burst;
  while (scanner_.TakeBurst(burst))
  {
    framesSinceBurst_ = 0;
    if (DecodeBurst(burst))
      ++goodBursts_;
    if (!active_ && goodBursts_ >= 2)
    {
      active_ = true;
      std::printf("[DtsIn] DTS bitstream detected (IEC 61937 type %d) -> decoding to PCM 5.1\n",
                  burst.dataType);
    }
  }

  if (!loggedUnsupported_ && !active_ && scanner_.unsupportedBursts() > 0)
  {
    loggedUnsupported_ = true;
    std::printf("[DtsIn] IEC 61937 stream with data type %d found: only DTS core (types 11-13) is "
                "decoded; this stream is treated as PCM\n", scanner_.lastUnsupportedType());
  }
  if (active_ && framesSinceBurst_ > outRate_ * 2 / 5)
  {
    std::printf("[DtsIn] DTS bitstream ended -> PCM input\n");
    Deactivate();
  }
  else if (!active_ && framesSinceBurst_ > outRate_)
  {
    goodBursts_ = 0; // sporadic false syncs must not add up
  }

  // Bound the latency if the consumer ever stalls: keep at most 1 s of decoded audio.
  const int excess = Available() - outRate_;
  if (excess > 0)
    av_audio_fifo_drain(fifo_, excess);
}

bool DtsBitstreamDecoder::DecodeBurst(const Iec61937Burst& burst)
{
  if (burst.payload.empty() || av_new_packet(pkt_, static_cast<int>(burst.payload.size())) < 0)
    return false;
  std::memcpy(pkt_->data, burst.payload.data(), burst.payload.size());
  int rc = avcodec_send_packet(ctx_, pkt_);
  av_packet_unref(pkt_);
  if (rc < 0)
    return false;

  bool produced = false;
  while ((rc = avcodec_receive_frame(ctx_, frame_)) == 0)
  {
    QueueFrame(frame_);
    produced = true;
    av_frame_unref(frame_);
  }
  return produced;
}

// Convert one decoded DTS frame to the output layout / rate / planar float and queue it.
void DtsBitstreamDecoder::QueueFrame(const AVFrame* f)
{
  const int inRate = f->sample_rate > 0 ? f->sample_rate : outRate_;
  const AVSampleFormat inFmt = static_cast<AVSampleFormat>(f->format);

  if (!swr_ || swrInRate_ != inRate || swrInFmt_ != inFmt ||
      av_channel_layout_compare(&swrInLayout_, &f->ch_layout) != 0)
  {
    if (swr_) swr_free(&swr_);
    av_channel_layout_uninit(&swrInLayout_);
    if (av_channel_layout_copy(&swrInLayout_, &f->ch_layout) < 0)
      return;
    swrInRate_ = inRate;
    swrInFmt_ = inFmt;
    int rc = swr_alloc_set_opts2(&swr_, &outLayout_, AV_SAMPLE_FMT_FLTP, outRate_, &f->ch_layout,
                                 inFmt, inRate, 0, nullptr);
    if (rc < 0 || !swr_ || swr_init(swr_) < 0)
    {
      std::fprintf(stderr, "[DtsIn] cannot convert the decoded DTS layout/rate\n");
      if (swr_) swr_free(&swr_);
      return;
    }
  }
  if (!loggedStream_)
  {
    loggedStream_ = true;
    char desc[64] = {0};
    av_channel_layout_describe(&f->ch_layout, desc, sizeof desc);
    std::printf("[DtsIn] decoded stream: %d Hz, %d ch (%s)\n", inRate, f->ch_layout.nb_channels, desc);
  }

  const int maxOut = swr_get_out_samples(swr_, f->nb_samples);
  if (maxOut <= 0)
    return;
  const size_t channels = static_cast<size_t>(outLayout_.nb_channels);
  if (conv_.size() != channels)
  {
    conv_.assign(channels, std::vector<float>());
    convPtrs_.assign(channels, nullptr);
  }
  for (size_t c = 0; c < channels; ++c)
  {
    if (conv_[c].size() < static_cast<size_t>(maxOut))
      conv_[c].resize(static_cast<size_t>(maxOut));
    convPtrs_[c] = reinterpret_cast<uint8_t*>(conv_[c].data());
  }
  const int got = swr_convert(swr_, convPtrs_.data(), maxOut,
                              reinterpret_cast<const uint8_t* const*>(f->extended_data), f->nb_samples);
  if (got > 0)
    av_audio_fifo_write(fifo_, reinterpret_cast<void**>(convPtrs_.data()), got);
}
