// PcmDsp.h — runs the DSP chain over an interleaved stereo PCM buffer (the PCM output mode).
//
// The encoder path hands the chain planar float; the PCM output works on the shared-mode device
// format (float32, int16 or int32 interleaved), so the buffer is converted to float, processed and
// converted back. When the chain is a pure bypass nothing is touched, so a disabled DSP never
// changes a single bit of the stream. Converting back clamps to full scale: the PCM path has no
// look-ahead limiter, so an EQ boost on a loud signal clips instead of wrapping around.
#pragma once

#include "DspChain.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace vac3dsp {

enum class PcmFormat { Float32, Int16, Int32 };

// `chain` must have been configured for 2 channels. `left` / `right` are scratch planes with room for
// `frames` samples. Returns true when the audio was processed, false when the chain was bypassed.
inline bool ProcessInterleavedStereo(DspChain& chain, PcmFormat fmt, void* data, int frames, float* left,
                                     float* right)
{
  if (!data || frames <= 0)
    return false;
  chain.PullParams();
  if (chain.Bypassed())
    return false;

  switch (fmt)
  {
    case PcmFormat::Float32:
    {
      const float* s = static_cast<const float*>(data);
      for (int i = 0; i < frames; ++i)
      {
        left[i] = s[2 * i];
        right[i] = s[2 * i + 1];
      }
      break;
    }
    case PcmFormat::Int16:
    {
      const int16_t* s = static_cast<const int16_t*>(data);
      for (int i = 0; i < frames; ++i)
      {
        left[i] = static_cast<float>(s[2 * i]) * (1.0f / 32768.0f);
        right[i] = static_cast<float>(s[2 * i + 1]) * (1.0f / 32768.0f);
      }
      break;
    }
    case PcmFormat::Int32:
    {
      const int32_t* s = static_cast<const int32_t*>(data);
      for (int i = 0; i < frames; ++i)
      {
        left[i] = static_cast<float>(static_cast<double>(s[2 * i]) * (1.0 / 2147483648.0));
        right[i] = static_cast<float>(static_cast<double>(s[2 * i + 1]) * (1.0 / 2147483648.0));
      }
      break;
    }
  }

  float* planes[2] = {left, right};
  chain.Process(planes, frames);

  switch (fmt)
  {
    case PcmFormat::Float32:
    {
      float* d = static_cast<float*>(data);
      for (int i = 0; i < frames; ++i)
      {
        d[2 * i] = std::min(1.0f, std::max(-1.0f, left[i]));
        d[2 * i + 1] = std::min(1.0f, std::max(-1.0f, right[i]));
      }
      break;
    }
    case PcmFormat::Int16:
    {
      int16_t* d = static_cast<int16_t*>(data);
      for (int i = 0; i < frames; ++i)
      {
        const long l = std::lround(static_cast<double>(left[i]) * 32768.0);
        const long r = std::lround(static_cast<double>(right[i]) * 32768.0);
        d[2 * i] = static_cast<int16_t>(std::min(32767L, std::max(-32768L, l)));
        d[2 * i + 1] = static_cast<int16_t>(std::min(32767L, std::max(-32768L, r)));
      }
      break;
    }
    case PcmFormat::Int32:
    {
      int32_t* d = static_cast<int32_t*>(data);
      for (int i = 0; i < frames; ++i)
      {
        const double l = std::round(static_cast<double>(left[i]) * 2147483648.0);
        const double r = std::round(static_cast<double>(right[i]) * 2147483648.0);
        d[2 * i] = static_cast<int32_t>(std::min(2147483647.0, std::max(-2147483648.0, l)));
        d[2 * i + 1] = static_cast<int32_t>(std::min(2147483647.0, std::max(-2147483648.0, r)));
      }
      break;
    }
  }
  return true;
}

}  // namespace vac3dsp
