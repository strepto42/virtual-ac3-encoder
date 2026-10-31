// ControlCenter.cpp — Modern Dark UI with Dear ImGui (DirectX 11) for Virtual AC3/DTS Encoder.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"
#include "MeterSharedState.h"

// Forward declare message handler from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

static const GUID kGuidSubtypeDolbyDigital = KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_DIGITAL;
static const GUID kGuidSubtypeDts = KSDATAFORMAT_SUBTYPE_IEC61937_DTS;

constexpr UINT WM_TRAYICON = WM_USER + 101;
constexpr UINT ID_TRAY_RESTORE = 2001;
constexpr UINT ID_TRAY_TOGGLE = 2002;
constexpr UINT ID_TRAY_EXIT = 2003;

struct AudioDevice {
    std::wstring name;
    std::wstring id;
    bool isSpdif = false;
    bool supportsAc3 = false;
    bool supportsDts = false;
    bool supportsPcm = true;
};
using Kv = std::map<std::string, std::string>;

// DirectX 11 global state
ID3D11Device*           g_pd3dDevice = nullptr;
ID3D11DeviceContext*    g_pd3dDeviceContext = nullptr;
IDXGISwapChain*         g_pSwapChain = nullptr;
ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;
HWND                    g_hWnd = nullptr;
NOTIFYICONDATAW         g_notifyIconData{};

// Process & File paths
PROCESS_INFORMATION g_engine{};
bool g_engineEnabled = true;
std::wstring g_exeDir;
std::wstring g_configPath;
std::wstring g_controlPath;
std::wstring g_logPath;

// Audio devices & settings state
std::vector<AudioDevice> g_captureDevices;
std::vector<AudioDevice> g_renderDevices;
int g_selectedInDevice = 0;
int g_selectedOutDevice = 0;
bool g_showAllRenderDevices = false;

// Settings
int g_outputMode = 0; // 0=DTS 5.1, 1=AC3 5.1, 2=PCM Stereo
int g_dtsBitrate = 0; // 0=Full 1536k, 1=Half 768k
int g_ac3Bitrate = 0; // 0=640k, 1=448k
int g_latencyPreset = 2; // 0=Extreme, 1=Fast, 2=Balanced, 3=Safe, 4=Relaxed
int g_upmixMode = 0; // 0=Dynamic Surround (Auto), 1=Always Surround, 2=Off
bool g_loopback = true;
bool g_outAutoSpdif = true;
int  g_reloadId = 1;

std::array<int, 6> g_gainDb{{0, 0, 0, 0, 0, 0}};
std::array<bool, 6> g_channelMute{{false, false, false, false, false, false}};

// Telemetry & meters
MeterSharedMemory g_meterMem;
std::array<float, 6> g_smoothedPeaks{{0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}};
std::array<float, 6> g_peakHold{{0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}};
std::array<int, 6> g_peakHoldFrames{{0, 0, 0, 0, 0, 0}};

const std::array<const char*, 6> kChannelNames{{"FL", "FR", "C", "LFE", "SL", "SR"}};
const std::array<const char*, 6> kChannelFullNames{{
    "Front Left", "Front Right", "Center (Dialog)", "Subwoofer / LFE", "Surround Left", "Surround Right"
}};
const std::array<const char*, 6> kGainKeys{{"fl_db", "fr_db", "c_db", "lfe_db", "sl_db", "sr_db"}};

// String helpers
std::wstring ExeDirectory() {
    std::wstring path(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (n == 0 || n >= path.size()) return L".";
    path.resize(n);
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : path.substr(0, slash);
}

std::string ToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, out.data(), n, nullptr, nullptr);
    out.resize(static_cast<size_t>(n - 1));
    return out;
}

std::wstring ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, out.data(), n);
    out.resize(static_cast<size_t>(n - 1));
    return out;
}

std::string Trim(std::string s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

// Config file I/O
Kv ReadKv(const std::wstring& path) {
    Kv values;
    std::ifstream in{std::filesystem::path(path), std::ios::binary};
    std::string line;
    while (std::getline(in, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        values[Trim(line.substr(0, eq))] = Trim(line.substr(eq + 1));
    }
    return values;
}

bool WriteTextAtomic(const std::wstring& path, const std::string& content) {
    const std::wstring temp = path + L".tmp";
    {
        std::ofstream out{std::filesystem::path(temp), std::ios::binary | std::ios::trunc};
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        out.flush();
        if (!out) return false;
    }
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temp.c_str());
        return false;
    }
    return true;
}

bool WriteKvAtomic(const std::wstring& path, const Kv& values) {
    std::ostringstream out;
    for (const auto& item : values) out << item.first << '=' << item.second << "\r\n";
    return WriteTextAtomic(path, out.str());
}

bool WriteControlFile(bool stop, int reloadId = 0) {
    std::ostringstream out;
    out << "stop=" << (stop ? 1 : 0) << "\r\n";
    out << "reload=" << reloadId << "\r\n";
    for (size_t i = 0; i < g_gainDb.size(); ++i) {
        int val = g_channelMute[i] ? -60 : g_gainDb[i];
        out << kGainKeys[i] << '=' << val << "\r\n";
    }
    return WriteTextAtomic(g_controlPath, out.str());
}

static bool ProbeDeviceFormat(IMMDevice* dev, const GUID& subFormat, int rate) {
    if (!dev) return false;
    IAudioClient* client = nullptr;
    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client))) || !client)
        return false;

    WAVEFORMATEXTENSIBLE_IEC61937 w{};
    w.FormatExt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    w.FormatExt.Format.nChannels = 2;
    w.FormatExt.Format.nSamplesPerSec = rate;
    w.FormatExt.Format.wBitsPerSample = 16;
    w.FormatExt.Format.nBlockAlign = 4;
    w.FormatExt.Format.nAvgBytesPerSec = rate * 4;
    w.FormatExt.Samples.wValidBitsPerSample = 16;
    w.FormatExt.dwChannelMask = KSAUDIO_SPEAKER_5POINT1;
    w.FormatExt.SubFormat = subFormat;

    // 1. Try extended format (MSDN standard)
    w.FormatExt.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE_IEC61937) - sizeof(WAVEFORMATEX);
    w.dwEncodedSamplesPerSec = rate;
    w.dwEncodedChannelCount = 6;
    w.dwAverageBytesPerSec = 0;
    HRESULT hr = client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &w.FormatExt.Format, nullptr);
    if (hr == S_OK) {
        client->Release();
        return true;
    }

    // 2. Try non-extended format (Kodi / broad compatibility)
    w.FormatExt.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    hr = client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &w.FormatExt.Format, nullptr);
    client->Release();
    return (hr == S_OK);
}

