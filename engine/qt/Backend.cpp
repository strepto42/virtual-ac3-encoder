#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "Backend.h"

#include <windows.h>
#include <TlHelp32.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

namespace cc {

namespace {
using Kv = std::map<std::string, std::string>;
const std::array<const char*, 6> kGainKeys{{"fl_db", "fr_db", "c_db", "lfe_db", "sl_db", "sr_db"}};
constexpr wchar_t kStartupLinkName[] = L"AC3 Control Center.lnk";

std::string TrimA(std::string s)
{
  const auto first = s.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return "";
  const auto last = s.find_last_not_of(" \t\r\n");
  return s.substr(first, last - first + 1);
}

Kv ReadKv(const std::wstring& path)
{
  Kv values;
  std::ifstream in{std::filesystem::path(path), std::ios::binary};
  std::string line;
  while (std::getline(in, line))
  {
    if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
        static_cast<unsigned char>(line[1]) == 0xBB && static_cast<unsigned char>(line[2]) == 0xBF)
      line.erase(0, 3);
    line = TrimA(line);
    if (line.empty() || line[0] == '#' || line[0] == ';') continue;
    const auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    values[TrimA(line.substr(0, eq))] = TrimA(line.substr(eq + 1));
  }
  return values;
}

bool WriteTextAtomic(const std::wstring& path, const std::string& content)
{
  const std::wstring temp = path + L".tmp";
  {
    std::ofstream out{std::filesystem::path(temp), std::ios::binary | std::ios::trunc};
    if (!out) return false;
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    out.flush();
    if (!out) return false;
  }
  if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
  {
    DeleteFileW(temp.c_str());
    return false;
  }
  return true;
}

bool WriteKvAtomic(const std::wstring& path, const Kv& values)
{
  std::ostringstream out;
  for (const auto& item : values) out << item.first << '=' << item.second << "\r\n";
  return WriteTextAtomic(path, out.str());
}

std::wstring FromUtf8(const std::string& s)
{
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  if (n <= 1) return {};
  std::wstring out(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
  out.resize(static_cast<size_t>(n - 1));
  return out;
}

std::wstring FullPath(const std::wstring& path)
{
  std::wstring full(32768, L'\0');
  const DWORD n = GetFullPathNameW(path.c_str(), static_cast<DWORD>(full.size()), full.data(), nullptr);
  if (n == 0 || n >= full.size()) return L"";
  full.resize(n);
  return full;
}

std::vector<Device> EnumerateDevices(EDataFlow flow)
{
  std::vector<Device> result;
  IMMDeviceEnumerator* enumerator = nullptr;
  HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
  if (FAILED(hr) || !enumerator) return result;
  IMMDeviceCollection* collection = nullptr;
  hr = enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection);
  if (SUCCEEDED(hr) && collection)
  {
    UINT count = 0;
    collection->GetCount(&count);
    for (UINT i = 0; i < count; ++i)
    {
      IMMDevice* device = nullptr;
      if (FAILED(collection->Item(i, &device)) || !device) continue;
      LPWSTR id = nullptr;
      IPropertyStore* store = nullptr;
      std::wstring name = L"(unnamed audio device)";
      std::wstring deviceId;
      if (SUCCEEDED(device->GetId(&id)) && id) deviceId = id;
      if (id) CoTaskMemFree(id);
      if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store)) && store)
      {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal)
          name = pv.pwszVal;
        PropVariantClear(&pv);
        store->Release();
      }
      if (!deviceId.empty()) result.push_back({deviceId, name});
      device->Release();
    }
    collection->Release();
  }
  enumerator->Release();
  return result;
}
} // namespace

std::string ToUtf8(const std::wstring& value)
{
  if (value.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) return {};
  std::string out(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, out.data(), n, nullptr, nullptr);
  out.resize(static_cast<size_t>(n - 1));
  return out;
}

