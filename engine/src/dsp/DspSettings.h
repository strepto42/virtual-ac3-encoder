// DspSettings.h — dsp-settings.json: reading, writing and live-reload polling of DspParams.
//
// The file is the contract between the engine and DSP Studio (and the user's text editor):
//
//   {
//     "version": 1,
//     "enabled": true,                  // master switch; false = exact bypass
//     "order": "eq_comp",               // or "comp_eq"
//     "eq": {
//       "enabled": true,
//       "preamp_db": -6.0,              // -24 .. +12
//       "bands_hz": [20, 25.4, ...],    // information only, never read
//       "bands_db": [0, 0, 3, ...],     // 30 values for all channels, OR up to 6 arrays of 30
//                                       //   (order of channels: L, R, C, LFE, Ls, Rs)
//       // optional, only written when a band was changed; shared by all channels:
//       "band_freq_hz": [20, 25.4, ...],      // centre of every band, 20 .. 20000
//       "band_width_oct": [0.3436, ...],      // width in octaves, 0.05 .. 4
//       "band_type": ["bell", "low_shelf", "high_shelf", "notch", "high_pass", "low_pass", ...]
//     },
//     "upmix": { "enabled": true, "center_width": 0.5, "center_focus": 0.5, "center_low_hz": 150,
//               "center_high_hz": 5000, "surround_level_db": -3, "surround_delay_ms": 10,
//               "lfe_enabled": false, "lfe_level_db": -6, "lfe_cut_hz": 100 },   // upmix_algo=matrix
//     "compressor": { "enabled": false, "threshold_db": -18, "ratio": 3, "attack_ms": 10,
//                     "release_ms": 120, "knee_db": 6, "makeup_db": 0, "lfe_in_detector": false }
//   }
//
// Missing keys keep their defaults (master switch default: off). Unknown keys are ignored. All values
// go through Sanitize(), so a typo can never produce an out-of-range filter. A UTF-8 BOM (Notepad
// writes one) is accepted. Plain header, no engine/Windows dependencies, so it is unit-tested on any OS.
#pragma once

#include "DspParams.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace vac3dsp {
namespace json {

struct Value
{
  enum class Type { Null, Bool, Number, String, Array, Object };
  Type type = Type::Null;
  bool boolean = false;
  double number = 0.0;
  std::string text;
  std::vector<Value> items;                            // Array
  std::vector<std::pair<std::string, Value>> members;  // Object

  const Value* Find(const char* key) const
  {
    if (type != Type::Object)
      return nullptr;
    for (const auto& m : members)
      if (m.first == key)
        return &m.second;
    return nullptr;
  }
};

// Small strict JSON parser (RFC 8259 subset: no surrogate-pair decoding in strings, depth <= 16).
class Parser
{
public:
  Parser(const std::string& text, std::string* error) : s_(text), err_(error) {}

  bool Parse(Value& out)
  {
    if (s_.size() >= 3 && static_cast<unsigned char>(s_[0]) == 0xEF && static_cast<unsigned char>(s_[1]) == 0xBB &&
        static_cast<unsigned char>(s_[2]) == 0xBF)
      pos_ = 3;
    if (!ParseValue(out, 0))
      return false;
    SkipWs();
    if (pos_ != s_.size())
      return Fail("unexpected text after the JSON value");
    return true;
  }

private:
  bool Fail(const char* msg)
  {
    if (err_)
    {
      int line = 1;
      for (size_t i = 0; i < pos_ && i < s_.size(); ++i)
        if (s_[i] == '\n')
          ++line;
      *err_ = std::string(msg) + " (line " + std::to_string(line) + ")";
    }
    return false;
  }

  void SkipWs()
  {
    while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\r' || s_[pos_] == '\n'))
      ++pos_;
  }

  bool ParseValue(Value& v, int depth)
  {
    if (depth > 16)
      return Fail("nesting too deep");
    SkipWs();
    if (pos_ >= s_.size())
      return Fail("unexpected end of file");
    const char c = s_[pos_];
    if (c == '{')
      return ParseObject(v, depth);
    if (c == '[')
      return ParseArray(v, depth);
    if (c == '"')
    {
      v.type = Value::Type::String;
      return ParseString(v.text);
    }
    if (c == 't' || c == 'f' || c == 'n')
      return ParseWord(v);
    return ParseNumber(v);
  }