bool IsOutputAllowedForMode(const AudioDevice& dev, int mode) {
    if (mode == 0) return dev.supportsDts;
    if (mode == 1) return dev.supportsAc3;
    return dev.supportsPcm;
}

// Device enumeration
std::vector<AudioDevice> EnumerateDevices(EDataFlow flow) {
    std::vector<AudioDevice> result;
    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr) || !enumerator) return result;
    IMMDeviceCollection* collection = nullptr;
    hr = enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection);
    if (SUCCEEDED(hr) && collection) {
        UINT count = 0;
        collection->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            IMMDevice* device = nullptr;
            if (FAILED(collection->Item(i, &device)) || !device) continue;
            LPWSTR id = nullptr;
            IPropertyStore* store = nullptr;
            std::wstring name = L"(unnamed audio device)";
            std::wstring deviceId;
            bool isSpdif = false;
            if (SUCCEEDED(device->GetId(&id)) && id) deviceId = id;
            if (id) CoTaskMemFree(id);
            if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store)) && store) {
                PROPVARIANT pv;
                PropVariantInit(&pv);
                if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal)
                    name = pv.pwszVal;
                PropVariantClear(&pv);

                PropVariantInit(&pv);
                if (SUCCEEDED(store->GetValue(PKEY_AudioEndpoint_FormFactor, &pv)) && pv.vt == VT_UI4)
                    isSpdif = (pv.ulVal == 5 /* SPDIF */);
                PropVariantClear(&pv);

                store->Release();
            }

            bool supportsAc3 = false;
            bool supportsDts = false;
            if (flow == eRender) {
                supportsAc3 = ProbeDeviceFormat(device, kGuidSubtypeDolbyDigital, 48000);
                supportsDts = ProbeDeviceFormat(device, kGuidSubtypeDts, 48000);
            }

            if (!deviceId.empty()) {
                AudioDevice dev;
                dev.name = name;
                dev.id = deviceId;
                dev.isSpdif = isSpdif;
                dev.supportsAc3 = supportsAc3;
                dev.supportsDts = supportsDts;
                dev.supportsPcm = true;
                result.push_back(dev);
            }
            device->Release();
        }
        collection->Release();
    }
    enumerator->Release();
    return result;
}

void RefreshDevices() {
    g_captureDevices = EnumerateDevices(eCapture);
    g_renderDevices = EnumerateDevices(eRender);
}

int FindDeviceById(const std::vector<AudioDevice>& devices, const std::string& id) {
    if (id.empty()) return -1;
    for (size_t i = 0; i < devices.size(); ++i)
        if (ToUtf8(devices[i].id) == id) return static_cast<int>(i);
    return -1;
}

int FindDeviceBySubstring(const std::vector<AudioDevice>& devices, const std::string& sub) {
    for (size_t i = 0; i < devices.size(); ++i)
        if (ToUtf8(devices[i].name).find(sub) != std::string::npos) return static_cast<int>(i);
    return -1;
}

// Engine process management
void CloseProcessHandles() {
    if (g_engine.hThread) CloseHandle(g_engine.hThread);
    if (g_engine.hProcess) CloseHandle(g_engine.hProcess);
    ZeroMemory(&g_engine, sizeof(g_engine));
}

bool EngineRunning() {
    if (!g_engine.hProcess) return false;
    DWORD code = 0;
    if (!GetExitCodeProcess(g_engine.hProcess, &code)) return false;
    return code == STILL_ACTIVE;
}

void KillOrphanedEngineProcesses() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"engine.exe") == 0) {
                if (g_engine.hProcess && pe.th32ProcessID == g_engine.dwProcessId) continue;
                HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                if (hProc) {
                    TerminateProcess(hProc, 0);
                    CloseHandle(hProc);
                }
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
}

void StopEngine() {
    if (g_engine.hProcess && EngineRunning()) {
        WriteControlFile(true); // Signal graceful stop
        DWORD wait = WaitForSingleObject(g_engine.hProcess, 1500);
        if (wait == WAIT_TIMEOUT) {
            TerminateProcess(g_engine.hProcess, 3);
            WaitForSingleObject(g_engine.hProcess, 1000);
        }
    }
    CloseProcessHandles();
    KillOrphanedEngineProcesses();
    WriteControlFile(false);
}

bool StartEngine() {
    StopEngine();

    const std::wstring enginePath = g_exeDir + L"\\engine.exe";
    const DWORD attrs = GetFileAttributesW(enginePath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        return false;
    }

    std::wstring command = L"\"" + enginePath + L"\" --hidden --log \"" + g_logPath + L"\"";
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};

    if (!CreateProcessW(enginePath.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, g_exeDir.c_str(), &si, &pi)) {
        return false;
    }

    g_engine = pi;
    CloseHandle(g_engine.hThread);
    g_engine.hThread = nullptr;
    return true;
}