struct Backend::Impl
{
  std::wstring exeDir;
  std::wstring configPath;
  std::wstring controlPath;
  std::wstring logPath;
  std::wstring uiIniPath;
  std::vector<Device> capture;
  std::vector<Device> render;
  HANDLE engine = nullptr;
  FILETIME iniWriteTime{};
  bool comOwned = false;
};

Backend::Backend() : d_(new Impl)
{
  const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  d_->comOwned = SUCCEEDED(hr);  // S_FALSE also needs a matching CoUninitialize

  std::wstring path(32768, L'\0');
  const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  path.resize(n);
  const size_t slash = path.find_last_of(L"\\/");
  d_->exeDir = slash == std::wstring::npos ? L"." : path.substr(0, slash);
  d_->configPath = d_->exeDir + L"\\virtual-ac3-encoder.conf";
  d_->controlPath = d_->exeDir + L"\\ac3-control.ini";
  d_->logPath = d_->exeDir + L"\\engine.log";
  d_->uiIniPath = d_->exeDir + L"\\control-center.ini";
}

Backend::~Backend()
{
  DetachEngine();
  if (d_->comOwned) CoUninitialize();
}

const std::wstring& Backend::exeDir() const { return d_->exeDir; }

// ------------------------------------------------------------------------------ devices
void Backend::RefreshDevices()
{
  d_->capture = EnumerateDevices(eCapture);
  d_->render = EnumerateDevices(eRender);
}
const std::vector<Device>& Backend::captureDevices() const { return d_->capture; }
const std::vector<Device>& Backend::renderDevices() const { return d_->render; }

