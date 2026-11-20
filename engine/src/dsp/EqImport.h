// EqImport.h — read and write parametric EQ text presets in the "Equalizer APO" format, which is also what
// AutoEq and REW (Room EQ Wizard) export:
//
//   Preamp: -5.9 dB
//   Filter 1: ON LSC Fc 105 Hz Gain 6.2 dB Q 0.70
//   Filter 2: ON PK Fc 2600 Hz Gain -4.1 dB Q 2.10
//   Filter: ON PK Fc 1000 Hz Gain 3 dB BW Oct 0.5
//
// Every filter lands on one of the 30 bands with its own frequency, width and shape (our bands can do that),
// so the imported curve is the real one, not an approximation on a fixed grid. PK = bell, LS/LSC = low shelf,
// HS/HSC = high shelf, NO = notch, HP/HPQ = high-pass, LP/LPQ = low-pass. Not supported (skipped, with a note):
// BP, AP, Channel: lines, Copy / Eval / Convolution.
#pragma once

#include "DspParams.h"
#include "GraphicEq.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace vac3dsp {

struct EqImportResult
{
  int imported = 0;
  int skipped = 0;
  bool preamp = false;
  std::vector<std::string> notes;
};

namespace eqimport_detail {

inline std::string Upper(std::string s)
{
  for (char& c : s)
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

inline std::vector<std::string> Tokens(const std::string& line)
{
  std::vector<std::string> out;
  std::string cur;
  for (char c : line)
  {
    if (c == ' ' || c == '\t' || c == '\r' || c == ':')
    {
      if (!cur.empty())
        out.push_back(cur), cur.clear();
    }
    else
      cur += c;
  }
  if (!cur.empty())
    out.push_back(cur);
  return out;
}

inline bool Number(std::string t, double& v)
{
  for (char& c : t)
    if (c == ',')
      c = '.';   // decimal comma of some locales
  char* end = nullptr;
  v = std::strtod(t.c_str(), &end);
  return end != t.c_str() && *end == '\0' && std::isfinite(v);
}

// Width in octaves for a Q (see GraphicEq: bell and notch are pre-warped, shelves and cuts use plain Q).
inline double WidthFromQ(double q, double hz, int rate, int type)
{
  const double kPi = 3.14159265358979323846;
  const double w0 = 2.0 * kPi * hz / static_cast<double>(rate);
  double width = (2.0 / std::log(2.0)) * std::asinh(1.0 / (2.0 * q));
  if (type == kBandBell || type == kBandNotch)
    width *= std::sin(w0) / w0;
  return width;
}

struct Filter
{
  int type = kBandBell;
  double fc = 1000.0, gain = 0.0, q = 0.0, bw = 0.0;
  bool hasQ = false, hasBw = false;
};

}  // namespace eqimport_detail

// Replaces the whole curve in `eq` (all channels get the same gains). Returns false when the text had no filter
// and no preamp at all.
inline bool ParseApoText(const std::string& text, int sampleRate, EqParams& eq, EqImportResult* result = nullptr)
{
  using namespace eqimport_detail;
  EqImportResult local;
  EqImportResult& res = result ? *result : local;
  res = EqImportResult{};
  std::vector<Filter> filters;
  double preamp = 0.0;

  size_t pos = 0;
  while (pos <= text.size())
  {
    size_t nl = text.find('\n', pos);
    if (nl == std::string::npos)
      nl = text.size();
    const std::string line = text.substr(pos, nl - pos);
    pos = nl + 1;
    const std::vector<std::string> t = Tokens(line);
    if (t.empty() || t[0][0] == '#' || t[0][0] == ';')
      continue;
    const std::string key = Upper(t[0]);
    if (key == "PREAMP")
    {
      double v = 0.0;
      if (t.size() >= 2 && Number(t[1], v))
      {
        preamp = v;
        res.preamp = true;
      }
      continue;
    }
    if (key != "FILTER")
    {
      if (key == "CHANNEL" || key == "COPY" || key == "EVAL" || key == "CONVOLUTION" || key == "INCLUDE")
        res.notes.push_back("строка «" + t[0] + "» не поддерживается и пропущена");
      continue;
    }
    // Filter [n]: ON|OFF TYPE key value ...
    size_t i = 1;
    if (i < t.size() && !t[i].empty() && std::isdigit(static_cast<unsigned char>(t[i][0])))
      ++i;
    if (i >= t.size())
      continue;
    const std::string state = Upper(t[i++]);
    if (state == "OFF")
    {
      ++res.skipped;
      continue;
    }
    if (state != "ON" || i >= t.size())
      continue;
    const std::string type = Upper(t[i++]);
    Filter f;
    if (type == "PK" || type == "PEQ") f.type = kBandBell;
    else if (type == "LS" || type == "LSC" || type == "LSQ") f.type = kBandLowShelf;
    else if (type == "HS" || type == "HSC" || type == "HSQ") f.type = kBandHighShelf;
    else if (type == "NO") f.type = kBandNotch;
    else if (type == "HP" || type == "HPQ" || type == "HPF") f.type = kBandHighPass;
    else if (type == "LP" || type == "LPQ" || type == "LPF") f.type = kBandLowPass;
    else
    {
      ++res.skipped;
      res.notes.push_back("тип фильтра «" + t[i - 1] + "» не поддерживается, фильтр пропущен");
      continue;
    }
    for (; i < t.size(); ++i)
    {
      const std::string k = Upper(t[i]);
      double v = 0.0;
      if (k == "FC" && i + 1 < t.size() && Number(t[i + 1], v)) { f.fc = v; ++i; }
      else if (k == "GAIN" && i + 1 < t.size() && Number(t[i + 1], v)) { f.gain = v; ++i; }
      else if (k == "Q" && i + 1 < t.size() && Number(t[i + 1], v)) { f.q = v; f.hasQ = true; ++i; }
      else if (k == "BW" && i + 2 < t.size() && Upper(t[i + 1]) == "OCT" && Number(t[i + 2], v)) { f.bw = v; f.hasBw = true; i += 2; }
    }
    filters.push_back(f);
  }

  if (filters.empty() && !res.preamp)
    return false;

  std::stable_sort(filters.begin(), filters.end(), [](const Filter& a, const Filter& b) { return a.fc < b.fc; });
  EqParams out;                       // factory layout: 30 flat bell bands
  out.enabled = eq.enabled;
  bool used[kBands] = {};
  for (const Filter& f : filters)
  {
    if (res.imported >= kBands)
    {
      ++res.skipped;
      continue;
    }
    // the free band whose factory frequency is closest (on a log scale)
    int best = -1;
    double bestDist = 1e18;
    for (int b = 0; b < kBands; ++b)
      if (!used[b])
      {
        const double d = std::fabs(std::log(f.fc / static_cast<double>(kBandHz[b])));
        if (d < bestDist)
        {
          bestDist = d;
          best = b;
        }
      }
    used[best] = true;
    BandShape& sh = out.shape[best];
    double fc = f.fc;
    if (fc < kBandFreqMinHz || fc > kBandFreqMaxHz)
    {
      fc = std::min<double>(kBandFreqMaxHz, std::max<double>(kBandFreqMinHz, fc));
      res.notes.push_back("частота фильтра вне 20 Гц…20 кГц, ограничена");
    }
    sh.freqHz = static_cast<float>(fc);
    sh.type = f.type;
    double width = DefaultBandWidthOct(f.type);
    if (f.hasBw)
      width = f.bw;
    else if (f.hasQ && f.q > 0.0)
      width = WidthFromQ(f.q, fc, sampleRate, f.type);
    if (width < kBandWidthMinOct || width > kBandWidthMaxOct)
    {
      width = std::min<double>(kBandWidthMaxOct, std::max<double>(kBandWidthMinOct, width));
      res.notes.push_back("ширина (Q) фильтра на " + std::to_string(static_cast<int>(fc)) + " Гц вне допустимого диапазона, ограничена");
    }
    sh.widthOct = static_cast<float>(width);
    if (BandTypeUsesGain(f.type))
    {
      double g = f.gain;
      if (g > kEqBandMaxDb || g < -kEqBandMaxDb)
      {
        g = std::min<double>(kEqBandMaxDb, std::max<double>(-kEqBandMaxDb, g));
        res.notes.push_back("усиление фильтра на " + std::to_string(static_cast<int>(fc)) + " Гц больше ±12 дБ, ограничено");
      }
      for (int c = 0; c < kMaxChannels; ++c)
        out.bandDb[c][best] = static_cast<float>(std::round(g * 10.0) / 10.0);
    }
    ++res.imported;
  }
  if (res.skipped > 0 && res.imported >= kBands)
    res.notes.push_back("фильтров больше 30, лишние пропущены");
  out.preampDb = static_cast<float>(std::min<double>(12.0, std::max<double>(kEqPreampMinDb, preamp)));
  eq = out;
  return true;
}

// Writes the curve of one channel (channel < 0: the front-left one, which is the linked curve).
inline std::string FormatApoText(const EqParams& eq, int channel, int sampleRate)
{
  struct Item { double fc; std::string line; };
  std::vector<Item> items;
  const int ch = channel < 0 ? 0 : std::min(channel, kMaxChannels - 1);
  for (int b = 0; b < kBands; ++b)
  {
    const BandShape& sh = eq.shape[b];
    const int type = NormalizeBandType(sh.type);
    const float gain = eq.bandDb[ch][b];
    if (BandTypeUsesGain(type) && std::fabs(gain) < 0.05f)
      continue;                       // a flat bell / shelf does nothing
    const char* code = type == kBandLowShelf ? "LSC" : type == kBandHighShelf ? "HSC" : type == kBandNotch ? "NO"
                       : type == kBandHighPass ? "HPQ" : type == kBandLowPass ? "LPQ" : "PK";
    const double q = GraphicEq::WidthToQ(static_cast<double>(sh.widthOct), static_cast<double>(sh.freqHz), sampleRate, type);
    char buf[160];
    if (BandTypeUsesGain(type))
      std::snprintf(buf, sizeof buf, "ON %s Fc %.2f Hz Gain %.1f dB Q %.3f", code, static_cast<double>(sh.freqHz),
                    static_cast<double>(gain), q);
    else
      std::snprintf(buf, sizeof buf, "ON %s Fc %.2f Hz Q %.3f", code, static_cast<double>(sh.freqHz), q);
    items.push_back({static_cast<double>(sh.freqHz), buf});
  }
  std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.fc < b.fc; });
  char head[64];
  std::snprintf(head, sizeof head, "Preamp: %.1f dB\n", static_cast<double>(eq.preampDb));
  std::string out = head;
  int n = 1;
  for (const Item& it : items)
    out += "Filter " + std::to_string(n++) + ": " + it.line + "\n";
  return out;
}

}  // namespace vac3dsp
