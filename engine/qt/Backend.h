// Backend.h — everything the control panel does that is NOT user interface: audio endpoint
// enumeration, reading/writing virtual-ac3-encoder.conf and ac3-control.ini, starting/stopping/
// attaching to engine.exe, the Startup-folder shortcut and control-center.ini.
// Windows-only, but no Windows or Qt headers leak through this file (pimpl).
#pragma once

#include <array>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace cc {

struct Device
{
  std::wstring id;
  std::wstring name;
};

constexpr int kModeCount = 7;

// What the user chooses in the panel (the rest of the engine config is preserved as it is).
struct Settings
{
  int mode = 5;                  // 0..6, see PanelWindow's mode table
  std::wstring inputId;
  std::wstring outputId;
  int stereoUpmix = 0;           // PCM 5.1 modes: 0 = auto, 1 = always, 2 = off
  int delaySel = 0;              // upmix FFT window: 0 = 4096, 1 = 2048, 2 = 1024
  bool matrixUpmix = false;      // upmix_algo=matrix: the FFT window above does not apply (read only, never written back)
};

enum class StartResult
{
  Started,
  AlreadyRunning,
  Attached,        // an engine.exe from this folder was already running; the panel took it over
  ForeignRunning,  // an engine.exe from ANOTHER folder holds the output; see ForeignEngines()
  Failed,
};

std::string ToUtf8(const std::wstring& value);

class Backend
{
public:
  Backend();
  ~Backend();
  Backend(const Backend&) = delete;
  Backend& operator=(const Backend&) = delete;

  const std::wstring& exeDir() const;
  static bool IsLoopbackMode(int mode) { return mode == 4 || mode == 5 || mode == 6; }

  // ---- audio endpoints -------------------------------------------------------------------
  void RefreshDevices();
  const std::vector<Device>& captureDevices() const;
  const std::vector<Device>& renderDevices() const;
  int PreferredOutputIndex() const;  // S/PDIF-looking render device, else 0, else -1
  static int FindById(const std::vector<Device>& devices, const std::wstring& id);
  static int FindBySubstring(const std::vector<Device>& devices, const std::wstring& text);

  // ---- configuration ---------------------------------------------------------------------
  Settings LoadSettings() const;
  bool SaveSettings(const Settings& settings, std::wstring* error);

  // ---- per-channel gains (ac3-control.ini, applied live by the engine) -------------------------
  std::array<int, 6>& gains() { return gainDb_; }
  void LoadGains();
  bool WriteControl(bool stop);

  // ---- engine process ----------------------------------------------------------------------
  bool EngineRunning() const;
  StartResult StartEngine(const Settings& settings, std::wstring* message);
  std::vector<unsigned long> ForeignEngines() const;
  void TerminateProcesses(const std::vector<unsigned long>& pids);
  void StopEngine(std::wstring* message);
  void DetachEngine();                       // forget the process; the engine keeps running
  bool PollExit(unsigned long* exitCode);    // true once, when the tracked engine has exited

  // ---- autostart of the panel (Startup-folder shortcut with --minimized) -------------------------
  bool AutostartEnabled() const;
  bool SetAutostart(bool enable);
  std::vector<std::wstring> OtherPanelShortcuts() const;
  static bool RemoveFile(const std::wstring& path);

  // ---- control-center.ini (interface options, polled by the panel) ---------------------------
  void EnsureUiIni();
  bool UiIniChanged();                       // true when the file changed since the last call
  int UiIniInt(const wchar_t* key, int fallback) const;
  std::wstring UiIniString(const wchar_t* key, const wchar_t* fallback) const;
  void UiIniSet(const wchar_t* key, const wchar_t* value);

private:
  struct Impl;
  std::unique_ptr<Impl> d_;
  std::array<int, 6> gainDb_{{0, 0, 0, 0, 0, 0}};
};

} // namespace cc
