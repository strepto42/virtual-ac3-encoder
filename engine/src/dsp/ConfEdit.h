// ConfEdit.h — read / change one `key=value` line of virtual-ac3-encoder.conf as text, leaving everything
// else (comments, order, line endings) alone. DSP Studio uses it to switch `upmix_algo`.
#pragma once

#include <string>

namespace vac3dsp {

namespace confdetail {
inline std::string Trim(const std::string& s)
{
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r'))
    ++a;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r'))
    --b;
  return s.substr(a, b - a);
}

// Is this line an active `key=...` line for `key`? On success *value gets the trimmed value.
inline bool IsKeyLine(const std::string& line, const std::string& key, std::string* value)
{
  const std::string t = Trim(line);
  if (t.empty() || t[0] == '#' || t[0] == ';')
    return false;
  const size_t eq = t.find('=');
  if (eq == std::string::npos || Trim(t.substr(0, eq)) != key)
    return false;
  if (value)
    *value = Trim(t.substr(eq + 1));
  return true;
}
}  // namespace confdetail

// Value of the LAST active `key=` line (the parser lets the last one win), or `fallback`.
inline std::string ConfGet(const std::string& text, const std::string& key, const std::string& fallback = std::string())
{
  std::string result = fallback;
  size_t pos = 0;
  while (pos <= text.size())
  {
    size_t nl = text.find('\n', pos);
    if (nl == std::string::npos)
      nl = text.size();
    std::string v;
    if (confdetail::IsKeyLine(text.substr(pos, nl - pos), key, &v))
      result = v;
    pos = nl + 1;
  }
  return result;
}

// Sets `key=value`: every active line of that key is rewritten, or one is appended when there is none.
inline std::string ConfSet(const std::string& text, const std::string& key, const std::string& value)
{
  const std::string eol = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
  std::string out;
  bool found = false;
  size_t pos = 0;
  while (pos < text.size())
  {
    size_t nl = text.find('\n', pos);
    const bool hasNl = nl != std::string::npos;
    if (!hasNl)
      nl = text.size();
    std::string line = text.substr(pos, nl - pos);
    const bool cr = !line.empty() && line.back() == '\r';
    if (confdetail::IsKeyLine(line, key, nullptr))
    {
      found = true;
      line = key + "=" + value + (cr ? "\r" : "");
    }
    out += line;
    if (hasNl)
      out += '\n';
    pos = nl + 1;
  }
  if (!found)
  {
    if (!out.empty() && out.back() != '\n')
      out += eol;
    out += key + "=" + value + eol;
  }
  return out;
}

}  // namespace vac3dsp