  bool ParseObject(Value& v, int depth)
  {
    v.type = Value::Type::Object;
    ++pos_;  // {
    SkipWs();
    if (pos_ < s_.size() && s_[pos_] == '}')
    {
      ++pos_;
      return true;
    }
    for (;;)
    {
      SkipWs();
      if (pos_ >= s_.size() || s_[pos_] != '"')
        return Fail("expected a quoted key");
      std::string key;
      if (!ParseString(key))
        return false;
      SkipWs();
      if (pos_ >= s_.size() || s_[pos_] != ':')
        return Fail("expected ':' after a key");
      ++pos_;
      Value child;
      if (!ParseValue(child, depth + 1))
        return false;
      v.members.emplace_back(std::move(key), std::move(child));
      SkipWs();
      if (pos_ >= s_.size())
        return Fail("unexpected end of file in an object");
      if (s_[pos_] == ',')
      {
        ++pos_;
        continue;
      }
      if (s_[pos_] == '}')
      {
        ++pos_;
        return true;
      }
      return Fail("expected ',' or '}'");
    }
  }

  bool ParseArray(Value& v, int depth)
  {
    v.type = Value::Type::Array;
    ++pos_;  // [
    SkipWs();
    if (pos_ < s_.size() && s_[pos_] == ']')
    {
      ++pos_;
      return true;
    }
    for (;;)
    {
      Value child;
      if (!ParseValue(child, depth + 1))
        return false;
      v.items.push_back(std::move(child));
      SkipWs();
      if (pos_ >= s_.size())
        return Fail("unexpected end of file in an array");
      if (s_[pos_] == ',')
      {
        ++pos_;
        continue;
      }
      if (s_[pos_] == ']')
      {
        ++pos_;
        return true;
      }
      return Fail("expected ',' or ']'");
    }
  }

  bool ParseString(std::string& out)
  {
    ++pos_;  // opening quote
    out.clear();
    while (pos_ < s_.size())
    {
      const char c = s_[pos_++];
      if (c == '"')
        return true;
      if (c == '\\')
      {
        if (pos_ >= s_.size())
          break;
        const char e = s_[pos_++];
        switch (e)
        {
          case '"': out += '"'; break;
          case '\\': out += '\\'; break;
          case '/': out += '/'; break;
          case 'b': out += '\b'; break;
          case 'f': out += '\f'; break;
          case 'n': out += '\n'; break;
          case 'r': out += '\r'; break;
          case 't': out += '\t'; break;
          case 'u':
            if (pos_ + 4 > s_.size())
              return Fail("bad \\u escape");
            pos_ += 4;
            out += '?';
            break;
          default: return Fail("bad escape in a string");
        }
      }
      else
        out += c;
    }
    return Fail("unterminated string");
  }

  bool ParseWord(Value& v)
  {
    if (s_.compare(pos_, 4, "true") == 0)
    {
      v.type = Value::Type::Bool;
      v.boolean = true;
      pos_ += 4;
      return true;
    }
    if (s_.compare(pos_, 5, "false") == 0)
    {
      v.type = Value::Type::Bool;
      v.boolean = false;
      pos_ += 5;
      return true;
    }
    if (s_.compare(pos_, 4, "null") == 0)
    {
      v.type = Value::Type::Null;
      pos_ += 4;
      return true;
    }
    return Fail("unknown word");
  }

  bool ParseNumber(Value& v)
  {
    const size_t start = pos_;
    if (pos_ < s_.size() && (s_[pos_] == '-' || s_[pos_] == '+'))
      ++pos_;
    while (pos_ < s_.size() && ((s_[pos_] >= '0' && s_[pos_] <= '9') || s_[pos_] == '.' || s_[pos_] == 'e' ||
                                s_[pos_] == 'E' || s_[pos_] == '-' || s_[pos_] == '+'))
      ++pos_;
    if (pos_ == start)
      return Fail("expected a value");
    const std::string token = s_.substr(start, pos_ - start);
    char* end = nullptr;
    const double d = std::strtod(token.c_str(), &end);  // the engine never changes the C locale
    if (end == token.c_str() || *end != '\0')
      return Fail("bad number");
    v.type = Value::Type::Number;
    v.number = d;
    return true;
  }