void SaveConfiguration(int reloadId = 0) {
    Kv cfg = ReadKv(g_configPath);

    const auto& inDevs = g_loopback ? g_renderDevices : g_captureDevices;
    if (g_selectedInDevice >= 0 && static_cast<size_t>(g_selectedInDevice) < inDevs.size())
        cfg["in_id"] = ToUtf8(inDevs[static_cast<size_t>(g_selectedInDevice)].id);

    if (g_outAutoSpdif) {
        cfg["out_spdif"] = "1";
        cfg.erase("out_id");
        cfg.erase("out");
    } else {
        if (g_selectedOutDevice >= 0 && static_cast<size_t>(g_selectedOutDevice) < g_renderDevices.size())
            cfg["out_id"] = ToUtf8(g_renderDevices[static_cast<size_t>(g_selectedOutDevice)].id);
        cfg.erase("out_spdif");
        cfg.erase("out");
    }

    cfg.erase("in");
    cfg["loopback"] = g_loopback ? "1" : "0";

    if (g_outputMode == 0) {
        cfg["output_mode"] = "dts";
        cfg["dts_rate"] = (g_dtsBitrate == 0) ? "full" : "half";
    } else if (g_outputMode == 1) {
        cfg["output_mode"] = "ac3";
        cfg["bitrate"] = (g_ac3Bitrate == 0) ? "640000" : "448000";
    } else {
        cfg["output_mode"] = "pcm";
    }

    const char* latencies[] = {"extreme", "fast", "balanced", "safe", "relaxed"};
    cfg["latency"] = latencies[std::clamp(g_latencyPreset, 0, 4)];
    cfg.erase("safe"); // Use preset

    const char* upmixes[] = {"dynamic", "front", "3.0", "2.1", "always", "always_front", "off"};
    cfg["upmix"] = upmixes[std::clamp(g_upmixMode, 0, 6)];

    WriteKvAtomic(g_configPath, cfg);
    WriteControlFile(false, reloadId);
}

void TriggerApplySettings() {
    g_reloadId++;
    SaveConfiguration(g_reloadId);
    if (!EngineRunning()) {
        StartEngine();
    }
}

void LoadConfiguration() {
    Kv cfg = ReadKv(g_configPath);

    g_loopback = (cfg.find("loopback") == cfg.end()) || (cfg["loopback"] == "1");
    g_outAutoSpdif = (cfg["out_spdif"] == "1") || (cfg.find("out_id") == cfg.end() && cfg.find("out") == cfg.end());

    RefreshDevices();

    const auto& inDevs = g_loopback ? g_renderDevices : g_captureDevices;
    g_selectedInDevice = FindDeviceById(inDevs, cfg["in_id"]);
    if (g_selectedInDevice < 0)
        g_selectedInDevice = FindDeviceBySubstring(inDevs, g_loopback ? "CABLE Input" : "CABLE Output");
    if (g_selectedInDevice < 0 && !inDevs.empty()) g_selectedInDevice = 0;

    g_selectedOutDevice = FindDeviceById(g_renderDevices, cfg["out_id"]);
    if (g_selectedOutDevice < 0)
        g_selectedOutDevice = FindDeviceBySubstring(g_renderDevices, "Realtek Digital");
    if (g_selectedOutDevice < 0 && !g_renderDevices.empty()) g_selectedOutDevice = 0;

    const std::string mode = cfg["output_mode"];
    if (mode == "dts") g_outputMode = 0;
    else if (mode == "pcm") g_outputMode = 2;
    else g_outputMode = 1; // ac3 default

    const std::string dtsR = cfg["dts_rate"];
    g_dtsBitrate = (dtsR == "half" || dtsR == "768000" || dtsR == "768k") ? 1 : 0;

    const std::string ac3R = cfg["bitrate"];
    g_ac3Bitrate = (ac3R == "448000" || ac3R == "448k" || ac3R == "448") ? 1 : 0;

    const std::string lat = cfg["latency"];
    if (lat == "extreme" || lat == "ultra") g_latencyPreset = 0;
    else if (lat == "fast" || lat == "low") g_latencyPreset = 1;
    else if (lat == "safe") g_latencyPreset = 3;
    else if (lat == "relaxed" || lat == "normal") g_latencyPreset = 4;
    else g_latencyPreset = 2; // balanced

    const std::string up = cfg["upmix"];
    if (up == "off") g_upmixMode = 6;
    else if (up == "always_front" || up == "always_3.1") g_upmixMode = 5;
    else if (up == "always") g_upmixMode = 4;
    else if (up == "2.1" || up == "stereo_sub") g_upmixMode = 3;
    else if (up == "3.0" || up == "front30") g_upmixMode = 2;
    else if (up == "front" || up == "3.1" || up == "frontstage" || up == "center_lfe") g_upmixMode = 1;
    else g_upmixMode = 0; // dynamic surround (or legacy "surround")

    // Read control gains
    std::ifstream in{std::filesystem::path(g_controlPath)};
    std::string line;
    while (std::getline(in, line)) {
        line = Trim(line);
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = Trim(line.substr(0, eq));
        const std::string val = Trim(line.substr(eq + 1));
        for (size_t i = 0; i < kGainKeys.size(); ++i) {
            if (key == kGainKeys[i]) {
                try {
                    int db = std::stoi(val);
                    if (db <= -60) {
                        g_channelMute[i] = true;
                        g_gainDb[i] = 0;
                    } else {
                        g_channelMute[i] = false;
                        g_gainDb[i] = std::clamp(db, -60, 0);
                    }
                } catch (...) {}
            }
        }
    }
}

// System tray helpers
void InitTrayIcon(HWND hwnd) {
    g_notifyIconData.cbSize = sizeof(NOTIFYICONDATAW);
    g_notifyIconData.hWnd = hwnd;
    g_notifyIconData.uID = 1;
    g_notifyIconData.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_notifyIconData.uCallbackMessage = WM_TRAYICON;
    HICON hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101));
    if (!hIcon) hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512) /* IDI_APPLICATION */);
    g_notifyIconData.hIcon = hIcon;
    wcscpy_s(g_notifyIconData.szTip, L"Dolby Control Center (DTS / AC3 5.1)");
    Shell_NotifyIconW(NIM_ADD, &g_notifyIconData);
}

void RemoveTrayIcon() {
    Shell_NotifyIconW(NIM_DELETE, &g_notifyIconData);
}

