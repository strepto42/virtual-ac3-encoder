// AutoProfiles.h — rules "when this application is making sound, use that DSP profile / upmix preset".
//
//   auto-profiles.json (next to the exe, edited in DSP Studio, read by the engine):
//   {
//     "enabled": true,
//     "hold_seconds": 3,
//     "default": { "dsp": "", "upmix": "" },
//     "rules": [ { "process": "vlc.exe", "dsp": "Кино", "upmix": "Movie" }, ... ]
//   }
//
// The engine asks Windows which applications are playing sound (audio sessions of the capture endpoint) and
// feeds their exe names to AutoProfileSelector. The first rule (in file order) that matches any playing app
// wins. A change is only applied when the candidate stayed the same for `hold_seconds` (no flapping), and
// only on a CHANGE: a manual choice in DSP Studio is not overridden until the situation changes again.
// "dsp" / "upmix" are file names (without .json) in dsp-presets / upmix-presets; empty = leave as it is.
#pragma once

#include "DspSettings.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

namespace vac3dsp {

struct AutoRule
{
  std::string process;   // "vlc.exe" or "vlc"
  std::string dsp;       // profile name or empty
  std::string upmix;     // upmix preset name or empty
};

struct AutoProfileConfig
{
  bool enabled = false;
  double holdSeconds = 3.0;       // 1 .. 60
  std::string defaultDsp;         // used when something plays that no rule matches (empty: leave as it is)
  std::string defaultUpmix;
  std::vector<AutoRule> rules;

  bool HasDefault() const { return !defaultDsp.empty() || !defaultUpmix.empty(); }
};

// "C:\\Program Files\\VLC\\VLC.EXE" -> "vlc"
inline std::string ProcessStem(const std::string& name)
{
  size_t start = name.find_last_of("\\\\/");
  std::string s = start == std::string::npos ? name : name.substr(start + 1);
  for (char& c : s)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (s.size() > 4 && s.compare(s.size() - 4, 4, ".exe") == 0)
    s.resize(s.size() - 4);
  return s;
}

namespace autoprof_detail {
inline std::string Str(const json::Value* obj, const char* key)
{
  const json::Value* v = obj ? obj->Find(key) : nullptr;
  return v && v->type == json::Value::Type::String ? v->text : std::string();
}
inline std::string Escape(const std::string& s)
{
  std::string o;
  for (unsigned char c : s)
  {
    if (c == '"') o += "\\\"";
    else if (c == '\\') o += "\\\\";
    else if (c < 0x20)
    {
      char b[8];
      std::snprintf(b, sizeof b, "\\u%04x", c);
      o += b;
    }
    else o += static_cast<char>(c);
  }
  return o;
}
}  // namespace autoprof_detail

inline bool ParseAutoProfiles(const std::string& text, AutoProfileConfig& out, std::string* error = nullptr)
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
  AutoProfileConfig cfg;
  if (const json::Value* e = root.Find("enabled"))
    cfg.enabled = detail::AsBool(*e, false);
  if (const json::Value* h = root.Find("hold_seconds"))
    if (h->type == json::Value::Type::Number)
      cfg.holdSeconds = std::min(60.0, std::max(1.0, h->number));
  if (const json::Value* d = root.Find("default"))
  {
    cfg.defaultDsp = autoprof_detail::Str(d, "dsp");
    cfg.defaultUpmix = autoprof_detail::Str(d, "upmix");
  }
  if (const json::Value* r = root.Find("rules"))
    if (r->type == json::Value::Type::Array)
      for (const json::Value& item : r->items)
      {
        AutoRule rule;
        rule.process = autoprof_detail::Str(&item, "process");
        rule.dsp = autoprof_detail::Str(&item, "dsp");
        rule.upmix = autoprof_detail::Str(&item, "upmix");
        if (!rule.process.empty())
          cfg.rules.push_back(rule);
      }
  out = cfg;
  return true;
}