int Backend::PreferredOutputIndex() const
{
  for (size_t i = 0; i < d_->render.size(); ++i)
  {
    std::wstring name = d_->render[i].name;
    std::transform(name.begin(), name.end(), name.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    if (name.find(L"realtek digital") != std::wstring::npos || name.find(L"digital output") != std::wstring::npos ||
        name.find(L"spdif") != std::wstring::npos || name.find(L"s/pdif") != std::wstring::npos)
      return static_cast<int>(i);
  }
  return d_->render.empty() ? -1 : 0;
}

int Backend::FindById(const std::vector<Device>& devices, const std::wstring& id)
{
  if (id.empty()) return -1;
  for (size_t i = 0; i < devices.size(); ++i)
    if (devices[i].id == id) return static_cast<int>(i);
  return -1;
}

int Backend::FindBySubstring(const std::vector<Device>& devices, const std::wstring& text)
{
  if (text.empty()) return -1;
  for (size_t i = 0; i < devices.size(); ++i)
    if (devices[i].name.find(text) != std::wstring::npos) return static_cast<int>(i);
  return -1;
}

// ------------------------------------------------------------------------------ configuration
Settings Backend::LoadSettings() const
{
  Kv cfg = ReadKv(d_->configPath);
  Settings s;
  const std::string& outputMode = cfg["output_mode"];
  const bool loopback = cfg["loopback"] == "1";
  if (outputMode == "pcm") s.mode = 2;
  else if (outputMode == "dts") s.mode = loopback ? 4 : 3;
  else if (loopback) s.mode = cfg["dts_in"] == "1" ? 6 : 5;
  else s.mode = cfg["upmix"] == "off" ? 0 : 1;
  if (cfg.empty()) s.mode = 5;
  s.inputId = FromUtf8(cfg["in_id"]);
  s.outputId = FromUtf8(cfg["out_id"]);
  s.stereoUpmix = cfg["loopback_upmix"] == "always" ? 1 : cfg["loopback_upmix"] == "off" ? 2 : 0;
  s.delaySel = cfg["upmix_win_size"] == "2048" ? 1 : cfg["upmix_win_size"] == "1024" ? 2 : 0;
  s.matrixUpmix = cfg["upmix_algo"] == "matrix";
  return s;
}

bool Backend::SaveSettings(const Settings& s, std::wstring* error)
{
  auto fail = [&](const wchar_t* text) {
    if (error) *error = text;
    return false;
  };
  if (s.mode < 0 || s.mode >= kModeCount) return fail(L"Выбери режим кодирования.");
  if (s.inputId.empty()) return fail(L"Выбери входное устройство для выбранного режима.");
  if (s.outputId.empty()) return fail(L"Выбери цифровое устройство вывода, подключённое к ресиверу.");

  const bool loopback = IsLoopbackMode(s.mode);
  Kv cfg = ReadKv(d_->configPath);
  if (cfg.empty())
  {
    cfg["bitrate"] = "640000";
    cfg["safe"] = "1536";
  }
  cfg["in_id"] = ToUtf8(s.inputId);
  cfg["out_id"] = ToUtf8(s.outputId);
  cfg.erase("in");
  cfg.erase("out");
  cfg.erase("out_spdif");
  cfg["loopback"] = loopback ? "1" : "0";
  cfg["bitrate"] = "640000";  // Dolby Digital over S/PDIF; the DTS encoder is fixed at 768 kbps
  if (!cfg.count("headroom_db")) cfg["headroom_db"] = "-6";
  if (!cfg.count("limiter")) cfg["limiter"] = "1";
  if (!cfg.count("limiter_ceiling_db")) cfg["limiter_ceiling_db"] = "-1";

  const char* loopbackUpmix = s.stereoUpmix == 1 ? "always" : s.stereoUpmix == 2 ? "off" : "auto";
  cfg["loopback_upmix"] = loopbackUpmix;
  cfg["upmix_win_size"] = s.delaySel == 1 ? "2048" : s.delaySel == 2 ? "1024" : "4096";
  cfg["dts_in"] = s.mode == 6 ? "1" : "0";

  switch (s.mode)
  {
  case 2: cfg["output_mode"] = "pcm"; cfg["upmix"] = "off"; break;
  case 3: cfg["output_mode"] = "dts"; cfg["upmix"] = "auto"; break;
  case 4: cfg["output_mode"] = "dts"; cfg["upmix"] = loopbackUpmix; break;
  case 5:
  case 6: cfg["output_mode"] = "ac3"; cfg["upmix"] = loopbackUpmix; break;
  default: cfg["output_mode"] = "ac3"; cfg["upmix"] = s.mode == 0 ? "off" : "auto"; break;
  }

  if (!WriteKvAtomic(d_->configPath, cfg))
    return fail(L"Не удалось записать virtual-ac3-encoder.conf. Проверь права доступа к папке.");
  if (!WriteControl(false))
    return fail(L"Не удалось записать ac3-control.ini.");
  return true;
}

// ------------------------------------------------------------------------------ gains
bool Backend::WriteControl(bool stop)
{
  std::ostringstream out;
  out << "stop=" << (stop ? 1 : 0) << "\r\n";
  for (size_t i = 0; i < gainDb_.size(); ++i) out << kGainKeys[i] << '=' << gainDb_[i] << "\r\n";
  return WriteTextAtomic(d_->controlPath, out.str());
}

void Backend::LoadGains()
{
  const Kv values = ReadKv(d_->controlPath);
  for (size_t i = 0; i < kGainKeys.size(); ++i)
  {
    const auto it = values.find(kGainKeys[i]);
    if (it == values.end()) continue;
    try { gainDb_[i] = std::clamp(std::stoi(it->second), -60, 0); } catch (...) {}
  }
}

// ------------------------------------------------------------------------------ engine
bool Backend::EngineRunning() const
{
  if (!d_->engine) return false;
  DWORD code = 0;
  if (!GetExitCodeProcess(d_->engine, &code)) return false;
  return code == STILL_ACTIVE;
}

void Backend::DetachEngine()
{
  if (d_->engine) CloseHandle(d_->engine);
  d_->engine = nullptr;
}

bool Backend::PollExit(unsigned long* exitCode)
{
  if (!d_->engine) return false;
  DWORD code = 0;
  if (GetExitCodeProcess(d_->engine, &code) && code != STILL_ACTIVE)
  {
    DetachEngine();
    if (exitCode) *exitCode = code;
    return true;
  }
  return false;
}

namespace {
// engine.exe processes: `sameFolder` = those started from `expectedFullPath`, else all others.
std::vector<std::pair<unsigned long, HANDLE>> FindEngines(const std::wstring& expectedFullPath, bool sameFolder,
                                                          DWORD access)
{
  std::vector<std::pair<unsigned long, HANDLE>> found;
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return found;
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  if (Process32FirstW(snapshot, &entry))
  {
    do
    {
      if (CompareStringOrdinal(entry.szExeFile, -1, L"engine.exe", -1, TRUE) != CSTR_EQUAL) continue;
      HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE | access, FALSE, entry.th32ProcessID);
      if (!process) continue;
      std::wstring actual(32768, L'\0');
      DWORD len = static_cast<DWORD>(actual.size());
      bool keep = false;
      if (QueryFullProcessImageNameW(process, 0, actual.data(), &len))
      {
        actual.resize(len);
        const bool same = CompareStringOrdinal(actual.c_str(), -1, expectedFullPath.c_str(), -1, TRUE) == CSTR_EQUAL;
        keep = (same == sameFolder) && WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
      }
      if (keep) found.emplace_back(entry.th32ProcessID, process);
      else CloseHandle(process);
    } while (Process32NextW(snapshot, &entry));
  }
  CloseHandle(snapshot);
  return found;
}
} // namespace