void ShowTrayContextMenu(HWND hwnd) {
    POINT pt;
    GetCursorPos(&pt);
    HMENU hMenu = CreatePopupMenu();
    InsertMenuW(hMenu, 0, MF_BYPOSITION | MF_STRING, ID_TRAY_RESTORE, L"Open Control Center");
    const bool isRunning = EngineRunning();
    InsertMenuW(hMenu, 1, MF_BYPOSITION | MF_STRING, ID_TRAY_TOGGLE,
                isRunning ? L"Disable (Release S/PDIF)" : L"Enable Encoding");
    InsertMenuW(hMenu, 2, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
    InsertMenuW(hMenu, 3, MF_BYPOSITION | MF_STRING, ID_TRAY_EXIT, L"Exit");

    SetForegroundWindow(hwnd);
    TrackPopupMenu(hMenu, TPM_RIGHTALIGN | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(hMenu);
}

// DirectX 11 helpers
bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                                createDeviceFlags, featureLevelArray, 2,
                                                D3D11_SDK_VERSION, &sd, &g_pSwapChain,
                                                &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res == DXGI_ERROR_UNSUPPORTED)
        res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                                            createDeviceFlags, featureLevelArray, 2,
                                            D3D11_SDK_VERSION, &sd, &g_pSwapChain,
                                            &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res != S_OK) return false;

    ID3D11Texture2D* pBackBuffer = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
    return true;
}

void CleanupRenderTarget() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

// Dark Audio Style for Dear ImGui
void SetupModernAudioStyle() {
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;

    style.WindowRounding    = 8.0f;
    style.FrameRounding     = 5.0f;
    style.PopupRounding     = 6.0f;
    style.GrabRounding      = 4.0f;
    style.ScrollbarRounding = 6.0f;
    style.TabRounding       = 5.0f;
    style.WindowBorderSize  = 1.0f;
    style.FrameBorderSize   = 1.0f;
    style.ItemSpacing       = ImVec2(10.0f, 8.0f);
    style.ItemInnerSpacing  = ImVec2(8.0f, 6.0f);
    style.WindowPadding     = ImVec2(16.0f, 16.0f);

    // Deep slate and dark cyber-audio palette
    colors[ImGuiCol_Text]                  = ImVec4(0.92f, 0.94f, 0.96f, 1.00f);
    colors[ImGuiCol_TextDisabled]          = ImVec4(0.50f, 0.55f, 0.60f, 1.00f);
    colors[ImGuiCol_WindowBg]              = ImVec4(0.09f, 0.10f, 0.12f, 1.00f); // #17191F
    colors[ImGuiCol_ChildBg]               = ImVec4(0.12f, 0.13f, 0.16f, 1.00f);
    colors[ImGuiCol_PopupBg]               = ImVec4(0.12f, 0.14f, 0.18f, 0.98f);
    colors[ImGuiCol_Border]                = ImVec4(0.20f, 0.24f, 0.28f, 0.70f);
    colors[ImGuiCol_BorderShadow]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_FrameBg]               = ImVec4(0.15f, 0.17f, 0.21f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]        = ImVec4(0.20f, 0.24f, 0.30f, 1.00f);
    colors[ImGuiCol_FrameBgActive]         = ImVec4(0.24f, 0.29f, 0.36f, 1.00f);
    colors[ImGuiCol_TitleBg]               = ImVec4(0.09f, 0.10f, 0.12f, 1.00f);
    colors[ImGuiCol_TitleBgActive]         = ImVec4(0.12f, 0.14f, 0.18f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed]      = ImVec4(0.09f, 0.10f, 0.12f, 0.75f);
    colors[ImGuiCol_MenuBarBg]             = ImVec4(0.12f, 0.14f, 0.18f, 1.00f);
    colors[ImGuiCol_ScrollbarBg]           = ImVec4(0.09f, 0.10f, 0.12f, 0.50f);
    colors[ImGuiCol_ScrollbarGrab]         = ImVec4(0.24f, 0.28f, 0.34f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.30f, 0.36f, 0.44f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.38f, 0.46f, 0.56f, 1.00f);
    colors[ImGuiCol_CheckMark]             = ImVec4(0.15f, 0.85f, 0.65f, 1.00f); // Bright Mint
    colors[ImGuiCol_SliderGrab]            = ImVec4(0.13f, 0.75f, 0.85f, 1.00f); // Vibrant Cyan
    colors[ImGuiCol_SliderGrabActive]      = ImVec4(0.25f, 0.85f, 0.95f, 1.00f);
    colors[ImGuiCol_Button]                = ImVec4(0.17f, 0.20f, 0.26f, 1.00f);
    colors[ImGuiCol_ButtonHovered]         = ImVec4(0.23f, 0.28f, 0.36f, 1.00f);
    colors[ImGuiCol_ButtonActive]          = ImVec4(0.28f, 0.35f, 0.45f, 1.00f);
    colors[ImGuiCol_Header]                = ImVec4(0.18f, 0.23f, 0.30f, 1.00f);
    colors[ImGuiCol_HeaderHovered]         = ImVec4(0.24f, 0.30f, 0.40f, 1.00f);
    colors[ImGuiCol_HeaderActive]          = ImVec4(0.30f, 0.38f, 0.50f, 1.00f);
    colors[ImGuiCol_Separator]             = ImVec4(0.20f, 0.24f, 0.28f, 0.70f);
    colors[ImGuiCol_SeparatorHovered]      = ImVec4(0.30f, 0.36f, 0.44f, 1.00f);
    colors[ImGuiCol_SeparatorActive]       = ImVec4(0.13f, 0.75f, 0.85f, 1.00f);
    colors[ImGuiCol_Tab]                   = ImVec4(0.14f, 0.17f, 0.22f, 1.00f);
    colors[ImGuiCol_TabHovered]            = ImVec4(0.22f, 0.28f, 0.36f, 1.00f);
    colors[ImGuiCol_TabActive]             = ImVec4(0.18f, 0.23f, 0.30f, 1.00f);
    colors[ImGuiCol_PlotLines]             = ImVec4(0.13f, 0.75f, 0.85f, 1.00f);
    colors[ImGuiCol_PlotHistogram]         = ImVec4(0.15f, 0.85f, 0.65f, 1.00f);
}