  const std::string& s_;
  std::string* err_;
  size_t pos_ = 0;
};

}  // namespace json

namespace detail {

inline const char* BandTypeName(int t)
{
  switch (NormalizeBandType(t))
  {
    case kBandLowShelf: return "low_shelf";
    case kBandHighShelf: return "high_shelf";
    case kBandNotch: return "notch";
    case kBandHighPass: return "high_pass";
    case kBandLowPass: return "low_pass";
    default: return "bell";
  }
}

inline int BandTypeFromName(const std::string& s)
{
  for (int t = 0; t < kBandTypeCount; ++t)
    if (s == BandTypeName(t))
      return t;
  return static_cast<int>(kBandBell);
}

inline bool AsBool(const json::Value& v, bool fallback)
{
  if (v.type == json::Value::Type::Bool)
    return v.boolean;
  if (v.type == json::Value::Type::Number)
    return v.number != 0.0;
  return fallback;
}

inline float AsFloat(const json::Value& v, float fallback)
{
  return v.type == json::Value::Type::Number ? static_cast<float>(v.number) : fallback;
}

inline void ReadBool(const json::Value* obj, const char* key, int32_t& dst)
{
  if (const json::Value* v = obj ? obj->Find(key) : nullptr)
    dst = AsBool(*v, dst != 0) ? 1 : 0;
}

inline void ReadFloat(const json::Value* obj, const char* key, float& dst)
{
  if (const json::Value* v = obj ? obj->Find(key) : nullptr)
    dst = AsFloat(*v, dst);
}

inline void ReadBandRow(const json::Value& arr, float* row)
{
  for (int b = 0; b < kBands; ++b)
    row[b] = (b < static_cast<int>(arr.items.size())) ? AsFloat(arr.items[static_cast<size_t>(b)], 0.0f) : 0.0f;
}

}  // namespace detail