std::vector<unsigned long> Backend::ForeignEngines() const
{
  const std::wstring expected = FullPath(d_->exeDir + L"\\engine.exe");
  std::vector<unsigned long> pids;
  if (expected.empty()) return pids;
  for (auto& item : FindEngines(expected, false, 0))
  {
    pids.push_back(item.first);
    CloseHandle(item.second);
  }
  return pids;
}

void Backend::TerminateProcesses(const std::vector<unsigned long>& pids)
{
  for (unsigned long pid : pids)
  {
    HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
    if (!process) continue;
    TerminateProcess(process, 4);
    WaitForSingleObject(process, 2000);
    CloseHandle(process);
  }
}

StartResult Backend::StartEngine(const Settings& settings, std::wstring* message)
{
  auto say = [&](const std::wstring& text) { if (message) *message = text; };
  if (EngineRunning())
  {
    say(L"Кодировщик уже работает.");
    return StartResult::AlreadyRunning;
  }
  DetachEngine();

  const std::wstring enginePath = d_->exeDir + L"\\engine.exe";
  const std::wstring expected = FullPath(enginePath);
  if (!expected.empty())
  {
    auto same = FindEngines(expected, true, SYNCHRONIZE | PROCESS_TERMINATE);
    if (!same.empty())
    {
      d_->engine = same.front().second;  // keep the first handle, close the others
      for (size_t i = 1; i < same.size(); ++i) CloseHandle(same[i].second);
      say(L"Подключено к уже работающему кодировщику.");
      return StartResult::Attached;
    }
    if (!ForeignEngines().empty())
    {
      say(L"Работает engine.exe из другой папки (старая сборка) и занимает цифровой выход.");
      return StartResult::ForeignRunning;
    }
  }

  std::wstring error;
  if (!SaveSettings(settings, &error))
  {
    say(error);
    return StartResult::Failed;
  }
  const DWORD attrs = GetFileAttributesW(enginePath.c_str());
  if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY))
  {
    say(L"Не найден engine.exe рядом с панелью. Сначала собери проект (build.ps1).");
    return StartResult::Failed;
  }
  std::wstring command = L"\"" + enginePath + L"\" --hidden --log \"" + d_->logPath + L"\"";
  std::vector<wchar_t> mutableCommand(command.begin(), command.end());
  mutableCommand.push_back(L'\0');
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(enginePath.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                      d_->exeDir.c_str(), &si, &pi))
  {
    say(L"Не удалось запустить engine.exe. Код Windows: " + std::to_wstring(GetLastError()));
    return StartResult::Failed;
  }
  CloseHandle(pi.hThread);
  d_->engine = pi.hProcess;
  say(L"Запуск кодировщика… подробности: engine.log");
  return StartResult::Started;
}

