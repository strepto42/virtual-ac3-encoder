// upmix_wav — offline test tool for the time-domain matrix upmix (upmix_algo=matrix).
//
//   upmix_wav in.wav out.wav [options]
//
// Reads a stereo WAV (16/32-bit PCM or float32; a mono file is doubled, extra channels are ignored), runs
// exactly the same code as the engine and writes a 5.1 WAV (FL FR FC LFE SL SR, float32 or 16-bit) that
// any multichannel player can open. Use it to compare settings by ear without the engine, and to compare
// with the FFT `surround` upmix.
//
//   --settings dsp-settings.json   take the "upmix" section from this file (default: the factory values)
//   --width 0..1  --focus 0..1  --low Hz  --high Hz  --surround-db dB  --delay-ms ms    override single values
//   --bit16                         write 16-bit PCM (clipped) instead of float32
//   --gain-db dB                    trim applied to all six channels (e.g. -6 like the engine's headroom)
#include "WavFile.h"
#include "dsp/DspSettings.h"
#include "dsp/MatrixUpmix.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

void Put16(std::ofstream& f, uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); }
void Put32(std::ofstream& f, uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); }

bool WriteWav(const std::string& path, const std::vector<std::vector<float>>& planes, uint32_t rate, bool bit16)
{
  const uint16_t channels = static_cast<uint16_t>(planes.size());
  const size_t frames = planes[0].size();
  const uint16_t bits = bit16 ? 16 : 32;
  const uint32_t dataBytes = static_cast<uint32_t>(frames * channels * (bits / 8));
  std::ofstream f(path, std::ios::binary);
  if (!f)
    return false;
  f.write("RIFF", 4);
  Put32(f, 4 + 8 + 40 + 8 + dataBytes);
  f.write("WAVE", 4);
  f.write("fmt ", 4);
  Put32(f, 40);                       // WAVE_FORMAT_EXTENSIBLE
  Put16(f, 0xFFFE);
  Put16(f, channels);
  Put32(f, rate);
  Put32(f, rate * channels * (bits / 8));
  Put16(f, static_cast<uint16_t>(channels * (bits / 8)));
  Put16(f, bits);
  Put16(f, 22);                       // size of the extension
  Put16(f, bits);                     // valid bits
  Put32(f, 0x60F);                    // FL FR FC LFE SL SR
  // sub format GUID: PCM {00000001-0000-0010-8000-00aa00389b71} or IEEE float {00000003-...}
  Put32(f, bit16 ? 1u : 3u);
  Put16(f, 0x0000);
  Put16(f, 0x0010);
  const uint8_t tail[8] = {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
  f.write(reinterpret_cast<const char*>(tail), 8);
  f.write("data", 4);
  Put32(f, dataBytes);
  for (size_t i = 0; i < frames; ++i)
    for (const auto& ch : planes)
    {
      if (bit16)
      {
        const float v = std::min(1.0f, std::max(-1.0f, ch[i]));
        Put16(f, static_cast<uint16_t>(static_cast<int16_t>(std::lround(v * 32767.0f))));
      }
      else
        f.write(reinterpret_cast<const char*>(&ch[i]), 4);
    }
  return static_cast<bool>(f);
}

double Db(double v) { return 20.0 * std::log10(v + 1e-12); }

}  // namespace