inline std::string FormatAutoProfiles(const AutoProfileConfig& cfg)
{
  using autoprof_detail::Escape;
  std::string s = "{\n";
  s += std::string("  \"enabled\": ") + (cfg.enabled ? "true" : "false") + ",\n";
  char buf[32];
  std::snprintf(buf, sizeof buf, "%g", cfg.holdSeconds);
  s += std::string("  \"hold_seconds\": ") + buf + ",\n";
  s += "  \"default\": { \"dsp\": \"" + Escape(cfg.defaultDsp) + "\", \"upmix\": \"" + Escape(cfg.defaultUpmix) + "\" },\n";
  s += "  \"rules\": [";
  for (size_t i = 0; i < cfg.rules.size(); ++i)
  {
    const AutoRule& r = cfg.rules[i];
    s += std::string(i ? ",\n    " : "\n    ") + "{ \"process\": \"" + Escape(r.process) + "\", \"dsp\": \"" + Escape(r.dsp) +
         "\", \"upmix\": \"" + Escape(r.upmix) + "\" }";
  }
  s += cfg.rules.empty() ? "]\n}\n" : "\n  ]\n}\n";
  return s;
}

// The settings to apply for a rule. A DSP profile replaces everything except the user's upmix tuning (unless the
// profile has an "upmix" section of its own); an upmix preset then replaces only the upmix values and keeps the
// on/off switch. Either part may be absent (have* == false): then that part stays as it is.
inline bool ComposeAutoSettings(const DspParams& current, bool haveDsp, const std::string& dspText, bool haveUpmix,
                                const std::string& upmixText, DspParams& out, std::string* error = nullptr)
{
  DspParams next = current;
  if (haveDsp)
  {
    DspParams p;
    if (!ParseSettings(dspText, p, error))
      return false;
    if (!HasUpmixSection(dspText))
      p.upmix = current.upmix;
    next = p;
  }
  if (haveUpmix)
  {
    UpmixParams u;
    if (!ParseUpmixPreset(upmixText, u, error))
      return false;
    const int enabled = next.upmix.enabled;
    next.upmix = u;
    next.upmix.enabled = enabled;
  }
  out = next;
  return true;
}

class AutoProfileSelector
{
public:
  static constexpr int kNone = -2;      // nothing applied yet
  static constexpr int kDefault = -1;   // the "default" entry

  void Reset()
  {
    current_ = kNone;
    pending_ = kNone;
    pendingSince_ = 0.0;
  }

  // `activeApps`: exe names (any case, paths allowed) of the applications that are playing sound right now.
  // Returns true when a profile has to be applied now; *ruleOut is the rule index or kDefault.
  bool Update(const AutoProfileConfig& cfg, const std::vector<std::string>& activeApps, double nowSec, int* ruleOut)
  {
    if (!cfg.enabled)
    {
      pending_ = kNone;
      return false;
    }
    int candidate = current_;                  // silence: keep what is on
    if (!activeApps.empty())
    {
      int match = kNone;
      for (size_t i = 0; i < cfg.rules.size() && match == kNone; ++i)
      {
        const std::string want = ProcessStem(cfg.rules[i].process);
        for (const std::string& app : activeApps)
          if (ProcessStem(app) == want)
          {
            match = static_cast<int>(i);
            break;
          }
      }
      if (match != kNone)
        candidate = match;
      else if (cfg.HasDefault())
        candidate = kDefault;                  // something plays, no rule fits: the default
    }
    if (candidate == current_)
    {
      pending_ = kNone;
      return false;
    }
    if (candidate != pending_)
    {
      pending_ = candidate;
      pendingSince_ = nowSec;
      return false;
    }
    if (nowSec - pendingSince_ < cfg.holdSeconds)
      return false;
    current_ = candidate;
    pending_ = kNone;
    if (ruleOut)
      *ruleOut = current_;
    return true;
  }

  int Current() const { return current_; }

private:
  int current_ = kNone;
  int pending_ = kNone;
  double pendingSince_ = 0.0;
};

}  // namespace vac3dsp