void Backend::StopEngine(std::wstring* message)
{
  auto say = [&](const std::wstring& text) { if (message) *message = text; };
  if (!d_->engine)
  {
    const std::wstring expected = FullPath(d_->exeDir + L"\\engine.exe");
    auto same = expected.empty() ? decltype(FindEngines(expected, true, 0)){} : FindEngines(expected, true, SYNCHRONIZE | PROCESS_TERMINATE);
    if (same.empty())
    {
      WriteControl(false);
      say(L"Работающий кодировщик не найден.");
      return;
    }
    d_->engine = same.front().second;
    for (size_t i = 1; i < same.size(); ++i) CloseHandle(same[i].second);
  }
  if (EngineRunning())
  {
    WriteControl(true);
    if (WaitForSingleObject(d_->engine, 3000) == WAIT_TIMEOUT)
    {
      TerminateProcess(d_->engine, 3);
      WaitForSingleObject(d_->engine, 1500);
      say(L"Движок не ответил на штатную остановку; процесс завершён принудительно.");
      DetachEngine();
      WriteControl(false);
      return;
    }
  }
  DetachEngine();
  WriteControl(false);
  say(L"Кодировщик остановлен.");
}

// ------------------------------------------------------------------------------ autostart
namespace {
std::wstring StartupFolder()
{
  wchar_t folder[MAX_PATH] = {};
  if (FAILED(SHGetFolderPathW(nullptr, CSIDL_STARTUP, nullptr, SHGFP_TYPE_CURRENT, folder))) return L"";
  return folder;
}
std::wstring StartupLinkPath()
{
  const std::wstring folder = StartupFolder();
  return folder.empty() ? L"" : folder + L"\\" + kStartupLinkName;
}
std::wstring ShortcutTarget(const std::wstring& linkPath)
{
  std::wstring target;
  IShellLinkW* shell = nullptr;
  if (FAILED(CoCreateInstance(__uuidof(ShellLink), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shell))) || !shell)
    return target;
  IPersistFile* file = nullptr;
  if (SUCCEEDED(shell->QueryInterface(IID_PPV_ARGS(&file))) && file)
  {
    if (SUCCEEDED(file->Load(linkPath.c_str(), STGM_READ)))
    {
      wchar_t path[MAX_PATH * 2] = {};
      if (SUCCEEDED(shell->GetPath(path, static_cast<int>(std::size(path)), nullptr, SLGP_RAWPATH))) target = path;
    }
    file->Release();
  }
  shell->Release();
  return target;
}
} // namespace