// Parses the text of dsp-settings.json. On failure returns false, sets *error and leaves `out` untouched.
inline bool ParseSettings(const std::string& text, DspParams& out, std::string* error = nullptr)
{
  json::Value root;
  json::Parser parser(text, error);
  if (!parser.Parse(root))
    return false;
  if (root.type != json::Value::Type::Object)
  {
    if (error)
      *error = "the file must contain a JSON object";
    return false;
  }

  DspParams p;  // defaults: master off
  detail::ReadBool(&root, "enabled", p.enabled);
  if (const json::Value* o = root.Find("order"))
  {
    if (o->type == json::Value::Type::String)
      p.order = (o->text == "comp_eq") ? 1 : 0;
    else if (o->type == json::Value::Type::Number)
      p.order = o->number == 1.0 ? 1 : 0;
  }

  if (const json::Value* eq = root.Find("eq"))
  {
    detail::ReadBool(eq, "enabled", p.eq.enabled);
    detail::ReadFloat(eq, "preamp_db", p.eq.preampDb);
    if (const json::Value* bands = eq->Find("bands_db"))
    {
      if (bands->type == json::Value::Type::Array && !bands->items.empty())
      {
        if (bands->items[0].type == json::Value::Type::Array)
        {
          for (int c = 0; c < kMaxChannels && c < static_cast<int>(bands->items.size()); ++c)
            if (bands->items[static_cast<size_t>(c)].type == json::Value::Type::Array)
              detail::ReadBandRow(bands->items[static_cast<size_t>(c)], p.eq.bandDb[c]);
        }
        else
        {
          detail::ReadBandRow(*bands, p.eq.bandDb[0]);
          for (int c = 1; c < kMaxChannels; ++c)
            for (int b = 0; b < kBands; ++b)
              p.eq.bandDb[c][b] = p.eq.bandDb[0][b];
        }
      }
    }
  }

  if (const json::Value* eq = root.Find("eq"))
  {
    // Band shapes (optional). Short arrays only change the first bands, anything else stays at the default.
    const json::Value* freq = eq->Find("band_freq_hz");
    const json::Value* width = eq->Find("band_width_oct");
    const json::Value* type = eq->Find("band_type");
    for (int b = 0; b < kBands; ++b)
    {
      const size_t i = static_cast<size_t>(b);
      BandShape& sh = p.eq.shape[b];
      if (type && type->type == json::Value::Type::Array && i < type->items.size())
      {
        const json::Value& v = type->items[i];
        if (v.type == json::Value::Type::String)
          sh.type = detail::BandTypeFromName(v.text);
        else if (v.type == json::Value::Type::Number)
          sh.type = NormalizeBandType(static_cast<int>(v.number));
      }
      sh.widthOct = DefaultBandWidthOct(sh.type);   // a typed band without its own width gets the type's default
      if (freq && freq->type == json::Value::Type::Array && i < freq->items.size())
        sh.freqHz = detail::AsFloat(freq->items[i], sh.freqHz);
      if (width && width->type == json::Value::Type::Array && i < width->items.size())
        sh.widthOct = detail::AsFloat(width->items[i], sh.widthOct);
    }
  }

  if (const json::Value* k = root.Find("compressor"))
  {
    detail::ReadBool(k, "enabled", p.comp.enabled);
    detail::ReadFloat(k, "threshold_db", p.comp.thresholdDb);
    detail::ReadFloat(k, "ratio", p.comp.ratio);
    detail::ReadFloat(k, "attack_ms", p.comp.attackMs);
    detail::ReadFloat(k, "release_ms", p.comp.releaseMs);
    detail::ReadFloat(k, "knee_db", p.comp.kneeDb);
    detail::ReadFloat(k, "makeup_db", p.comp.makeupDb);
    detail::ReadBool(k, "lfe_in_detector", p.comp.lfeInDetector);
  }

  if (const json::Value* u = root.Find("upmix"))
  {
    detail::ReadBool(u, "enabled", p.upmix.enabled);
    detail::ReadFloat(u, "center_width", p.upmix.centerWidth);
    detail::ReadFloat(u, "center_focus", p.upmix.centerFocus);
    detail::ReadFloat(u, "center_low_hz", p.upmix.centerLowHz);
    detail::ReadFloat(u, "center_high_hz", p.upmix.centerHighHz);
    detail::ReadFloat(u, "surround_level_db", p.upmix.surroundLevelDb);
    detail::ReadFloat(u, "surround_delay_ms", p.upmix.surroundDelayMs);
    detail::ReadBool(u, "lfe_enabled", p.upmix.lfeEnabled);
    detail::ReadFloat(u, "lfe_level_db", p.upmix.lfeLevelDb);
    detail::ReadFloat(u, "lfe_cut_hz", p.upmix.lfeCutHz);
  }

  Sanitize(p);
  out = p;
  return true;
}

namespace detail {
inline std::string Num(double v)
{
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.3f", v);
  std::string s = buf;
  while (!s.empty() && s.back() == '0')
    s.pop_back();
  if (!s.empty() && s.back() == '.')
    s.pop_back();
  if (s.empty() || s == "-0")
    s = "0";
  return s;
}

// The "upmix" section, formatted (shared by the settings file and by upmix presets).
inline std::string UpmixBlock(const UpmixParams& u)
{
  std::string s;
  s += "  \"upmix\": {\n";
  s += std::string("    \"enabled\": ") + (u.enabled ? "true" : "false") + ",\n";
  s += "    \"center_width\": " + Num(u.centerWidth) + ",\n";
  s += "    \"center_focus\": " + Num(u.centerFocus) + ",\n";
  s += "    \"center_low_hz\": " + Num(u.centerLowHz) + ",\n";
  s += "    \"center_high_hz\": " + Num(u.centerHighHz) + ",\n";
  s += "    \"surround_level_db\": " + Num(u.surroundLevelDb) + ",\n";
  s += "    \"surround_delay_ms\": " + Num(u.surroundDelayMs) + ",\n";
  s += std::string("    \"lfe_enabled\": ") + (u.lfeEnabled ? "true" : "false") + ",\n";
  s += "    \"lfe_level_db\": " + Num(u.lfeLevelDb) + ",\n";
  s += "    \"lfe_cut_hz\": " + Num(u.lfeCutHz) + "\n";
  s += "  }\n";
  return s;
}

inline std::string NumFixed(double v, int decimals)
{
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
  return buf;
}

inline std::string Row(const float* row)
{
  std::string s = "[";
  for (int b = 0; b < kBands; ++b)
  {
    if (b)
      s += ", ";
    s += Num(row[b]);
  }
  return s + "]";
}
}  // namespace detail

