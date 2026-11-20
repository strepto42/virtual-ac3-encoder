// DspParams.h — parameter block of the DSP chain (EQ -> compressor), shared by the engine and by
// VirtualAc3 DSP Studio. Plain data only (no pointers, no std types with state), so the same
// struct can later travel through shared memory or be written to dsp-settings.json.
//
// Channel order everywhere is the encoder's 5.1 order: L, R, C, LFE, Ls, Rs.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <type_traits>

namespace vac3dsp {

constexpr int kMaxChannels = 6;
constexpr int kLfeChannel = 3;
constexpr int kBands = 30;

// 30 bands, logarithmically spaced from 20 Hz to 20 kHz: f[i] = 20 Hz * 1000^(i/29), a constant
// ratio of 1.269 (0.344 octave) between neighbours. The UI shows rounded labels.
constexpr float kBandHz[kBands] = {
    20.0000f,   25.3792f,   32.2052f,   40.8672f,   51.8589f,    65.8069f,    83.5064f,
    105.9663f,  134.4672f,  170.6336f,  216.5273f,  274.7648f,   348.6658f,   442.4433f,
    561.4432f,  712.4496f,  904.0707f,  1147.2305f, 1455.7908f,  1847.3417f,  2344.2046f,
    2974.7042f, 3774.7836f, 4790.0532f, 6078.3908f, 7713.2408f,  9787.8018f,  12420.3388f,
    15760.9256f, 20000.0000f};

// Width of every band (octaves) = distance between neighbouring centres, so the bands tile evenly.
constexpr double kBandWidthOct = 0.34364773395386505;  // log2(1000) / 29

// Index of the band whose centre is closest (on a log scale) to `hz`.
inline int NearestBand(float hz)
{
  int best = 0;
  float bestDist = 1e30f;
  for (int b = 0; b < kBands; ++b)
  {
    const float d = std::fabs(std::log(hz / kBandHz[b]));
    if (d < bestDist)
    {
      bestDist = d;
      best = b;
    }
  }
  return best;
}

constexpr float kEqBandMaxDb = 12.0f;   // each band: -12 .. +12 dB
constexpr float kEqPreampMinDb = -24.0f;
constexpr float kEqPreampMaxDb = 12.0f;

// Filter shape of one band. Bell, low shelf and high shelf use the band's gain; notch and the two
// cut filters (high-pass = "low cut", low-pass = "high cut") ignore it and always act when selected.
enum BandType : int32_t
{
  kBandBell = 0,
  kBandLowShelf = 1,
  kBandHighShelf = 2,
  kBandNotch = 3,
  kBandHighPass = 4,
  kBandLowPass = 5
};
constexpr int kBandTypeCount = 6;

inline int NormalizeBandType(int t) { return (t >= 0 && t < kBandTypeCount) ? t : static_cast<int>(kBandBell); }
inline bool BandTypeUsesGain(int t) { return NormalizeBandType(t) <= static_cast<int>(kBandHighShelf); }

constexpr float kBandFreqMinHz = 20.0f;
constexpr float kBandFreqMaxHz = 20000.0f;
constexpr float kBandWidthMinOct = 0.05f;   // Q about 28
constexpr float kBandWidthMaxOct = 4.0f;    // Q about 0.27

// Width a band gets by default: bell/notch tile the spectrum like the original graphic EQ, shelves and cut
// filters get 1.9 octaves (Q 0.707, the classic maximally flat slope).
inline float DefaultBandWidthOct(int type)
{
  const int t = NormalizeBandType(type);
  return (t == kBandBell || t == kBandNotch) ? static_cast<float>(kBandWidthOct) : 1.9f;
}

// Centre, width and shape of one band. Shared by all channels (the gains below are per channel).
struct BandShape
{
  float freqHz = 1000.0f;      // 20 .. 20000
  float widthOct = 0.3436f;    // 0.05 .. 4 octaves (the -3 dB style bandwidth; Q = 1 / (2 sinh(ln2/2 * width)) near 1 octave)
  int32_t type = kBandBell;
};

struct EqParams
{
  int32_t enabled = 1;
  float preampDb = 0.0f;                       // -24 .. +12, applied before the bands
  float bandDb[kMaxChannels][kBands] = {};     // per channel, per band: -12 .. +12
  BandShape shape[kBands];                     // per band (all channels): frequency, width, shape

