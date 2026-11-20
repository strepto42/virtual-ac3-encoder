// DtsBitstream.h — decodes a DTS bitstream that arrives as IEC 61937 bursts inside a 16-bit
// stereo PCM carrier (what a media player sends when "S/PDIF / DTS passthrough" is enabled).
//
//   carrier words --Iec61937Scanner--> DTS core frames --libavcodec "dca"--> 5.1 float PCM
//   --libswresample (layout / rate conversion)--> planar-float FIFO in the encoder's layout
//
// Everything runs on the caller's (audio) thread; Push() never blocks.
#pragma once

#include <cstdint>
#include <vector>

#include "Iec61937.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

class DtsBitstreamDecoder
{
public:
  DtsBitstreamDecoder() = default;
  ~DtsBitstreamDecoder() { Close(); }
  DtsBitstreamDecoder(const DtsBitstreamDecoder&) = delete;
  DtsBitstreamDecoder& operator=(const DtsBitstreamDecoder&) = delete;

  // outLayout / outSampleRate: what the consumer (the AC3 encoder) wants to read.
  bool Init(const AVChannelLayout& outLayout, int outSampleRate);
  void Close();

  // Feed `frames` stereo frames of the carrier as int16 words (L0 R0 L1 R1 ...).
  void Push(const int16_t* lrWords, int frames);

  // True while DTS bursts keep arriving (>= 2 decoded bursts, no gap longer than 0.4 s).
  bool Active() const { return active_; }
  // Decoded frames waiting in the FIFO.
  int Available() const { return fifo_ ? av_audio_fifo_size(fifo_) : 0; }
  // Pop exactly `frames` planar-float frames (one plane per output channel).
  bool Read(void** planes, int frames);

private:
  bool DecodeBurst(const Iec61937Burst& burst);
  void QueueFrame(const AVFrame* f);
  void Deactivate();

  Iec61937Scanner  scanner_;
  AVCodecContext*  ctx_ = nullptr;
  AVPacket*        pkt_ = nullptr;
  AVFrame*         frame_ = nullptr;
  SwrContext*      swr_ = nullptr;
  AVAudioFifo*     fifo_ = nullptr;
  AVChannelLayout  outLayout_{};
  AVChannelLayout  swrInLayout_{};
  AVSampleFormat   swrInFmt_ = AV_SAMPLE_FMT_NONE;
  int              swrInRate_ = 0;
  int              outRate_ = 48000;

  std::vector<std::vector<float>> conv_;  // swr output scratch (one vector per channel)
  std::vector<uint8_t*>           convPtrs_;

  bool active_ = false;
  bool loggedStream_ = false;
  bool loggedUnsupported_ = false;
  int  goodBursts_ = 0;
  int  framesSinceBurst_ = 0;
};