// Formats DspParams as the readable JSON above (flat bands_db when every channel has the same curve).
inline std::string FormatSettings(const DspParams& in)
{
  DspParams p = in;
  Sanitize(p);
  const auto b = [](int32_t v) { return v ? "true" : "false"; };
  bool linked = true;
  for (int c = 1; c < kMaxChannels && linked; ++c)
    for (int i = 0; i < kBands; ++i)
      if (p.eq.bandDb[c][i] != p.eq.bandDb[0][i])
        linked = false;

  std::string s = "{\n";
  s += "  \"version\": 1,\n";
  s += std::string("  \"enabled\": ") + b(p.enabled) + ",\n";
  s += std::string("  \"order\": \"") + (p.order == 1 ? "comp_eq" : "eq_comp") + "\",\n";
  s += "  \"eq\": {\n";
  s += std::string("    \"enabled\": ") + b(p.eq.enabled) + ",\n";
  s += "    \"preamp_db\": " + detail::Num(p.eq.preampDb) + ",\n";
  s += "    \"bands_hz\": [";
  for (int i = 0; i < kBands; ++i)
  {
    if (i)
      s += ", ";
    s += detail::Num(kBandHz[i]);
  }
  s += "],\n";
  // Band shapes: written only when something differs from the factory layout, so untouched files stay as they were.
  bool customFreq = false, customWidth = false, customType = false;
  for (int i = 0; i < kBands; ++i)
  {
    const BandShape& sh = p.eq.shape[i];
    customFreq = customFreq || std::fabs(sh.freqHz - kBandHz[i]) > 0.005f;
    customWidth = customWidth || std::fabs(sh.widthOct - static_cast<float>(kBandWidthOct)) > 0.0005f;
    customType = customType || sh.type != kBandBell;
  }
  if (customFreq || customWidth || customType)
  {
    if (customFreq)
    {
      s += "    \"band_freq_hz\": [";
      for (int i = 0; i < kBands; ++i)
        s += std::string(i ? ", " : "") + detail::NumFixed(p.eq.shape[i].freqHz, 2);
      s += "],\n";
    }
    if (customWidth)
    {
      s += "    \"band_width_oct\": [";
      for (int i = 0; i < kBands; ++i)
        s += std::string(i ? ", " : "") + detail::NumFixed(p.eq.shape[i].widthOct, 4);
      s += "],\n";
    }
    if (customType)
    {
      s += "    \"band_type\": [";
      for (int i = 0; i < kBands; ++i)
        s += std::string(i ? ", " : "") + "\"" + detail::BandTypeName(p.eq.shape[i].type) + "\"";
      s += "],\n";
    }
  }
  if (linked)
    s += "    \"bands_db\": " + detail::Row(p.eq.bandDb[0]) + "\n";
  else
  {
    s += "    \"bands_db\": [\n";
    for (int c = 0; c < kMaxChannels; ++c)
      s += "      " + detail::Row(p.eq.bandDb[c]) + (c + 1 < kMaxChannels ? ",\n" : "\n");
    s += "    ]\n";
  }
  s += "  },\n";
  s += "  \"compressor\": {\n";
  s += std::string("    \"enabled\": ") + b(p.comp.enabled) + ",\n";
  s += "    \"threshold_db\": " + detail::Num(p.comp.thresholdDb) + ",\n";
  s += "    \"ratio\": " + detail::Num(p.comp.ratio) + ",\n";
  s += "    \"attack_ms\": " + detail::Num(p.comp.attackMs) + ",\n";
  s += "    \"release_ms\": " + detail::Num(p.comp.releaseMs) + ",\n";
  s += "    \"knee_db\": " + detail::Num(p.comp.kneeDb) + ",\n";
  s += "    \"makeup_db\": " + detail::Num(p.comp.makeupDb) + ",\n";
  s += std::string("    \"lfe_in_detector\": ") + b(p.comp.lfeInDetector) + "\n";
  s += "  },\n";
  s += detail::UpmixBlock(p.upmix);
  s += "}\n";
  return s;
}