  EqParams()
  {
    for (int b = 0; b < kBands; ++b)
    {
      shape[b].freqHz = kBandHz[b];
      shape[b].widthOct = static_cast<float>(kBandWidthOct);
      shape[b].type = kBandBell;
    }
  }
};

// The factory layout: 30 bell bands on the table above.
inline const BandShape* DefaultBandShapes()
{
  static const EqParams defaults;
  return defaults.shape;
}

struct CompParams
{
  int32_t enabled = 0;
  float thresholdDb = -18.0f;  // -60 .. 0
  float ratio = 3.0f;          // 1 .. 20
  float attackMs = 10.0f;      // 0.1 .. 200
  float releaseMs = 120.0f;    // 5 .. 2000
  float kneeDb = 6.0f;         // 0 .. 24
  float makeupDb = 0.0f;       // 0 .. 24
  int32_t lfeInDetector = 0;   // 0: the LFE channel does not trigger the compressor
};

// Stereo -> 5.1 matrix upmix (engine option upmix_algo=matrix). It does not depend on the master switch
// below: that one is for the EQ and the compressor.
struct UpmixParams
{
  int32_t enabled = 1;            // 0 = bypass: the stereo signal stays in FL/FR
  float centerWidth = 0.5f;       // 0 .. 1  how far off-centre a source may sit and still feed the centre channel
  float centerFocus = 0.5f;       // 0 .. 1  how strictly left and right must agree (0 = also diffuse content)
  float centerLowHz = 150.0f;     // 60 .. 400   the centre is taken from this band only,
  float centerHighHz = 5000.0f;   // 2000 .. 12000   bass and treble stay in L and R
  float surroundLevelDb = -3.0f;  // -24 .. +6
  float surroundDelayMs = 10.0f;  // 0 .. 40
  int32_t lfeEnabled = 0;         // 1 = feed the LFE channel with the low-passed mono sum of L and R
  float lfeLevelDb = -6.0f;       // -24 .. +6
  float lfeCutHz = 100.0f;        // 40 .. 200 (low-pass corner, 4th order)
};

struct DspParams
{
  int32_t enabled = 0;   // master switch; 0 = the chain is an exact bypass
  int32_t order = 0;     // 0: EQ -> compressor, 1: compressor -> EQ
  EqParams eq;
  CompParams comp;
  UpmixParams upmix;
};

static_assert(std::is_trivially_copyable<DspParams>::value, "DspParams must be plain data");
static_assert(std::is_standard_layout<DspParams>::value, "DspParams must be plain data");

namespace detail {
inline float Clamp(float v, float lo, float hi, float fallback)
{
  if (!std::isfinite(v))
    return fallback;
  return std::min(std::max(v, lo), hi);
}
}  // namespace detail

// Forces every field into its legal range (NaN/Inf -> default). Always call this on parameters that
// come from a file or from another process.
inline void Sanitize(DspParams& p)
{
  using detail::Clamp;
  p.enabled = p.enabled != 0 ? 1 : 0;
  p.order = p.order == 1 ? 1 : 0;

  p.eq.enabled = p.eq.enabled != 0 ? 1 : 0;
  p.eq.preampDb = Clamp(p.eq.preampDb, kEqPreampMinDb, kEqPreampMaxDb, 0.0f);
  for (int c = 0; c < kMaxChannels; ++c)
    for (int b = 0; b < kBands; ++b)
      p.eq.bandDb[c][b] = Clamp(p.eq.bandDb[c][b], -kEqBandMaxDb, kEqBandMaxDb, 0.0f);
  for (int b = 0; b < kBands; ++b)
  {
    BandShape& sh = p.eq.shape[b];
    sh.freqHz = Clamp(sh.freqHz, kBandFreqMinHz, kBandFreqMaxHz, kBandHz[b]);
    sh.type = NormalizeBandType(sh.type);
    sh.widthOct = Clamp(sh.widthOct, kBandWidthMinOct, kBandWidthMaxOct, DefaultBandWidthOct(sh.type));
  }

  CompParams& k = p.comp;
  k.enabled = k.enabled != 0 ? 1 : 0;
  k.thresholdDb = Clamp(k.thresholdDb, -60.0f, 0.0f, -18.0f);
  k.ratio = Clamp(k.ratio, 1.0f, 20.0f, 3.0f);
  k.attackMs = Clamp(k.attackMs, 0.1f, 200.0f, 10.0f);
  k.releaseMs = Clamp(k.releaseMs, 5.0f, 2000.0f, 120.0f);
  k.kneeDb = Clamp(k.kneeDb, 0.0f, 24.0f, 6.0f);
  k.makeupDb = Clamp(k.makeupDb, 0.0f, 24.0f, 0.0f);
  k.lfeInDetector = k.lfeInDetector != 0 ? 1 : 0;

  UpmixParams& u = p.upmix;
  u.enabled = u.enabled != 0 ? 1 : 0;
  u.centerWidth = Clamp(u.centerWidth, 0.0f, 1.0f, 0.5f);
  u.centerFocus = Clamp(u.centerFocus, 0.0f, 1.0f, 0.5f);
  u.centerLowHz = Clamp(u.centerLowHz, 60.0f, 400.0f, 150.0f);
  u.centerHighHz = Clamp(u.centerHighHz, 2000.0f, 12000.0f, 5000.0f);
  u.surroundLevelDb = Clamp(u.surroundLevelDb, -24.0f, 6.0f, -3.0f);
  u.surroundDelayMs = Clamp(u.surroundDelayMs, 0.0f, 40.0f, 10.0f);
  u.lfeEnabled = u.lfeEnabled != 0 ? 1 : 0;
  u.lfeLevelDb = Clamp(u.lfeLevelDb, -24.0f, 6.0f, -6.0f);
  u.lfeCutHz = Clamp(u.lfeCutHz, 40.0f, 200.0f, 100.0f);
}

}  // namespace vac3dsp
