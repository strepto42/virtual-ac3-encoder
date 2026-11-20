// StudioModel.h — editing logic of DSP Studio without any GUI: which curve is shown, how a slider
// change is applied to the channels, auto-preamp, labels. Unit-tested; the Qt window only calls it.
#pragma once

#include "DspParams.h"
#include "DspSettings.h"
#include "GraphicEq.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace vac3dsp {

constexpr int kAllChannels = -1;
constexpr int kCurvePoints = 241;   // points of the drawn response curve (20 Hz .. 20 kHz, log scale)

inline double CurveFrequency(int i)
{
  return 20.0 * std::pow(1000.0, static_cast<double>(i) / static_cast<double>(kCurvePoints - 1));
}

// "20", "25", ..., "904", "1.1k", ... "20k" (slider captions)
inline std::string FreqLabel(double f)
{
  char buf[16];
  if (f < 1000.0)
    std::snprintf(buf, sizeof buf, "%.0f", f);
  else if (f < 9950.0)
    std::snprintf(buf, sizeof buf, "%.1fk", f / 1000.0);
  else
    std::snprintf(buf, sizeof buf, "%.0fk", f / 1000.0);
  return buf;
}
inline std::string BandLabel(int band) { return FreqLabel(static_cast<double>(kBandHz[band])); }

// Two-letter tag of a shape for the little indicator under each fader.
inline const char* BandTypeShort(int type)
{
  switch (NormalizeBandType(type))
  {
    case kBandLowShelf: return "LS";
    case kBandHighShelf: return "HS";
    case kBandNotch: return "N";
    case kBandHighPass: return "HP";
    case kBandLowPass: return "LP";
    default: return "B";
  }
}

// 0..1 length of the width bar of the indicator (logarithmic between the minimum and maximum width).
inline float WidthFraction(float widthOct)
{
  const float w = std::min(kBandWidthMaxOct, std::max(kBandWidthMinOct, widthOct));
  return std::log(w / kBandWidthMinOct) / std::log(kBandWidthMaxOct / kBandWidthMinOct);
}

// Two parameter sets are "the same" when they would be written to the file identically (float noise below
// the file's precision is not an edit).
inline bool SameParams(const DspParams& a, const DspParams& b) { return FormatSettings(a) == FormatSettings(b); }

class StudioModel
{
public:
  DspParams params;                  // what is written to dsp-settings.json
  int channel = kAllChannels;        // curve being edited: kAllChannels (linked) or 0..5
  int selectedBand = 0;              // band shown in the "selected band" row

  // Gain of `band` as displayed: the selected channel, or the front-left one when all are linked.
  float Band(int band) const { return params.eq.bandDb[Shown()][band]; }

  // A slider / curve drag: linked edits move the band on every channel, a channel edit only on that one.
  void SetBand(int band, float db)
  {
    db = std::min(kEqBandMaxDb, std::max(-kEqBandMaxDb, std::round(db * 10.0f) / 10.0f));
    if (channel == kAllChannels)
      for (int c = 0; c < kMaxChannels; ++c)
        params.eq.bandDb[c][band] = db;
    else
      params.eq.bandDb[channel][band] = db;
  }

  // Shape of a band (shared by all channels). Out-of-range values are clamped.
  const BandShape& Shape(int band) const { return params.eq.shape[band]; }

  void SetFreq(int band, float hz)
  {
    hz = std::isfinite(hz) ? hz : kBandHz[band];
    params.eq.shape[band].freqHz = std::min(kBandFreqMaxHz, std::max(kBandFreqMinHz, std::round(hz * 100.0f) / 100.0f));
  }

  void SetWidth(int band, float widthOct)
  {
    widthOct = std::isfinite(widthOct) ? widthOct : static_cast<float>(kBandWidthOct);
    params.eq.shape[band].widthOct =
        std::min(kBandWidthMaxOct, std::max(kBandWidthMinOct, std::round(widthOct * 1000.0f) / 1000.0f));
  }

  // A new shape keeps a width the user chose; a width that was still the old shape's default follows the
  // new shape's default (a 0.34-octave high-pass would ring badly).
  void SetType(int band, int type)
  {
    BandShape& sh = params.eq.shape[band];
    type = NormalizeBandType(type);
    if (std::fabs(sh.widthOct - DefaultBandWidthOct(sh.type)) < 0.002f)
      sh.widthOct = DefaultBandWidthOct(type);
    sh.type = type;
  }

  // Back to the factory band: own frequency, width, bell shape and 0 dB (on the shown channels).
  void ResetBand(int band)
  {
    params.eq.shape[band].freqHz = kBandHz[band];
    params.eq.shape[band].widthOct = static_cast<float>(kBandWidthOct);
    params.eq.shape[band].type = kBandBell;
    SetBand(band, 0.0f);
  }

  double Q(int band, int sampleRate) const
  {
    return GraphicEq::WidthToQ(static_cast<double>(params.eq.shape[band].widthOct),
                               static_cast<double>(params.eq.shape[band].freqHz), sampleRate,
                               params.eq.shape[band].type);
  }

  // True when the channels do not share one curve (the linked view then shows the front-left curve).
  bool ChannelsDiffer() const
  {
    for (int c = 1; c < kMaxChannels; ++c)
      for (int b = 0; b < kBands; ++b)
        if (params.eq.bandDb[c][b] != params.eq.bandDb[0][b])
          return true;
    return false;
  }

  // Everything back to factory: all gains 0, the 30 bell bands on their original frequencies and widths.
  void ResetEq()
  {
    for (int c = 0; c < kMaxChannels; ++c)
      for (int b = 0; b < kBands; ++b)
        params.eq.bandDb[c][b] = 0.0f;
    params.eq.preampDb = 0.0f;
    const BandShape* def = DefaultBandShapes();
    for (int b = 0; b < kBands; ++b)
      params.eq.shape[b] = def[b];
  }

  // Response of the shown curve (dB) at the kCurvePoints log-spaced frequencies.
  void Curve(int sampleRate, double* outDb) const
  {
    double f[kCurvePoints];
    for (int i = 0; i < kCurvePoints; ++i)
      f[i] = CurveFrequency(i);
    GraphicEq::ResponseCurveDb(params.eq.bandDb[Shown()], params.eq.shape, sampleRate, f, kCurvePoints, outDb);
  }

  // Preamp (dB) that cancels the largest boost of every channel's curve, rounded up to 0.5 dB.
  float AutoPreampDb(int sampleRate) const
  {
    double peak = 0.0;
    double db[kCurvePoints];
    double f[kCurvePoints];
    for (int i = 0; i < kCurvePoints; ++i)
      f[i] = CurveFrequency(i);
    for (int c = 0; c < kMaxChannels; ++c)
    {
      GraphicEq::ResponseCurveDb(params.eq.bandDb[c], params.eq.shape, sampleRate, f, kCurvePoints, db);
      for (int i = 0; i < kCurvePoints; ++i)
        peak = std::max(peak, db[i]);
    }
    if (peak <= 0.0)
      return 0.0f;
    return std::max(kEqPreampMinDb, -static_cast<float>(std::ceil(peak * 2.0) / 2.0));
  }

private:
  int Shown() const { return channel == kAllChannels ? 0 : std::min(std::max(channel, 0), kMaxChannels - 1); }
};

}  // namespace vac3dsp