// Writes the file atomically (temporary file + rename), so the engine never reads half a file.
inline bool SaveSettings(const std::filesystem::path& path, const DspParams& p, std::string* error = nullptr)
{
  namespace fs = std::filesystem;
  fs::path tmp = path;
  tmp += ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f)
    {
      if (error)
        *error = "cannot write " + tmp.string();
      return false;
    }
    f << FormatSettings(p);
    if (!f)
    {
      if (error)
        *error = "write error in " + tmp.string();
      return false;
    }
  }
  std::error_code ec;
  fs::rename(tmp, path, ec);  // replaces an existing file on both POSIX and Windows (std::filesystem)
  if (ec)
  {
    fs::remove(tmp, ec);
    if (error)
      *error = "cannot replace " + path.string();
    return false;
  }
  return true;
}

// Polls the file (cheap: one stat per call) and reloads it when its time stamp or size changes.
class SettingsFile
{
public:
  enum class Result
  {
    Unchanged,  // nothing new
    Loaded,     // `out` holds the new parameters
    Failed,     // the file changed but is not valid; `out` untouched, *error says why (reported once)
    Missing     // the file does not exist (reported once after it was there, or on the first poll)
  };

  explicit SettingsFile(std::filesystem::path path) : path_(std::move(path)) {}
  std::string Path() const { return path_.string(); }

  Result Poll(DspParams& out, std::string* error = nullptr)
  {
    namespace fs = std::filesystem;
    std::error_code ec;
    const auto time = fs::last_write_time(path_, ec);
    if (ec)
    {
      if (state_ == State::Missing)
        return Result::Unchanged;
      state_ = State::Missing;
      return Result::Missing;
    }
    const auto size = fs::file_size(path_, ec);
    if (state_ == State::Seen && time == time_ && !ec && size == size_)
      return Result::Unchanged;

    std::ifstream f(path_, std::ios::binary);
    if (!f)
      return Result::Unchanged;  // locked for a moment: try again on the next poll
    std::ostringstream ss;
    ss << f.rdbuf();
    time_ = time;
    size_ = ec ? 0 : size;
    state_ = State::Seen;
    return ParseSettings(ss.str(), out, error) ? Result::Loaded : Result::Failed;
  }

private:
  enum class State { Unknown, Seen, Missing };
  std::filesystem::path path_;
  State state_ = State::Unknown;
  std::filesystem::file_time_type time_{};
  std::uintmax_t size_ = 0;
};

// Does this settings text have its own "upmix" section? (Profiles made before the upmix existed do not; applying
// such a profile must not reset the user's upmix settings to the defaults.)
inline bool HasUpmixSection(const std::string& text) { return text.find("\"upmix\"") != std::string::npos; }

// An upmix preset is a settings file that only has the "upmix" section: { "upmix": { ... } }.
inline std::string FormatUpmixPreset(const UpmixParams& u) { return "{\n" + detail::UpmixBlock(u) + "}\n"; }

// Reads the "upmix" section of a preset (or of a complete settings file); everything else is ignored.
inline bool ParseUpmixPreset(const std::string& text, UpmixParams& out, std::string* error = nullptr)
{
  DspParams p;
  if (!ParseSettings(text, p, error))
    return false;
  out = p.upmix;
  return true;
}

}  // namespace vac3dsp