int main(int argc, char** argv)
{
  if (argc < 3)
  {
    std::fprintf(stderr, "usage: upmix_wav in.wav out.wav [--settings dsp-settings.json] [--width 0..1] [--focus 0..1]\n"
                         "                 [--low Hz] [--high Hz] [--surround-db dB] [--delay-ms ms] [--bit16] [--gain-db dB]\n");
    return 2;
  }
  vac3dsp::DspParams params;
  bool bit16 = false;
  float gainDb = 0.0f;
  for (int i = 3; i < argc; ++i)
  {
    const std::string a = argv[i];
    auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : "0"; };
    if (a == "--settings")
    {
      std::ifstream in(next(), std::ios::binary);
      std::stringstream ss;
      ss << in.rdbuf();
      std::string err;
      if (!vac3dsp::ParseSettings(ss.str(), params, &err))
      {
        std::fprintf(stderr, "cannot read settings: %s\n", err.c_str());
        return 1;
      }
    }
  }
  for (int i = 3; i < argc; ++i)
  {
    const std::string a = argv[i];
    auto next = [&]() -> float { return i + 1 < argc ? std::strtof(argv[++i], nullptr) : 0.0f; };
    if (a == "--settings") ++i;
    else if (a == "--width") params.upmix.centerWidth = next();
    else if (a == "--focus") params.upmix.centerFocus = next();
    else if (a == "--low") params.upmix.centerLowHz = next();
    else if (a == "--high") params.upmix.centerHighHz = next();
    else if (a == "--surround-db") params.upmix.surroundLevelDb = next();
    else if (a == "--delay-ms") params.upmix.surroundDelayMs = next();
    else if (a == "--gain-db") gainDb = next();
    else if (a == "--bit16") bit16 = true;
  }
  params.upmix.enabled = 1;
  vac3dsp::Sanitize(params);

  WavFile wav;
  std::string error;
  if (!WavFile::Load(argv[1], wav, error))
  {
    std::fprintf(stderr, "cannot read %s: %s\n", argv[1], error.c_str());
    return 1;
  }
  const size_t frames = wav.numFrames();
  if (frames == 0 || wav.channels == 0)
  {
    std::fprintf(stderr, "empty input\n");
    return 1;
  }
  std::vector<std::vector<float>> planes(6, std::vector<float>(frames, 0.0f));
  const size_t bytes = wav.bitsPerSample / 8;
  for (size_t i = 0; i < frames; ++i)
    for (int c = 0; c < 2; ++c)
    {
      const size_t ch = std::min<size_t>(static_cast<size_t>(c), static_cast<size_t>(wav.channels) - 1);   // mono: both sides
      const uint8_t* p = wav.data.data() + (i * wav.channels + ch) * bytes;
      float v = 0.0f;
      if (wav.isFloat && wav.bitsPerSample == 32) { float x; std::memcpy(&x, p, 4); v = x; }
      else if (wav.bitsPerSample == 16) { int16_t x; std::memcpy(&x, p, 2); v = static_cast<float>(x) / 32768.0f; }
      else if (wav.bitsPerSample == 32) { int32_t x; std::memcpy(&x, p, 4); v = static_cast<float>(x / 2147483648.0); }
      else { std::fprintf(stderr, "unsupported sample format (%u bit)\n", wav.bitsPerSample); return 1; }
      planes[static_cast<size_t>(c)][i] = v;
    }

  vac3dsp::MatrixUpmix up;
  up.Configure(static_cast<int>(wav.sampleRate));
  up.SetParams(params.upmix);
  up.Reset();
  up.SetForce(true);
  for (size_t pos = 0; pos < frames; pos += 1536)   // the engine's packet size
  {
    float* pl[6];
    for (size_t c = 0; c < 6; ++c)
      pl[c] = planes[c].data() + pos;
    up.Process(pl, static_cast<int>(std::min<size_t>(1536, frames - pos)));
  }
  const float gain = std::pow(10.0f, gainDb / 20.0f);
  if (gain != 1.0f)
    for (auto& ch : planes)
      for (float& v : ch)
        v *= gain;

  static const char* const names[6] = {"FL", "FR", "FC", "LFE", "SL", "SR"};
  std::printf("upmix: centre %.0f..%.0f Hz, width %.2f, focus %.2f; surround %.1f dB, delay %.1f ms\n",
              static_cast<double>(params.upmix.centerLowHz), static_cast<double>(params.upmix.centerHighHz),
              static_cast<double>(params.upmix.centerWidth), static_cast<double>(params.upmix.centerFocus),
              static_cast<double>(params.upmix.surroundLevelDb), static_cast<double>(params.upmix.surroundDelayMs));
  for (size_t c = 0; c < 6; ++c)
  {
    double peak = 0.0, sum = 0.0;
    for (float v : planes[c])
    {
      peak = std::max(peak, static_cast<double>(std::fabs(v)));
      sum += static_cast<double>(v) * v;
    }
    std::printf("  %-3s peak %7.1f dBFS   rms %7.1f dBFS\n", names[c], Db(peak), Db(std::sqrt(sum / static_cast<double>(frames))));
  }
  if (!WriteWav(argv[2], planes, wav.sampleRate, bit16))
  {
    std::fprintf(stderr, "cannot write %s\n", argv[2]);
    return 1;
  }
  std::printf("wrote %s (%s, 5.1)\n", argv[2], bit16 ? "16-bit" : "float32");
  return 0;
}