bool Backend::AutostartEnabled() const
{
  const std::wstring link = StartupLinkPath();
  return !link.empty() && GetFileAttributesW(link.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool Backend::SetAutostart(bool enable)
{
  const std::wstring link = StartupLinkPath();
  if (link.empty()) return false;
  if (!enable) return DeleteFileW(link.c_str()) != FALSE || GetLastError() == ERROR_FILE_NOT_FOUND;

  std::wstring exePath(32768, L'\0');
  const DWORD n = GetModuleFileNameW(nullptr, exePath.data(), static_cast<DWORD>(exePath.size()));
  if (n == 0 || n >= exePath.size()) return false;
  exePath.resize(n);

  IShellLinkW* shell = nullptr;
  if (FAILED(CoCreateInstance(__uuidof(ShellLink), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shell))) || !shell)
    return false;
  shell->SetPath(exePath.c_str());
  shell->SetArguments(L"--minimized");
  shell->SetWorkingDirectory(d_->exeDir.c_str());
  shell->SetDescription(L"AC3 / DTS Control Center (стартует свёрнутым в трей)");
  HRESULT hr = E_FAIL;
  IPersistFile* file = nullptr;
  if (SUCCEEDED(shell->QueryInterface(IID_PPV_ARGS(&file))) && file)
  {
    hr = file->Save(link.c_str(), TRUE);
    file->Release();
  }
  shell->Release();
  return SUCCEEDED(hr);
}

std::vector<std::wstring> Backend::OtherPanelShortcuts() const
{
  std::vector<std::wstring> found;
  const std::wstring folder = StartupFolder();
  const std::wstring ourLink = StartupLinkPath();
  if (folder.empty()) return found;
  WIN32_FIND_DATAW data{};
  HANDLE search = FindFirstFileW((folder + L"\\*.lnk").c_str(), &data);
  if (search == INVALID_HANDLE_VALUE) return found;
  static const wchar_t* const kSuffixes[] = {L"control_center.exe", L"controlcenter_a_mixer.exe",
                                             L"controlcenter_b_modes.exe"};
  do
  {
    const std::wstring full = folder + L"\\" + data.cFileName;
    if (CompareStringOrdinal(full.c_str(), -1, ourLink.c_str(), -1, TRUE) == CSTR_EQUAL) continue;
    std::wstring target = ShortcutTarget(full);
    std::transform(target.begin(), target.end(), target.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    for (const wchar_t* suffix : kSuffixes)
    {
      const std::wstring s = suffix;
      if (target.size() >= s.size() && target.compare(target.size() - s.size(), s.size(), s) == 0)
      {
        found.push_back(full);
        break;
      }
    }
  } while (FindNextFileW(search, &data));
  FindClose(search);
  return found;
}

bool Backend::RemoveFile(const std::wstring& path) { return DeleteFileW(path.c_str()) != FALSE; }

// ------------------------------------------------------------------------------ control-center.ini
void Backend::EnsureUiIni()
{
  if (GetFileAttributesW(d_->uiIniPath.c_str()) != INVALID_FILE_ATTRIBUTES) return;
  HANDLE file = CreateFileW(d_->uiIniPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return;
  // UTF-16 LE with BOM: the Windows INI API reads (and later writes) the Cyrillic comments correctly.
  static const wchar_t text[] =
      L"\xFEFF; AC3 / DTS Control Center: настройки интерфейса.\r\n"
      L"; Изменения подхватываются без перезапуска панели (раз в пару секунд).\r\n"
      L"[ui]\r\n"
      L"; 1 = показывать выпадающее меню \"Задержка апмикса\", 0 = скрыть.\r\n"
      L"; Скрытое меню ничего не сбрасывает: выбранное значение остаётся в virtual-ac3-encoder.conf (upmix_win_size).\r\n"
      L"show_upmix_latency_menu=1\r\n"
      L"; Тема: dark или light (переключается и кнопкой в окне).\r\n"
      L"theme=dark\r\n";
  DWORD written = 0;
  WriteFile(file, text, static_cast<DWORD>((sizeof(text) / sizeof(wchar_t) - 1) * sizeof(wchar_t)), &written, nullptr);
  CloseHandle(file);
}

bool Backend::UiIniChanged()
{
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (!GetFileAttributesExW(d_->uiIniPath.c_str(), GetFileExInfoStandard, &data)) return false;
  if (CompareFileTime(&data.ftLastWriteTime, &d_->iniWriteTime) == 0) return false;
  d_->iniWriteTime = data.ftLastWriteTime;
  return true;
}

int Backend::UiIniInt(const wchar_t* key, int fallback) const
{
  return static_cast<int>(GetPrivateProfileIntW(L"ui", key, fallback, d_->uiIniPath.c_str()));
}

std::wstring Backend::UiIniString(const wchar_t* key, const wchar_t* fallback) const
{
  wchar_t buffer[128] = {};
  GetPrivateProfileStringW(L"ui", key, fallback, buffer, static_cast<DWORD>(std::size(buffer)), d_->uiIniPath.c_str());
  return buffer;
}

void Backend::UiIniSet(const wchar_t* key, const wchar_t* value)
{
  WritePrivateProfileStringW(L"ui", key, value, d_->uiIniPath.c_str());
}

} // namespace cc