// 6-channel VU meters renderer (100% static mathematical layout, zero jitter)
void DrawSpeakerVUMeters(bool isRunning) {
    ImGui::TextColored(ImVec4(0.7f, 0.75f, 0.8f, 1.0f), "REAL-TIME 5.1 SPEAKER METERS");

    const float colWidth = 56.0f;
    const float colSpacing = 10.0f;
    const float barWidth = 24.0f;
    const float meterHeight = 130.0f;

    // Read shared memory levels
    std::array<float, 6> rawPeaks{{0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}};
    if (isRunning && g_meterMem.IsOpen()) {
        const MeterSharedState* state = g_meterMem.Get();
        if (state && state->active) {
            for (size_t i = 0; i < 6; ++i)
                rawPeaks[i] = state->peakLevels[i];
        }
    }

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 startPos = ImGui::GetCursorScreenPos();
    const float headerH = ImGui::GetTextLineHeight();
    const float meterTopY = startPos.y + headerH + 6.0f;
    const float meterBottomY = meterTopY + meterHeight;
    const float textY = meterBottomY + 6.0f;

    for (size_t i = 0; i < 6; ++i) {
        // Static horizontal position for channel i (never moves or fluctuates)
        const float colStartX = startPos.x + static_cast<float>(i) * (colWidth + colSpacing);

        // Studio ballistics: fast attack (~15 ms) and time-based exponential release (~20 dB/s)
        const float dt = ImGui::GetIO().DeltaTime;
        const float attackRate = std::clamp(dt * 60.0f, 0.15f, 1.0f);
        const float decayFactor = std::exp(-dt * 4.5f);

        const float targetPeak = rawPeaks[i];
        if (targetPeak >= g_smoothedPeaks[i]) {
            g_smoothedPeaks[i] += (targetPeak - g_smoothedPeaks[i]) * attackRate;
        } else {
            g_smoothedPeaks[i] *= decayFactor;
            if (g_smoothedPeaks[i] < 0.0001f) g_smoothedPeaks[i] = 0.0f;
        }

        // Peak-hold indicator
        if (targetPeak >= g_peakHold[i]) {
            g_peakHold[i] = targetPeak;
            g_peakHoldFrames[i] = 45; // Hold for ~0.75s
        } else if (g_peakHoldFrames[i] > 0) {
            g_peakHoldFrames[i]--;
        } else {
            g_peakHold[i] *= std::exp(-dt * 2.5f);
            if (g_peakHold[i] < 0.0001f) g_peakHold[i] = 0.0f;
        }

        const float linearVal = g_smoothedPeaks[i];
        float db = (linearVal > 0.0001f) ? (20.0f * std::log10(linearVal)) : -60.0f;
        db = std::clamp(db, -60.0f, 0.0f);
        const float norm = (db + 60.0f) / 60.0f; // 0.0 to 1.0

        // 1. Channel Header (Centered statically in colWidth)
        const char* name = kChannelNames[i];
        const ImVec2 nameSize = ImGui::CalcTextSize(name);
        const float nameX = colStartX + (colWidth - nameSize.x) * 0.5f;
        drawList->AddText(ImVec2(nameX, startPos.y), IM_COL32(33, 191, 217, 255), name);

        // 2. Meter Bar Background (Centered statically in colWidth)
        const float barX = colStartX + (colWidth - barWidth) * 0.5f;
        const ImVec2 bgMin = ImVec2(barX, meterTopY);
        const ImVec2 bgMax = ImVec2(barX + barWidth, meterBottomY);

        drawList->AddRectFilled(bgMin, bgMax, IM_COL32(20, 24, 30, 255), 4.0f);
        drawList->AddRect(bgMin, bgMax, IM_COL32(40, 48, 58, 255), 4.0f);

        // 3. Fill active level
        if (norm > 0.01f) {
            const float filledHeight = norm * meterHeight;
            const ImVec2 fillMin = ImVec2(barX + 2, bgMax.y - filledHeight);
            const ImVec2 fillMax = ImVec2(bgMax.x - 2, bgMax.y - 2);

            ImU32 colTop = IM_COL32(32, 226, 128, 255);    // Green
            ImU32 colBottom = IM_COL32(20, 180, 100, 255);
            if (db > -3.0f) {
                colTop = IM_COL32(235, 60, 60, 255);       // Red clip
            } else if (db > -12.0f) {
                colTop = IM_COL32(245, 170, 35, 255);      // Yellow warm
            }

            drawList->AddRectFilledMultiColor(fillMin, fillMax, colTop, colTop, colBottom, colBottom);
        }

        // 4. Peak-hold line
        const float holdDb = (g_peakHold[i] > 0.0001f) ? (20.0f * std::log10(g_peakHold[i])) : -60.0f;
        const float holdNorm = std::clamp((holdDb + 60.0f) / 60.0f, 0.0f, 1.0f);
        if (holdNorm > 0.02f) {
            const float holdY = bgMax.y - (holdNorm * meterHeight);
            drawList->AddLine(ImVec2(barX + 1, holdY), ImVec2(bgMax.x - 1, holdY),
                              IM_COL32(255, 255, 255, 220), 1.5f);
        }

        // 5. dB text label (Centered statically in colWidth)
        char dbText[16];
        ImU32 textColor = IM_COL32(235, 240, 245, 255);
        if (g_channelMute[i]) {
            strcpy_s(dbText, "MUTE");
            textColor = IM_COL32(220, 80, 80, 255);
        } else if (db <= -59.5f) {
            strcpy_s(dbText, "-inf");
            textColor = IM_COL32(130, 140, 150, 255);
        } else {
            snprintf(dbText, sizeof(dbText), "%.1f", db);
        }
        const ImVec2 textSize = ImGui::CalcTextSize(dbText);
        const float textX = colStartX + (colWidth - textSize.x) * 0.5f;
        drawList->AddText(ImVec2(textX, textY), textColor, dbText);
    }

    // Advance cursor past the entire static meter block
    const float totalWidth = 6.0f * colWidth + 5.0f * colSpacing;
    const float totalHeight = (textY + headerH + 6.0f) - startPos.y;
    ImGui::Dummy(ImVec2(totalWidth, totalHeight));
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_TRAYICON:
        if (lParam == WM_LBUTTONDBLCLK) {
            ShowWindow(hWnd, SW_SHOW);
            ShowWindow(hWnd, SW_RESTORE);
            SetForegroundWindow(hWnd);
        } else if (lParam == WM_RBUTTONUP) {
            ShowTrayContextMenu(hWnd);
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case ID_TRAY_RESTORE:
            ShowWindow(hWnd, SW_SHOW);
            ShowWindow(hWnd, SW_RESTORE);
            SetForegroundWindow(hWnd);
            break;
        case ID_TRAY_TOGGLE:
            if (EngineRunning()) {
                g_engineEnabled = false;
                StopEngine();
            } else {
                g_engineEnabled = true;
                SaveConfiguration();
                StartEngine();
            }
            break;
        case ID_TRAY_EXIT:
            g_engineEnabled = false;
            StopEngine();
            DestroyWindow(hWnd);
            break;
        }
        return 0;

    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_MINIMIZE) {
            ShowWindow(hWnd, SW_HIDE);
            return 0;
        }
        break;

    case WM_CLOSE:
        ShowWindow(hWnd, SW_HIDE);
        return 0; // Minimize to tray instead of quitting

    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = 760;
        info->ptMinTrackSize.y = 650;
        return 0;
    }

    case WM_SIZE:
        if (g_pd3dDevice != nullptr && wParam != SIZE_MINIMIZED) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            ID3D11Texture2D* pBackBuffer = nullptr;
            g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
            g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
            pBackBuffer->Release();
        }
        return 0;

    case WM_DESTROY:
        RemoveTrayIcon();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR pCmdLine, int) {
    HANDLE hSingleInstanceMutex = CreateMutexW(nullptr, FALSE, L"Local\\VirtualAc3DtsControlCenter_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existingWnd = FindWindowW(L"VirtualAc3DtsControlCenter", nullptr);
        if (existingWnd) {
            ShowWindow(existingWnd, SW_SHOW);
            ShowWindow(existingWnd, SW_RESTORE);
            SetForegroundWindow(existingWnd);
        }
        if (hSingleInstanceMutex) CloseHandle(hSingleInstanceMutex);
        return 0;
    }

    g_exeDir = ExeDirectory();
    g_configPath = g_exeDir + L"\\virtual-ac3-encoder.conf";
    g_controlPath = g_exeDir + L"\\ac3-control.ini";
    g_logPath = g_exeDir + L"\\engine.log";

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    HICON hAppIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(101));
    if (!hAppIcon) hAppIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512) /* IDI_APPLICATION */);

    WNDCLASSEXW wc = {
        sizeof(WNDCLASSEXW), CS_CLASSDC, WndProc, 0L, 0L, hInstance,
        hAppIcon, LoadCursor(nullptr, IDC_ARROW),
        nullptr, nullptr, L"VirtualAc3DtsControlCenter", nullptr
    };
    RegisterClassExW(&wc);

    const int winWidth = 780;
    const int winHeight = 860;
    const int screenW = GetSystemMetrics(SM_CXSCREEN);
    const int screenH = GetSystemMetrics(SM_CYSCREEN);
    const int posX = std::max(0, (screenW - winWidth) / 2);
    const int posY = std::max(0, (screenH - winHeight) / 2);

    g_hWnd = CreateWindowW(wc.lpszClassName, L"Dolby Control Center",
                           WS_OVERLAPPEDWINDOW,
                           posX, posY, winWidth, winHeight, nullptr, nullptr, wc.hInstance, nullptr);

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE_OLD
#define DWMWA_USE_IMMERSIVE_DARK_MODE_OLD 19
#endif
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif

    if (g_hWnd) {
        BOOL useDarkMode = TRUE;
        if (FAILED(DwmSetWindowAttribute(g_hWnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDarkMode, sizeof(useDarkMode)))) {
            DwmSetWindowAttribute(g_hWnd, DWMWA_USE_IMMERSIVE_DARK_MODE_OLD, &useDarkMode, sizeof(useDarkMode));
        }
        COLORREF captionColor = RGB(0x17, 0x19, 0x1F); // Matches #17191F dark UI background
        DwmSetWindowAttribute(g_hWnd, DWMWA_CAPTION_COLOR, &captionColor, sizeof(captionColor));
    }

    if (hAppIcon && g_hWnd) {
        SendMessageW(g_hWnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(hAppIcon));
        SendMessageW(g_hWnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hAppIcon));
    }

    if (!CreateDeviceD3D(g_hWnd)) {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    InitTrayIcon(g_hWnd);
    const bool startMinimized = (pCmdLine && (wcsstr(pCmdLine, L"--minimized") || wcsstr(pCmdLine, L"--tray")));
    if (!startMinimized) {
        ShowWindow(g_hWnd, SW_SHOWDEFAULT);
        UpdateWindow(g_hWnd);
    }

    // Initialize Dear ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr; // Don't create imgui.ini

    SetupModernAudioStyle();
    ImGui_ImplWin32_Init(g_hWnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    // Open telemetry shared memory
    g_meterMem.OpenConsumer();

    // Load initial settings
    LoadConfiguration();

    // Start engine automatically if not running
    if (!EngineRunning()) {
        StartEngine();
    }

    auto lastCheck = std::chrono::steady_clock::now();
    auto lastRestartAttempt = std::chrono::steady_clock::now();

    // Main render loop
    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done) break;

        // Periodic check (every 500 ms) for shared memory reconnection and auto-supervision
        const auto now = std::chrono::steady_clock::now();
        if (now - lastCheck >= std::chrono::milliseconds(500)) {
            lastCheck = now;
            if (!g_meterMem.IsOpen()) g_meterMem.OpenConsumer();
            if (g_engineEnabled && !EngineRunning()) {
                if (now - lastRestartAttempt >= std::chrono::seconds(4)) {
                    lastRestartAttempt = now;
                    StartEngine();
                }
            }
        }

        const bool isRunning = EngineRunning();

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // Single fullscreen ImGui window matching the client area
        RECT rect;
        GetClientRect(g_hWnd, &rect);
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(static_cast<float>(rect.right), static_cast<float>(rect.bottom)));
        ImGui::Begin("MainControlPanel", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

        // --- 1. Master Power Bar ---
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
        if (isRunning) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.65f, 0.40f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.15f, 0.75f, 0.45f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.10f, 0.55f, 0.35f, 1.0f));
            if (ImGui::Button("[ ACTIVE: ENCODING TO S/PDIF ]  (Click to Disable & Release Port)", ImVec2(-1, 38))) {
                g_engineEnabled = false;
                StopEngine();
            }
            ImGui::PopStyleColor(3);
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.28f, 0.32f, 0.38f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.35f, 0.40f, 0.48f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.22f, 0.25f, 0.30f, 1.0f));
            if (ImGui::Button("[ DISABLED: S/PDIF RELEASED ]  (Click to Enable Encoding)", ImVec2(-1, 38))) {
                g_engineEnabled = true;
                SaveConfiguration();
                StartEngine();
            }
            ImGui::PopStyleColor(3);
        }
        ImGui::PopStyleVar();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // --- 2. Real-Time VU Meters ---
        DrawSpeakerVUMeters(isRunning);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // --- 3. Audio Routing & Hardware Configuration ---
        if (ImGui::CollapsingHeader("AUDIO ROUTING & FORMAT", ImGuiTreeNodeFlags_DefaultOpen)) {
            // Loopback Capture Toggle
            if (ImGui::Checkbox("WASAPI Loopback Capture (Play to CABLE Input, capture directly)", &g_loopback)) {
                const auto& newDevs = g_loopback ? g_renderDevices : g_captureDevices;
                g_selectedInDevice = FindDeviceBySubstring(newDevs, g_loopback ? "CABLE Input" : "CABLE Output");
                if (g_selectedInDevice < 0 && !newDevs.empty()) g_selectedInDevice = 0;
                TriggerApplySettings();
            }
            ImGui::SameLine();
            ImGui::TextDisabled(g_loopback ? "(Captures Playback device, e.g. CABLE Input)" : "(Captures Recording endpoint, e.g. Mics / CABLE Output)");

            const auto& inDevs = g_loopback ? g_renderDevices : g_captureDevices;

            // Input Device
            std::string inCurrentName = (g_selectedInDevice >= 0 && static_cast<size_t>(g_selectedInDevice) < inDevs.size())
                ? ToUtf8(inDevs[static_cast<size_t>(g_selectedInDevice)].name) : "Select Input...";
            const char* inComboLabel = g_loopback ? "Input Device (Playback / Loopback)" : "Input Device (Recording / Mic)";
            if (ImGui::BeginCombo(inComboLabel, inCurrentName.c_str())) {
                for (int i = 0; i < static_cast<int>(inDevs.size()); ++i) {
                    const bool isSel = (g_selectedInDevice == i);
                    if (ImGui::Selectable(ToUtf8(inDevs[static_cast<size_t>(i)].name).c_str(), isSel)) {
                        g_selectedInDevice = i;
                        TriggerApplySettings();
                    }
                    if (isSel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            // Output S/PDIF Device
            std::vector<int> compatibleIndices;
            for (int i = 0; i < static_cast<int>(g_renderDevices.size()); ++i) {
                if (g_showAllRenderDevices || IsOutputAllowedForMode(g_renderDevices[static_cast<size_t>(i)], g_outputMode)) {
                    compatibleIndices.push_back(i);
                }
            }

            std::string outCurrentName;
            if (g_outAutoSpdif) {
                outCurrentName = "[Auto-Detect First S/PDIF Optical Port] (out_spdif=1)";
            } else if (g_selectedOutDevice >= 0 && static_cast<size_t>(g_selectedOutDevice) < g_renderDevices.size()) {
                const auto& dev = g_renderDevices[static_cast<size_t>(g_selectedOutDevice)];
                outCurrentName = ToUtf8(dev.name);
                if (dev.isSpdif) outCurrentName += " [SPDIF]";
            } else {
                outCurrentName = "Select Output...";
            }

            if (ImGui::BeginCombo("Digital Output (S/PDIF)", outCurrentName.c_str())) {
                const bool isAutoSel = g_outAutoSpdif;
                if (ImGui::Selectable("[Auto-Detect First S/PDIF Optical Port] (Recommended)", isAutoSel)) {
                    g_outAutoSpdif = true;
                    TriggerApplySettings();
                }
                if (isAutoSel) ImGui::SetItemDefaultFocus();

                ImGui::Separator();

                if (compatibleIndices.empty()) {
                    ImGui::TextDisabled("  (No devices found supporting this mode)");
                } else {
                    for (int i : compatibleIndices) {
                        const bool isSel = (!g_outAutoSpdif && g_selectedOutDevice == i);
                        const auto& dev = g_renderDevices[static_cast<size_t>(i)];
                        std::string devLabel = ToUtf8(dev.name);
                        if (dev.isSpdif) devLabel += " [SPDIF]";
                        if (dev.supportsDts && dev.supportsAc3) devLabel += " (DTS+AC3)";
                        else if (dev.supportsDts) devLabel += " (DTS)";
                        else if (dev.supportsAc3) devLabel += " (AC3)";
                        else devLabel += " (PCM Only)";

                        if (ImGui::Selectable(devLabel.c_str(), isSel)) {
                            g_outAutoSpdif = false;
                            g_selectedOutDevice = i;
                            TriggerApplySettings();
                        }
                        if (isSel) ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }

            ImGui::SameLine();
            if (ImGui::Button("Refresh Devices")) {
                RefreshDevices();
            }

            ImGui::Checkbox("Show all system audio outputs (including incompatible / analog)", &g_showAllRenderDevices);

            // Output Format
            const char* modes[] = { "DTS 5.1 Surround", "Dolby Digital 5.1 (AC3)", "PCM Stereo 2.0 (Pass-through)" };
            if (ImGui::Combo("Encoder Mode", &g_outputMode, modes, IM_ARRAYSIZE(modes))) {
                if (!g_outAutoSpdif && g_selectedOutDevice >= 0 && static_cast<size_t>(g_selectedOutDevice) < g_renderDevices.size()) {
                    if (!IsOutputAllowedForMode(g_renderDevices[static_cast<size_t>(g_selectedOutDevice)], g_outputMode)) {
                        g_outAutoSpdif = true; // Fallback to auto if selected device is incompatible
                    }
                }
                TriggerApplySettings();
            }

            // Bitrates
            if (g_outputMode == 0) { // DTS
                const char* dtsRates[] = { "1536 kbps (Full-Rate Core - Maximum Fidelity)", "768 kbps (Half-Rate Core - High Compatibility)" };
                if (ImGui::Combo("DTS Bitrate", &g_dtsBitrate, dtsRates, IM_ARRAYSIZE(dtsRates))) {
                    TriggerApplySettings();
                }
            } else if (g_outputMode == 1) { // AC3
                const char* ac3Rates[] = { "640 kbps (Maximum Optical Quality)", "448 kbps (Standard DVD Rate)" };
                if (ImGui::Combo("AC3 Bitrate", &g_ac3Bitrate, ac3Rates, IM_ARRAYSIZE(ac3Rates))) {
                    TriggerApplySettings();
                }
            }

            // Latency Presets
            const char* latencies[] = {
                "Extreme (~3 ms / 128 smp) - High-End PC Only",
                "Fast (~7 ms / 256 smp) - Competitive Gaming",
                "Balanced (~12 ms / 512 smp) - Recommended",
                "Safe (~20 ms / 768 smp) - High Stability",
                "Relaxed (~35 ms / 1536 smp) - Conservative / Laptop"
            };
            if (ImGui::Combo("Latency Preset", &g_latencyPreset, latencies, IM_ARRAYSIZE(latencies))) {
                TriggerApplySettings();
            }

            // Stereo Upmixing
            const char* upmixes[] = {
                "Dynamic Surround 5.1 (Auto: Center + Sub + Rears for Stereo, 5.1 Untouched)",
                "Front Stage 3.1 (Center + Subwoofer / LFE, Rears Silent - Home Theater)",
                "Front Stage 3.0 (Center + Front L/R, No Sub / Rears - HDMI ARC / Soundbar)",
                "Stereo + Sub 2.1 (Front L/R + Subwoofer / LFE, Center & Rears Silent)",
                "Always Surround 5.1 (Forced 5.1 on All Audio)",
                "Always Front Stage 3.1 (Forced 3.1 on All Audio - Rears Silent)",
                "Off (Pure Stereo 2.0: Front L/R Only, Center & Sub Silent)"
            };
            if (ImGui::Combo("Stereo Upmix", &g_upmixMode, upmixes, IM_ARRAYSIZE(upmixes))) {
                TriggerApplySettings();
            }

            ImGui::Spacing();
            const char* activeModeStr = (g_outputMode == 0) ? "DTS 5.1" : ((g_outputMode == 1) ? "Dolby Digital (AC3)" : "PCM Stereo");
            ImGui::TextColored(ImVec4(0.20f, 0.85f, 0.65f, 1.0f), "[Settings Active: %s | %s Latency]", activeModeStr, latencies[g_latencyPreset]);
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // --- 4. Channel Gain Sliders ---
        if (ImGui::CollapsingHeader("SPEAKER LEVEL TRIMS (-60 dB to 0 dB)", ImGuiTreeNodeFlags_DefaultOpen)) {
            bool gainsChanged = false;

            for (size_t i = 0; i < 6; ++i) {
                ImGui::PushID(static_cast<int>(i));

                // Mute button
                if (g_channelMute[i]) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.2f, 0.2f, 1.0f));
                    if (ImGui::Button("Muted", ImVec2(55, 0))) {
                        g_channelMute[i] = false;
                        gainsChanged = true;
                    }
                    ImGui::PopStyleColor();
                } else {
                    if (ImGui::Button("Mute", ImVec2(55, 0))) {
                        g_channelMute[i] = true;
                        gainsChanged = true;
                    }
                }

                ImGui::SameLine();
                char sliderLabel[64];
                snprintf(sliderLabel, sizeof(sliderLabel), "%s (%s)", kChannelNames[i], kChannelFullNames[i]);

                if (g_channelMute[i]) {
                    int dummy = -60;
                    ImGui::BeginDisabled();
                    ImGui::SliderInt(sliderLabel, &dummy, -60, 0, "MUTED");
                    ImGui::EndDisabled();
                } else {
                    if (ImGui::SliderInt(sliderLabel, &g_gainDb[i], -60, 0, "%d dB")) {
                        gainsChanged = true;
                    }
                }

                ImGui::PopID();
            }

            ImGui::Spacing();
            if (ImGui::Button("Reset All Trims to 0 dB")) {
                for (size_t i = 0; i < 6; ++i) {
                    g_gainDb[i] = 0;
                    g_channelMute[i] = false;
                }
                gainsChanged = true;
            }

            if (gainsChanged) {
                WriteControlFile(false, 0);
            }
        }

        ImGui::End();

        // Rendering
        ImGui::Render();
        const float clear_color[4] = { 0.09f, 0.10f, 0.12f, 1.00f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        g_pSwapChain->Present(1, 0); // VSync 60 FPS
    }

    // Cleanup
    g_meterMem.Close();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    DestroyWindow(g_hWnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    if (hSingleInstanceMutex) CloseHandle(hSingleInstanceMutex);
    CoUninitialize();

    return 0;
}
