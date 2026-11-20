#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <TlHelp32.h>
#include <commctrl.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cwctype>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include <filesystem>
#include <iterator>

namespace {
constexpr int ID_INPUT = 1001;
constexpr int ID_OUTPUT = 1002;
constexpr int ID_MODE = 1003;
constexpr int ID_APPLY = 1004;
constexpr int ID_START = 1005;
constexpr int ID_STOP = 1006;
constexpr int ID_RESET = 1007;
constexpr int ID_SLIDER_BASE = 1100;
constexpr int ID_VALUE_BASE = 1200;
constexpr int ID_AUTOSTART = 1008;
constexpr int ID_LOOPBACK_UPMIX = 1009;
constexpr int ID_UPMIX_DELAY = 1010;
constexpr int ID_TRAY_SHOW = 2001;
constexpr int ID_TRAY_START = 2002;
constexpr int ID_TRAY_STOP = 2003;
constexpr int ID_TRAY_AUTOSTART = 2004;
constexpr int ID_TRAY_EXIT = 2005;
constexpr UINT_PTR TIMER_ID = 1;
constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT WM_SHOW_PANEL = WM_APP + 2;
constexpr UINT TRAY_UID = 1;
constexpr wchar_t kWindowClassName[] = L"Ac3ControlCenterWindow";
constexpr wchar_t kSingleInstanceMutex[] = L"Local\\Ac3ControlCenter.SingleInstance";
constexpr wchar_t kStartupLinkName[] = L"AC3 Control Center.lnk";
constexpr int kMaxAutoRestarts = 10;

struct AudioDevice {
    std::wstring name;
    std::wstring id;
};
using Kv = std::map<std::string, std::string>;

HWND g_window = nullptr;
HWND g_inputCombo = nullptr;
HWND g_outputCombo = nullptr;
HWND g_modeCombo = nullptr;
HWND g_status = nullptr;
HWND g_startButton = nullptr;
HWND g_stopButton = nullptr;
std::array<HWND, 6> g_sliders{};
std::array<HWND, 6> g_values{};
std::array<int, 6> g_gainDb{{0, 0, 0, 0, 0, 0}};
std::vector<AudioDevice> g_captureDevices;
std::vector<AudioDevice> g_renderDevices;
bool g_inputListUsesRender = false;
PROCESS_INFORMATION g_engine{};
std::wstring g_exeDir;
std::wstring g_configPath;
std::wstring g_controlPath;
std::wstring g_logPath;
bool g_initializing = false;
bool g_silent = false;          // no modal dialogs (autostart at logon): errors go to the status line
bool g_userStopped = false;     // the user pressed Stop / tray Stop: do not auto-restart the engine
int g_restartAttempts = 0;
ULONGLONG g_nextRestartTick = 0;
ULONGLONG g_engineStartTick = 0;
HWND g_autostartCheck = nullptr;
HWND g_loopbackUpmixCombo = nullptr;
HWND g_upmixDelayLabel = nullptr;
HWND g_upmixDelayCombo = nullptr;
FILETIME g_uiIniWriteTime{};
HICON g_iconRunning = nullptr;
HICON g_iconStopped = nullptr;
bool g_trayAdded = false;
int g_trayState = -1;           // -1 = unknown, 0 = engine stopped, 1 = engine running
UINT g_taskbarCreatedMsg = 0;

const std::array<const wchar_t*, 6> kChannelNames{{
    L"Front Left (FL)", L"Front Right (FR)", L"Center (C)",
    L"Subwoofer / LFE", L"Surround Left (SL)", L"Surround Right (SR)"
}};
const std::array<const char*, 6> kGainKeys{{"fl_db", "fr_db", "c_db", "lfe_db", "sl_db", "sr_db"}};

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

std::wstring TrimW(std::wstring s) {
    const auto first = s.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return L"";
    const auto last = s.find_last_not_of(L" \t\r\n");
    return s.substr(first, last - first + 1);
}

std::string TrimA(std::string s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

Kv ReadKv(const std::wstring& path) {
    Kv values;
    std::ifstream in{std::filesystem::path(path), std::ios::binary};
    std::string line;
    while (std::getline(in, line)) {
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

bool WriteTextAtomic(const std::wstring& path, const std::string& content) {
    const std::wstring temp = path + L".tmp";
    {
        std::ofstream out{std::filesystem::path(temp), std::ios::binary | std::ios::trunc};
        if (!out) return false;
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
            if (SUCCEEDED(device->GetId(&id)) && id) deviceId = id;
            if (id) CoTaskMemFree(id);
            if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store)) && store) {
                PROPVARIANT pv;
                PropVariantInit(&pv);
                if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal)
                    name = pv.pwszVal;
                PropVariantClear(&pv);
                store->Release();
            }
            if (!deviceId.empty()) result.push_back({name, deviceId});
            device->Release();
        }
        collection->Release();
    }
    enumerator->Release();
    return result;
}

int FindById(const std::vector<AudioDevice>& devices, const std::string& id) {
    if (id.empty()) return -1;
    for (size_t i = 0; i < devices.size(); ++i)
        if (ToUtf8(devices[i].id) == id) return static_cast<int>(i);
    return -1;
}

int FindBySubstring(const std::vector<AudioDevice>& devices, const std::string& text) {
    if (text.empty()) return -1;
    for (size_t i = 0; i < devices.size(); ++i) {
        const std::string name = ToUtf8(devices[i].name);
        if (name.find(text) != std::string::npos) return static_cast<int>(i);
    }
    return -1;
}

int PreferredOutput() {
    for (size_t i = 0; i < g_renderDevices.size(); ++i) {
        std::wstring name = g_renderDevices[i].name;
        std::transform(name.begin(), name.end(), name.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        if (name.find(L"realtek digital") != std::wstring::npos ||
            name.find(L"digital output") != std::wstring::npos ||
            name.find(L"spdif") != std::wstring::npos ||
            name.find(L"s/pdif") != std::wstring::npos)
            return static_cast<int>(i);
    }
    return g_renderDevices.empty() ? -1 : 0;
}

void PopulateDeviceCombo(HWND combo, const std::vector<AudioDevice>& devices) {
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    for (const auto& device : devices) SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(device.name.c_str()));
}

bool IsLoopbackMode(int mode) {
    return mode == 4 || mode == 5 || mode == 6;
}

void UpdateInputDeviceComboForMode(int mode, const std::string& configuredId, bool preserveCurrentSelection) {
    if (!g_inputCombo) return;

    const bool useRender = IsLoopbackMode(mode);
    std::string selectedId = configuredId;

    // При переключении DTS <-> Dolby сохраняем выбранный источник.
    if (preserveCurrentSelection && g_inputListUsesRender == useRender) {
        const LRESULT oldSel = SendMessageW(g_inputCombo, CB_GETCURSEL, 0, 0);
        const auto& oldDevices = g_inputListUsesRender ? g_renderDevices : g_captureDevices;

        if (oldSel >= 0 && static_cast<size_t>(oldSel) < oldDevices.size())
            selectedId = ToUtf8(oldDevices[static_cast<size_t>(oldSel)].id);
    }

    const auto& devices = useRender ? g_renderDevices : g_captureDevices;
    PopulateDeviceCombo(g_inputCombo, devices);

    int index = FindById(devices, selectedId);
    if (index < 0)
        index = FindBySubstring(devices, useRender ? "CABLE Input" : "CABLE Output");
    if (index < 0 && !devices.empty())
        index = 0;

    if (index >= 0)
        SendMessageW(g_inputCombo, CB_SETCURSEL, index, 0);

    g_inputListUsesRender = useRender;
}

void SetStatus(const std::wstring& text) {
    if (g_status) SetWindowTextW(g_status, text.c_str());
}

// Error / warning box. In silent mode (autostart at logon) it only updates the status line.
int Msg(const std::wstring& text, UINT icon) {
    if (g_silent) {
        SetStatus(text);
        return IDOK;
    }
    return MessageBoxW(g_window, text.c_str(), L"AC3 Control Center", icon);
}

bool EngineRunning() {
    if (!g_engine.hProcess) return false;
    DWORD code = 0;
    if (!GetExitCodeProcess(g_engine.hProcess, &code)) return false;
    return code == STILL_ACTIVE;
}

bool WriteControlFile(bool stop) {
    std::ostringstream out;
    out << "stop=" << (stop ? 1 : 0) << "\r\n";
    for (size_t i = 0; i < g_gainDb.size(); ++i)
        out << kGainKeys[i] << '=' << g_gainDb[i] << "\r\n";
    return WriteTextAtomic(g_controlPath, out.str());
}

void UpdateGainLabel(size_t i) {
    if (!g_values[i]) return;
    std::wstring label;
    if (g_gainDb[i] <= -60) label = L"MUTE";
    else label = std::to_wstring(g_gainDb[i]) + L" dB";
    SetWindowTextW(g_values[i], label.c_str());
}

void ReadGainState() {
    std::ifstream in{std::filesystem::path(g_controlPath)};
    std::string line;
    while (std::getline(in, line)) {
        line = TrimA(line);
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = TrimA(line.substr(0, eq));
        const std::string value = TrimA(line.substr(eq + 1));
        for (size_t i = 0; i < kGainKeys.size(); ++i) {
            if (key == kGainKeys[i]) {
                try {
                    int db = std::stoi(value);
                    g_gainDb[i] = std::clamp(db, -60, 0);
                } catch (...) {}
            }
        }
    }
}

bool SaveSelectedConfiguration() {
    const LRESULT modeResult = SendMessageW(g_modeCombo, CB_GETCURSEL, 0, 0);

    if (modeResult < 0 || modeResult > 6) {
        Msg(L"Выбери режим кодирования.", MB_ICONWARNING);
        return false;
    }

    const int modeSel = static_cast<int>(modeResult);
    const bool loopback = IsLoopbackMode(modeSel);
    const auto& inputDevices = loopback ? g_renderDevices : g_captureDevices;

    const LRESULT inSel = SendMessageW(g_inputCombo, CB_GETCURSEL, 0, 0);
    const LRESULT outSel = SendMessageW(g_outputCombo, CB_GETCURSEL, 0, 0);

    if (inSel < 0 || static_cast<size_t>(inSel) >= inputDevices.size()) {
        Msg(L"Выбери входное устройство для выбранного режима.", MB_ICONWARNING);
        return false;
    }

    if (outSel < 0 || static_cast<size_t>(outSel) >= g_renderDevices.size()) {
        Msg(L"Выбери цифровое устройство вывода, подключённое к ресиверу.", MB_ICONWARNING);
        return false;
    }

    Kv cfg = ReadKv(g_configPath);
    if (cfg.empty()) {
        cfg["bitrate"] = "640000";
        cfg["safe"] = "1536";
    }

    cfg["in_id"] = ToUtf8(inputDevices[static_cast<size_t>(inSel)].id);
    cfg["out_id"] = ToUtf8(g_renderDevices[static_cast<size_t>(outSel)].id);
    cfg.erase("in");
    cfg.erase("out");
    cfg.erase("out_spdif");
    cfg["loopback"] = loopback ? "1" : "0";
    cfg["bitrate"] = "640000";  // Dolby Digital over S/PDIF; the DTS encoder is fixed at 768 kbps
    if (!cfg.count("headroom_db")) cfg["headroom_db"] = "-6";
    if (!cfg.count("limiter")) cfg["limiter"] = "1";
    if (!cfg.count("limiter_ceiling_db")) cfg["limiter_ceiling_db"] = "-1";

    // Stereo -> 5.1 for the PCM 5.1 (loopback) modes: auto / always / off.
    const LRESULT upmixSel = g_loopbackUpmixCombo ? SendMessageW(g_loopbackUpmixCombo, CB_GETCURSEL, 0, 0) : 0;
    const char* loopbackUpmix = upmixSel == 1 ? "always" : upmixSel == 2 ? "off" : "auto";
    cfg["loopback_upmix"] = loopbackUpmix;
    const LRESULT delaySel = g_upmixDelayCombo ? SendMessageW(g_upmixDelayCombo, CB_GETCURSEL, 0, 0) : 0;
    cfg["upmix_win_size"] = delaySel == 1 ? "2048" : delaySel == 2 ? "1024" : "4096";
    cfg["dts_in"] = modeSel == 6 ? "1" : "0";  // mode 6: DTS bitstream in the loopback is decoded

    if (modeSel == 2) {
        cfg["output_mode"] = "pcm";
        cfg["upmix"] = "off";
    } else if (modeSel == 3) {
        // DTS с обычным захватом. "auto" = как "surround" для стерео-устройства, а для 6-канального
        // захвата (CABLE Output в формате 5.1) раскладывает стерео-источник в 5.1 и не трогает настоящий 5.1.
        cfg["output_mode"] = "dts";
        cfg["upmix"] = "auto";
    } else if (modeSel == 4) {
        // Многоканальный DTS Core: Loopback; стерео-источники раскладываются в 5.1 (авто / всегда / выкл).
        cfg["output_mode"] = "dts";
        cfg["upmix"] = loopbackUpmix;
    } else if (modeSel == 5 || modeSel == 6) {
        // Многоканальный Dolby Digital: Loopback; стерео-источники раскладываются в 5.1 (авто / всегда / выкл).
        // Режим 6 дополнительно декодирует DTS-бит-стрим (dts_in=1) и кодирует его в Dolby Digital.
        cfg["output_mode"] = "ac3";
        cfg["upmix"] = loopbackUpmix;
    } else {
        cfg["output_mode"] = "ac3";
        cfg["upmix"] = modeSel == 0 ? "off" : "auto";  // see mode 3: auto also handles a 6-channel capture
    }

    if (!WriteKvAtomic(g_configPath, cfg)) {
        Msg(L"Не удалось записать virtual-ac3-encoder.conf. Проверь права доступа к папке.", MB_ICONERROR);
        return false;
    }

    if (!WriteControlFile(false)) {
        Msg(L"Не удалось записать ac3-control.ini.", MB_ICONERROR);
        return false;
    }

    return true;
}

void CloseProcessHandles() {
    if (g_engine.hThread) CloseHandle(g_engine.hThread);
    if (g_engine.hProcess) CloseHandle(g_engine.hProcess);
    ZeroMemory(&g_engine, sizeof(g_engine));
}

bool FindAndAttachExistingEngine() {
    const std::wstring expectedPath = g_exeDir + L"\\engine.exe";

    std::wstring fullExpectedPath(32768, L'\0');
    DWORD pathLength = GetFullPathNameW(
        expectedPath.c_str(),
        static_cast<DWORD>(fullExpectedPath.size()),
        fullExpectedPath.data(),
        nullptr
    );

    if (pathLength == 0 || pathLength >= fullExpectedPath.size())
        return false;

    fullExpectedPath.resize(pathLength);

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return false;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    bool attached = false;

    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (CompareStringOrdinal(
                    entry.szExeFile, -1, L"engine.exe", -1, TRUE
                ) != CSTR_EQUAL) {
                continue;
            }

            HANDLE process = OpenProcess(
                PROCESS_QUERY_LIMITED_INFORMATION |
                SYNCHRONIZE |
                PROCESS_TERMINATE,
                FALSE,
                entry.th32ProcessID
            );

            if (!process)
                continue;

            std::wstring actualPath(32768, L'\0');
            DWORD actualLength = static_cast<DWORD>(actualPath.size());

            const BOOL queried = QueryFullProcessImageNameW(
                process, 0, actualPath.data(), &actualLength
            );

            if (queried) {
                actualPath.resize(actualLength);

                if (CompareStringOrdinal(
                        actualPath.c_str(), -1,
                        fullExpectedPath.c_str(), -1, TRUE
                    ) == CSTR_EQUAL &&
                    WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {

                    g_engine.hProcess = process;
                    g_engine.hThread = nullptr;
                    g_engine.dwProcessId = entry.th32ProcessID;
                    g_engine.dwThreadId = 0;
                    attached = true;
                    break;
                }
            }

            CloseHandle(process);
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return attached;
}

// engine.exe processes that run from a DIFFERENT folder (e.g. an older build). Two engines cannot
// share the exclusive S/PDIF output, so we offer to close them before starting ours.
std::vector<DWORD> FindForeignEngineProcesses() {
    std::vector<DWORD> pids;
    std::wstring expected(32768, L'\0');
    const DWORD expectedLen = GetFullPathNameW((g_exeDir + L"\\engine.exe").c_str(),
                                               static_cast<DWORD>(expected.size()), expected.data(), nullptr);
    if (expectedLen == 0 || expectedLen >= expected.size()) return pids;
    expected.resize(expectedLen);

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return pids;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (CompareStringOrdinal(entry.szExeFile, -1, L"engine.exe", -1, TRUE) != CSTR_EQUAL) continue;
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
            if (!process) continue;
            std::wstring actual(32768, L'\0');
            DWORD actualLen = static_cast<DWORD>(actual.size());
            if (QueryFullProcessImageNameW(process, 0, actual.data(), &actualLen)) {
                actual.resize(actualLen);
                if (CompareStringOrdinal(actual.c_str(), -1, expected.c_str(), -1, TRUE) != CSTR_EQUAL)
                    pids.push_back(entry.th32ProcessID);
            }
            CloseHandle(process);
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return pids;
}

void TerminateProcesses(const std::vector<DWORD>& pids) {
    for (DWORD pid : pids) {
        HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
        if (!process) continue;
        TerminateProcess(process, 4);
        WaitForSingleObject(process, 2000);
        CloseHandle(process);
    }
}

bool StartEngine() {
    g_engineStartTick = GetTickCount64();
    if (EngineRunning()) {
        SetStatus(L"Кодировщик уже работает.");
        return true;
    }

    CloseProcessHandles();

    if (FindAndAttachExistingEngine()) {
        SetStatus(L"Подключено к уже работающему кодировщику.");
        return true;
    }

    const std::vector<DWORD> foreignEngines = FindForeignEngineProcesses();
    if (!foreignEngines.empty()) {
        if (g_silent) {
            SetStatus(L"Работает engine.exe из другой папки (старая сборка). Закрой его или запусти панель вручную.");
            return false;
        }
        const int answer = MessageBoxW(g_window,
            L"Уже работает engine.exe из другой папки (старая сборка) и занимает цифровой выход. "
            L"Закрыть его и запустить кодировщик из этой папки?",
            L"AC3 Control Center", MB_YESNO | MB_ICONQUESTION);
        if (answer != IDYES) {
            SetStatus(L"Запуск отменён: работает engine.exe из другой папки.");
            return false;
        }
        TerminateProcesses(foreignEngines);
    }

    if (!SaveSelectedConfiguration()) return false;
    const std::wstring enginePath = g_exeDir + L"\\engine.exe";
    const DWORD attrs = GetFileAttributesW(enginePath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        Msg(L"Не найден engine.exe рядом с Control Center. Сначала собери проект CMake.", MB_ICONERROR);
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
        const DWORD err = GetLastError();
        std::wstring message = L"Не удалось запустить engine.exe. Код Windows: " + std::to_wstring(err);
        Msg(message.c_str(), MB_ICONERROR);
        return false;
    }
    g_engine = pi;
    CloseHandle(g_engine.hThread);
    g_engine.hThread = nullptr;
    SetStatus(L"Запуск кодировщика… подробности: engine.log");
    return true;
}

void StopEngine(bool updateStatus = true) {
    if (!g_engine.hProcess && !FindAndAttachExistingEngine()) {
        WriteControlFile(false);
        if (updateStatus) SetStatus(L"Работающий кодировщик не найден.");
        return;
    }
    if (EngineRunning()) {
        WriteControlFile(true);
        DWORD wait = WaitForSingleObject(g_engine.hProcess, 3000);
        if (wait == WAIT_TIMEOUT) {
            TerminateProcess(g_engine.hProcess, 3);
            WaitForSingleObject(g_engine.hProcess, 1500);
            if (updateStatus) SetStatus(L"Движок не ответил на штатную остановку; процесс завершён принудительно.");
        }
    }
    CloseProcessHandles();
    WriteControlFile(false);
    if (updateStatus) SetStatus(L"Кодировщик остановлен.");
}

void ApplyConfiguration() {
    if (!SaveSelectedConfiguration()) return;
    if (EngineRunning()) {
        const int answer = MessageBoxW(g_window,
            L"Изменение режима или устройств требует перезапуска кодировщика. Звук кратковременно прервётся. Перезапустить сейчас?",
            L"Применить настройки", MB_YESNO | MB_ICONQUESTION);
        if (answer != IDYES) {
            SetStatus(L"Настройки сохранены; перезапуск не выполнен.");
            return;
        }
        StopEngine(false);
        if (StartEngine()) SetStatus(L"Настройки применены, кодировщик перезапущен.");
    } else {
        SetStatus(L"Настройки сохранены. Нажми «Запустить» для применения.");
    }
}

void UpdateModeControls() {
    if (!g_modeCombo) return;

    const LRESULT mode = SendMessageW(g_modeCombo, CB_GETCURSEL, 0, 0);
    const bool pcmMode = mode == 2;

    for (size_t i = 0; i < g_sliders.size(); ++i)
        EnableWindow(g_sliders[i], !pcmMode || i < 2);
    if (g_loopbackUpmixCombo) EnableWindow(g_loopbackUpmixCombo, mode == 4 || mode == 5 || mode == 6);
    if (g_upmixDelayCombo) EnableWindow(g_upmixDelayCombo, mode != 0 && mode != 2);  // modes with a stereo upmix

    if (mode == 2) {
        SetStatus(L"PCM Stereo 2.0: используются только левый и правый каналы.");
    } else if (mode == 4) {
        SetStatus(L"PCM 5.1 -> DTS 5.1 Core: Loopback. Настоящий 5.1 идёт как есть, стерео (браузер, плеер) раскладывается в 5.1 (см. «Стерео в 5.1»).");
    } else if (mode == 6) {
        SetStatus(L"DTS 5.1 (бит-стрим) -> Dolby Digital 5.1: Loopback. DTS из плеера декодируется и кодируется в Dolby Digital, обычный PCM 5.1 идёт как в режиме «PCM 5.1 -> Dolby Digital».");
    } else if (mode == 5) {
        SetStatus(L"PCM 5.1 -> Dolby Digital 5.1: Loopback. Настоящий 5.1 идёт как есть, стерео (браузер, плеер) раскладывается в 5.1 (см. «Стерео в 5.1»).");
    }
}

void ResetGains() {
    g_initializing = true;
    for (size_t i = 0; i < g_sliders.size(); ++i) {
        g_gainDb[i] = 0;
        SendMessageW(g_sliders[i], TBM_SETPOS, TRUE, 0);
        UpdateGainLabel(i);
    }
    g_initializing = false;
    WriteControlFile(false);
    SetStatus(EngineRunning() ? L"Все уровни сброшены на 0 dB и применены." : L"Уровни сброшены на 0 dB.");
}

bool g_uiOnly = false;

void CheckEngineStatus() {
    if (!g_engine.hProcess) return;
    DWORD code = 0;
    if (GetExitCodeProcess(g_engine.hProcess, &code) && code != STILL_ACTIVE) {
        CloseProcessHandles();
        SetStatus(L"Движок остановлен (код завершения " + std::to_wstring(code) + L"). Подробности: engine.log");
    }
}

// ---------------------------------------------------------------------------
// Tray icon, autostart (Startup-folder shortcut), engine supervision
// ---------------------------------------------------------------------------
HICON MakeDotIcon(COLORREF color) {
    const int size = std::max(16, GetSystemMetrics(SM_CXSMICON));
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP colorBitmap = CreateCompatibleBitmap(screen, size, size);
    HBITMAP maskBitmap = CreateBitmap(size, size, 1, 1, nullptr);
    ReleaseDC(nullptr, screen);

    HICON icon = nullptr;
    if (dc && colorBitmap && maskBitmap) {
        HGDIOBJ originalBitmap = SelectObject(dc, colorBitmap);
        PatBlt(dc, 0, 0, size, size, BLACKNESS);
        HBRUSH fill = CreateSolidBrush(color);
        HPEN outline = CreatePen(PS_SOLID, 1, RGB(30, 30, 30));
        HGDIOBJ originalBrush = SelectObject(dc, fill);
        HGDIOBJ originalPen = SelectObject(dc, outline);
        Ellipse(dc, 1, 1, size - 1, size - 1);
        SelectObject(dc, originalBrush);
        SelectObject(dc, originalPen);
        DeleteObject(fill);
        DeleteObject(outline);

        SelectObject(dc, maskBitmap);                 // mask: 1 = transparent, 0 = opaque
        PatBlt(dc, 0, 0, size, size, WHITENESS);
        SelectObject(dc, GetStockObject(BLACK_BRUSH));
        SelectObject(dc, GetStockObject(BLACK_PEN));
        Ellipse(dc, 1, 1, size - 1, size - 1);
        SelectObject(dc, originalBitmap);

        ICONINFO info{};
        info.fIcon = TRUE;
        info.hbmMask = maskBitmap;
        info.hbmColor = colorBitmap;
        icon = CreateIconIndirect(&info);
    }
    if (colorBitmap) DeleteObject(colorBitmap);
    if (maskBitmap) DeleteObject(maskBitmap);
    if (dc) DeleteDC(dc);
    return icon ? icon : LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
}

// Adds the tray icon, or updates it when the engine state (running / stopped) changed.
void UpdateTray(bool force = false) {
    if (!g_window || g_uiOnly) return;
    const int state = EngineRunning() ? 1 : 0;
    if (!force && g_trayAdded && state == g_trayState) return;
    if (!g_iconRunning) g_iconRunning = MakeDotIcon(RGB(46, 184, 76));
    if (!g_iconStopped) g_iconStopped = MakeDotIcon(RGB(205, 60, 60));

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_window;
    nid.uID = TRAY_UID;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = state ? g_iconRunning : g_iconStopped;
    wcsncpy_s(nid.szTip,
              state ? L"AC3 / DTS Control Center: кодировщик работает"
                    : L"AC3 / DTS Control Center: кодировщик остановлен",
              _TRUNCATE);
    if (!g_trayAdded)
        g_trayAdded = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    else
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    g_trayState = state;
}

void RemoveTray() {
    if (!g_trayAdded || !g_window) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_window;
    nid.uID = TRAY_UID;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_trayAdded = false;
}

void ShowPanel() {
    if (!g_window) return;
    ShowWindow(g_window, IsIconic(g_window) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(g_window);
}

std::wstring StartupFolder() {
    wchar_t folder[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_STARTUP, nullptr, SHGFP_TYPE_CURRENT, folder))) return L"";
    return folder;
}

std::wstring StartupLinkPath() {
    const std::wstring folder = StartupFolder();
    return folder.empty() ? L"" : folder + L"\\" + kStartupLinkName;
}

bool AutostartEnabled() {
    const std::wstring link = StartupLinkPath();
    return !link.empty() && GetFileAttributesW(link.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring ShortcutTarget(const std::wstring& linkPath) {
    std::wstring target;
    IShellLinkW* shell = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(ShellLink), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shell))) || !shell)
        return target;
    IPersistFile* file = nullptr;
    if (SUCCEEDED(shell->QueryInterface(IID_PPV_ARGS(&file))) && file) {
        if (SUCCEEDED(file->Load(linkPath.c_str(), STGM_READ))) {
            wchar_t path[MAX_PATH * 2] = {};
            if (SUCCEEDED(shell->GetPath(path, static_cast<int>(std::size(path)), nullptr, SLGP_RAWPATH)))
                target = path;
        }
        file->Release();
    }
    shell->Release();
    return target;
}

// Other Startup shortcuts that start a (different) control_center.exe, e.g. an older build.
std::vector<std::wstring> FindOtherPanelShortcuts() {
    std::vector<std::wstring> found;
    const std::wstring folder = StartupFolder();
    const std::wstring ourLink = StartupLinkPath();
    if (folder.empty()) return found;

    WIN32_FIND_DATAW data{};
    HANDLE search = FindFirstFileW((folder + L"\\*.lnk").c_str(), &data);
    if (search == INVALID_HANDLE_VALUE) return found;
    const std::wstring suffix = L"control_center.exe";
    do {
        const std::wstring full = folder + L"\\" + data.cFileName;
        if (CompareStringOrdinal(full.c_str(), -1, ourLink.c_str(), -1, TRUE) == CSTR_EQUAL) continue;
        std::wstring target = ShortcutTarget(full);
        std::transform(target.begin(), target.end(), target.begin(),
                       [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        if (target.size() >= suffix.size() &&
            target.compare(target.size() - suffix.size(), suffix.size(), suffix) == 0)
            found.push_back(full);
    } while (FindNextFileW(search, &data));
    FindClose(search);
    return found;
}

bool SetAutostart(bool enable) {
    const std::wstring link = StartupLinkPath();
    if (link.empty()) return false;
    if (!enable)
        return DeleteFileW(link.c_str()) != FALSE || GetLastError() == ERROR_FILE_NOT_FOUND;

    std::wstring exePath(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, exePath.data(), static_cast<DWORD>(exePath.size()));
    if (n == 0 || n >= exePath.size()) return false;
    exePath.resize(n);

    IShellLinkW* shell = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(ShellLink), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shell))) || !shell)
        return false;
    shell->SetPath(exePath.c_str());
    shell->SetArguments(L"--minimized");
    shell->SetWorkingDirectory(g_exeDir.c_str());
    shell->SetDescription(L"AC3 / DTS Control Center (стартует свёрнутым в трей)");
    HRESULT hr = E_FAIL;
    IPersistFile* file = nullptr;
    if (SUCCEEDED(shell->QueryInterface(IID_PPV_ARGS(&file))) && file) {
        hr = file->Save(link.c_str(), TRUE);
        file->Release();
    }
    shell->Release();
    return SUCCEEDED(hr);
}

void ToggleAutostart(bool enable) {
    if (!SetAutostart(enable)) {
        Msg(L"Не удалось изменить автозапуск (папка Startup).", MB_ICONERROR);
    } else if (enable && !g_silent) {
        const std::vector<std::wstring> others = FindOtherPanelShortcuts();
        if (!others.empty()) {
            std::wstring text = L"В автозагрузке найден другой ярлык панели управления:\n\n";
            for (const auto& path : others) {
                const size_t slash = path.find_last_of(L"\\/");
                text += L"  • " + (slash == std::wstring::npos ? path : path.substr(slash + 1)) + L"\n";
            }
            text += L"\nУдалить его, чтобы при входе в Windows не запускались две панели и два кодировщика?";
            if (MessageBoxW(g_window, text.c_str(), L"AC3 Control Center", MB_YESNO | MB_ICONQUESTION) == IDYES)
                for (const auto& path : others) DeleteFileW(path.c_str());
        }
    }
    if (g_autostartCheck)
        SendMessageW(g_autostartCheck, BM_SETCHECK, AutostartEnabled() ? BST_CHECKED : BST_UNCHECKED, 0);
}

bool UserStartEngine() {
    g_userStopped = false;
    g_restartAttempts = 0;
    g_nextRestartTick = 0;
    const bool ok = StartEngine();
    UpdateTray();
    return ok;
}

void UserStopEngine() {
    g_userStopped = true;
    StopEngine();
    UpdateTray();
}

// If the engine is not running and the user did not stop it (e.g. the audio devices were not
// ready yet at logon, or it crashed), start it again every 5 s, at most kMaxAutoRestarts times.
void SuperviseEngine() {
    static bool announcedGiveUp = false;
    if (g_uiOnly || g_userStopped) return;
    if (EngineRunning()) {
        if (GetTickCount64() - g_engineStartTick > 30000) g_restartAttempts = 0;
        g_nextRestartTick = 0;
        announcedGiveUp = false;
        return;
    }
    if (g_restartAttempts >= kMaxAutoRestarts) {
        if (!announcedGiveUp) {
            announcedGiveUp = true;
            SetStatus(L"Автоперезапуск остановлен после " + std::to_wstring(kMaxAutoRestarts) +
                      L" попыток. Проверь engine.log и нажми «Запустить кодировщик».");
        }
        return;
    }
    const ULONGLONG now = GetTickCount64();
    if (g_nextRestartTick == 0) {
        g_nextRestartTick = now + 5000;
        return;
    }
    if (now < g_nextRestartTick) return;
    g_nextRestartTick = 0;
    ++g_restartAttempts;
    SetStatus(L"Кодировщик не работает — автоперезапуск (попытка " + std::to_wstring(g_restartAttempts) +
              L" из " + std::to_wstring(kMaxAutoRestarts) + L")…");
    const bool previousSilent = g_silent;
    g_silent = true;
    StartEngine();
    g_silent = previousSilent;
}

void ShowTrayMenu() {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    const bool running = EngineRunning();
    const UINT noStart = (running || g_uiOnly) ? MF_GRAYED : 0;
    const UINT noStop = (!running || g_uiOnly) ? MF_GRAYED : 0;
    AppendMenuW(menu, MF_STRING, ID_TRAY_SHOW, L"Показать панель");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | noStart, ID_TRAY_START, L"Запустить кодировщик");
    AppendMenuW(menu, MF_STRING | noStop, ID_TRAY_STOP, L"Остановить кодировщик");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (AutostartEnabled() ? MF_CHECKED : 0u), ID_TRAY_AUTOSTART,
                L"Запускать вместе с Windows");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_TRAY_EXIT, L"Выход (кодировщик продолжит работать)");
    SetMenuDefaultItem(menu, ID_TRAY_SHOW, FALSE);

    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(g_window);  // required, otherwise the menu does not close on outside click
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, cursor.x, cursor.y, 0, g_window, nullptr);
    PostMessageW(g_window, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

// Exit the panel only. The engine is a separate process and keeps running.
void ExitPanel() {
    RemoveTray();
    CloseProcessHandles();
    DestroyWindow(g_window);
}

// ---------------------------------------------------------------------------
// control-center.ini (next to the exe): interface options that can be edited by hand.
// Re-read every ~2 s, so a change takes effect without restarting the panel.
// ---------------------------------------------------------------------------
std::wstring UiIniPath() { return g_exeDir + L"\\control-center.ini"; }

void EnsureUiIni() {
    const std::wstring path = UiIniPath();
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    // UTF-16 LE with BOM: the Windows INI API then reads the Cyrillic comments correctly.
    static const wchar_t text[] =
        L"\xFEFF; AC3 / DTS Control Center: настройки интерфейса.\r\n"
        L"; Изменения подхватываются без перезапуска панели (раз в пару секунд).\r\n"
        L"[ui]\r\n"
        L"; 1 = показывать выпадающее меню \"Задержка апмикса\", 0 = скрыть.\r\n"
        L"; Скрытое меню ничего не сбрасывает: выбранное значение остаётся в virtual-ac3-encoder.conf (upmix_win_size).\r\n"
        L"show_upmix_latency_menu=1\r\n";
    DWORD written = 0;
    WriteFile(file, text, static_cast<DWORD>((sizeof(text) / sizeof(wchar_t) - 1) * sizeof(wchar_t)), &written, nullptr);
    CloseHandle(file);
}

void ApplyUiIni() {
    const std::wstring path = UiIniPath();
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return;
    if (CompareFileTime(&data.ftLastWriteTime, &g_uiIniWriteTime) == 0) return;
    g_uiIniWriteTime = data.ftLastWriteTime;
    const bool showDelay = GetPrivateProfileIntW(L"ui", L"show_upmix_latency_menu", 1, path.c_str()) != 0;
    const int command = showDelay ? SW_SHOW : SW_HIDE;
    if (g_upmixDelayLabel) ShowWindow(g_upmixDelayLabel, command);
    if (g_upmixDelayCombo) ShowWindow(g_upmixDelayCombo, command);
}

HWND AddControl(const wchar_t* cls, const wchar_t* text, DWORD style,
                int x, int y, int w, int h, int id, HWND parent) {
    return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                           x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                           GetModuleHandleW(nullptr), nullptr);
}

void CreateControls(HWND hwnd) {
    AddControl(L"STATIC", L"Входное устройство:", 0, 20, 18, 260, 24, 0, hwnd);
    g_inputCombo = AddControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 290, 16, 560, 240, ID_INPUT, hwnd);
    PopulateDeviceCombo(g_inputCombo, g_captureDevices);

    AddControl(L"STATIC", L"Цифровой выход (S/PDIF / Toslink):", 0, 20, 55, 260, 24, 0, hwnd);
    g_outputCombo = AddControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 290, 52, 560, 240, ID_OUTPUT, hwnd);
    PopulateDeviceCombo(g_outputCombo, g_renderDevices);

    AddControl(L"STATIC", L"Формат цифрового выхода:", 0, 20, 93, 230, 24, 0, hwnd);
    g_modeCombo = AddControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, 290, 89, 270, 220, ID_MODE, hwnd);
    SendMessageW(g_modeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Dolby Digital 5.1 — без upmix"));
    SendMessageW(g_modeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Dolby Digital 5.1 — Surround upmix"));
    SendMessageW(g_modeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"PCM Stereo 2.0 — обычный S/PDIF"));
    SendMessageW(g_modeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"DTS 5.1 Core - Toslink / S/PDIF"));
    SendMessageW(g_modeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"PCM 5.1 -> DTS 5.1 Core"));
    SendMessageW(g_modeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"PCM 5.1 -> Dolby Digital 5.1"));
    SendMessageW(g_modeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"DTS 5.1 (бит-стрим) -> Dolby Digital 5.1"));
    AddControl(L"BUTTON", L"Применить / перезапустить", BS_PUSHBUTTON | WS_TABSTOP, 575, 88, 275, 30, ID_APPLY, hwnd);

    AddControl(L"STATIC", L"Громкость каналов: −60 dB = MUTE, 0 dB = максимум (запас −6 dB для Dolby/DTS уже заложен). На лету.", 0, 20, 130, 830, 24, 0, hwnd);
    for (size_t i = 0; i < g_sliders.size(); ++i) {
        const int y = 166 + static_cast<int>(i) * 45;
        AddControl(L"STATIC", kChannelNames[i], 0, 20, y + 3, 255, 24, 0, hwnd);
        g_sliders[i] = AddControl(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS | WS_TABSTOP,
                                  290, y, 460, 32, ID_SLIDER_BASE + static_cast<int>(i), hwnd);
        SendMessageW(g_sliders[i], TBM_SETRANGE, TRUE, MAKELPARAM(-60, 0));
        SendMessageW(g_sliders[i], TBM_SETTICFREQ, 10, 0);
        SendMessageW(g_sliders[i], TBM_SETPOS, TRUE, g_gainDb[i]);
        g_values[i] = AddControl(L"STATIC", L"0 dB", SS_RIGHT, 765, y + 5, 80, 22, ID_VALUE_BASE + static_cast<int>(i), hwnd);
        UpdateGainLabel(i);
    }
    AddControl(L"BUTTON", L"Сбросить все уровни", BS_PUSHBUTTON | WS_TABSTOP, 290, 445, 190, 32, ID_RESET, hwnd);
    AddControl(L"STATIC", L"Стерео в 5.1 (режимы PCM 5.1):", 0, 495, 452, 215, 22, 0, hwnd);
    g_loopbackUpmixCombo = AddControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, 715, 447, 135, 120,
                                      ID_LOOPBACK_UPMIX, hwnd);
    SendMessageW(g_loopbackUpmixCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Авто"));
    SendMessageW(g_loopbackUpmixCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Всегда"));
    SendMessageW(g_loopbackUpmixCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Выкл"));
    // Upmix delay (FFT window of the stereo -> 5.1 filter). Shown/hidden via control-center.ini.
    g_upmixDelayLabel = AddControl(L"STATIC", L"Задержка апмикса:", 0, 495, 494, 150, 22, 0, hwnd);
    g_upmixDelayCombo = AddControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, 650, 489, 200, 120,
                                   ID_UPMIX_DELAY, hwnd);
    SendMessageW(g_upmixDelayCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Качество (~87 мс)"));
    SendMessageW(g_upmixDelayCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Баланс (~44 мс)"));
    SendMessageW(g_upmixDelayCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Мин. задержка (~22 мс)"));
    EnsureUiIni();
    ApplyUiIni();
    g_startButton = AddControl(L"BUTTON", L"Запустить кодировщик", BS_PUSHBUTTON | WS_TABSTOP, 20, 531, 220, 36, ID_START, hwnd);
    g_stopButton = AddControl(L"BUTTON", L"Остановить", BS_PUSHBUTTON | WS_TABSTOP, 255, 531, 180, 36, ID_STOP, hwnd);
    g_autostartCheck = AddControl(L"BUTTON", L"Запускать панель вместе с Windows (в трее)",
                                  BS_AUTOCHECKBOX | WS_TABSTOP, 455, 539, 395, 24, ID_AUTOSTART, hwnd);
    SendMessageW(g_autostartCheck, BM_SETCHECK, AutostartEnabled() ? BST_CHECKED : BST_UNCHECKED, 0);
    g_status = AddControl(L"STATIC", L"Готов. Кодировщик остановлен.", 0, 20, 581, 830, 38, 0, hwnd);
    AddControl(L"STATIC", L"Закрытие окна сворачивает панель в трей, кодировщик продолжает работать. "
                          L"Выход: правая кнопка по значку в трее.", 0, 20, 628, 850, 22, 0, hwnd);

    Kv cfg = ReadKv(g_configPath);
    int outIndex = FindById(g_renderDevices, cfg["out_id"]);
    if (outIndex < 0) outIndex = FindBySubstring(g_renderDevices, cfg["out"]);
    if (outIndex < 0) outIndex = PreferredOutput();
    if (outIndex >= 0) SendMessageW(g_outputCombo, CB_SETCURSEL, outIndex, 0);
    const LRESULT selectedMode = cfg["output_mode"] == "pcm" ? 2 :
        cfg["output_mode"] == "dts" ? (cfg["loopback"] == "1" ? 4 : 3) :
        cfg["loopback"] == "1" ? (cfg["dts_in"] == "1" ? 6 : 5) :
        (cfg["upmix"] == "off" ? 0 : 1);
    SendMessageW(g_modeCombo, CB_SETCURSEL, selectedMode, 0);
    SendMessageW(g_loopbackUpmixCombo, CB_SETCURSEL,
                 cfg["loopback_upmix"] == "always" ? 1 : cfg["loopback_upmix"] == "off" ? 2 : 0, 0);
    SendMessageW(g_upmixDelayCombo, CB_SETCURSEL,
                 cfg["upmix_win_size"] == "2048" ? 1 : cfg["upmix_win_size"] == "1024" ? 2 : 0, 0);
    UpdateInputDeviceComboForMode(static_cast<int>(selectedMode), cfg["in_id"], false);
    UpdateModeControls();
    EnableWindow(g_startButton, !g_captureDevices.empty() && !g_renderDevices.empty());
    EnableWindow(g_stopButton, FALSE);
    SetTimer(hwnd, TIMER_ID, 500, nullptr);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (g_taskbarCreatedMsg != 0 && message == g_taskbarCreatedMsg) {  // Explorer restarted
        g_trayAdded = false;
        UpdateTray(true);
        return 0;
    }
    switch (message) {
    case WM_CREATE:
        CreateControls(hwnd);
        return 0;
    case WM_COMMAND: {
        const int id = LOWORD(wParam);

        if (g_uiOnly &&
            (id == ID_START || id == ID_STOP ||
             id == ID_APPLY || id == ID_RESET))
            return 0;
        if (id == ID_AUTOSTART && HIWORD(wParam) == BN_CLICKED) {
            ToggleAutostart(SendMessageW(g_autostartCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
            return 0;
        }
        if (id == ID_TRAY_SHOW) {
            ShowPanel();
            return 0;
        }
        if (id == ID_TRAY_START) {
            if (!g_uiOnly) UserStartEngine();
            return 0;
        }
        if (id == ID_TRAY_STOP) {
            if (!g_uiOnly) UserStopEngine();
            return 0;
        }
        if (id == ID_TRAY_AUTOSTART) {
            ToggleAutostart(!AutostartEnabled());
            return 0;
        }
        if (id == ID_TRAY_EXIT) {
            ExitPanel();
            return 0;
        }
        if (id == ID_MODE && HIWORD(wParam) == CBN_SELCHANGE) {
            const LRESULT mode = SendMessageW(g_modeCombo, CB_GETCURSEL, 0, 0);
            Kv currentCfg = ReadKv(g_configPath);
            UpdateInputDeviceComboForMode(static_cast<int>(mode), currentCfg["in_id"], true);
            UpdateModeControls();
            if (mode == 2)
                SetStatus(L"Выбран PCM Stereo 2.0. Нажми «Применить / перезапустить» для переключения формата.");
            else if (mode == 4)
                SetStatus(L"PCM 5.1 -> DTS 5.1 Core: Loopback, без апмикса. Нажми «Применить / перезапустить».");
            else if (mode == 6)
                SetStatus(L"DTS 5.1 (бит-стрим) -> Dolby Digital 5.1: Loopback. Нажми «Применить / перезапустить».");
            else if (mode == 5)
                SetStatus(L"PCM 5.1 -> Dolby Digital 5.1: Loopback, без апмикса. Нажми «Применить / перезапустить».");
            else if (mode == 3)
                SetStatus(L"DTS 5.1 Core выбран. Нажми Применить / перезапустить для переключения формата.");
            else
                SetStatus(L"Выбран выход Dolby Digital 5.1. Нажми «Применить / перезапустить» для переключения формата.");
            return 0;
        }
        if (id == ID_START && HIWORD(wParam) == BN_CLICKED) {
            if (UserStartEngine()) {
                EnableWindow(g_startButton, FALSE);
                EnableWindow(g_stopButton, TRUE);
            }
            return 0;
        }
        if (id == ID_STOP && HIWORD(wParam) == BN_CLICKED) {
            UserStopEngine();
            EnableWindow(g_startButton, TRUE);
            EnableWindow(g_stopButton, FALSE);
            return 0;
        }
        if (id == ID_APPLY && HIWORD(wParam) == BN_CLICKED) {
            ApplyConfiguration();
            const bool running = EngineRunning();
            EnableWindow(g_startButton, !running);
            EnableWindow(g_stopButton, running);
            return 0;
        }
        if (id == ID_RESET && HIWORD(wParam) == BN_CLICKED) {
            ResetGains();
            return 0;
        }
        return 0;
    }
    case WM_HSCROLL: {
        if (g_uiOnly) return 0;
        if (g_initializing) return 0;
        HWND slider = reinterpret_cast<HWND>(lParam);
        for (size_t i = 0; i < g_sliders.size(); ++i) {
            if (slider == g_sliders[i]) {
                g_gainDb[i] = static_cast<int>(SendMessageW(g_sliders[i], TBM_GETPOS, 0, 0));
                UpdateGainLabel(i);
                WriteControlFile(false);
                SetStatus(EngineRunning() ? L"Уровни каналов применяются в реальном времени." : L"Уровни сохранены; будут использованы при запуске.");
                break;
            }
        }
        return 0;
    }
    case WM_TIMER:
        CheckEngineStatus();
        SuperviseEngine();
        UpdateTray();
        {
            static int iniTick = 0;
            if (++iniTick % 4 == 0) ApplyUiIni();  // control-center.ini is polled every ~2 s
        }
        if (g_startButton && g_stopButton) {
            const bool running = EngineRunning();
            EnableWindow(g_startButton, !running);
            EnableWindow(g_stopButton, running);
        }
        return 0;
    case WM_CLOSE:
        // Closing the window only hides it in the tray; the engine and the panel keep running.
        // (UI-only preview mode has no tray, so it really closes.)
        if (g_uiOnly || !g_trayAdded) {
            CloseProcessHandles();
            DestroyWindow(hwnd);
        } else {
            ShowWindow(hwnd, SW_HIDE);
        }
        return 0;
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED && g_trayAdded) {
            ShowWindow(hwnd, SW_HIDE);  // minimize to tray
            return 0;
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    case WM_TRAYICON:
        switch (static_cast<UINT>(lParam)) {
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
            ShowPanel();
            break;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            ShowTrayMenu();
            break;
        default:
            break;
        }
        return 0;
    case WM_SHOW_PANEL:
        ShowPanel();
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, TIMER_ID);
        RemoveTray();
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);
    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninitializeCom = SUCCEEDED(comHr);

    g_uiOnly = std::wstring(GetCommandLineW()).find(L"--ui-only") != std::wstring::npos;
    const bool startHidden = std::wstring(GetCommandLineW()).find(L"--minimized") != std::wstring::npos;

    // Only one panel at a time: a second launch just brings the first one to the front.
    HANDLE instanceMutex = nullptr;
    if (!g_uiOnly) {
        instanceMutex = CreateMutexW(nullptr, FALSE, kSingleInstanceMutex);
        if (instanceMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
            HWND existing = FindWindowW(kWindowClassName, nullptr);
            if (existing && !startHidden) PostMessageW(existing, WM_SHOW_PANEL, 0, 0);
            CloseHandle(instanceMutex);
            if (uninitializeCom) CoUninitialize();
            return 0;
        }
    }

    g_exeDir = ExeDirectory();
    g_configPath = g_exeDir + L"\\virtual-ac3-encoder.conf";
    g_controlPath = g_exeDir + L"\\ac3-control.ini";
    g_logPath = g_exeDir + L"\\engine.log";
    g_captureDevices = EnumerateDevices(eCapture);
    g_renderDevices = EnumerateDevices(eRender);
    ReadGainState();

    const wchar_t* const CLASS_NAME = kWindowClassName;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = CLASS_NAME;
    if (!RegisterClassExW(&wc)) {
        if (uninitializeCom) CoUninitialize();
        return 1;
    }
    g_window = CreateWindowExW(0, CLASS_NAME, L"AC3 / DTS Control Center  [v2 stage 3.5]",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 900, 726, nullptr, nullptr, instance, nullptr);
    if (!g_window) {
        if (uninitializeCom) CoUninitialize();
        return 1;
    }
    g_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");
    if (!startHidden || g_uiOnly) {  // --minimized (autostart): stay hidden, only the tray icon
        ShowWindow(g_window, showCommand);
        UpdateWindow(g_window);
    }
    if (g_uiOnly) {
        EnableWindow(g_startButton, FALSE);
        EnableWindow(g_stopButton, FALSE);
        EnableWindow(GetDlgItem(g_window, ID_APPLY), FALSE);
        EnableWindow(GetDlgItem(g_window, ID_RESET), FALSE);
        EnableWindow(g_inputCombo, FALSE);
        EnableWindow(g_outputCombo, FALSE);
        EnableWindow(g_autostartCheck, FALSE);
        EnableWindow(g_loopbackUpmixCombo, FALSE);

        for (HWND slider : g_sliders) {
            EnableWindow(slider, FALSE);
        }

        SetStatus(L"UI-only: просмотр режимов; движок и настройки не изменяются.");
    } else {
        g_silent = startHidden;  // at logon: no modal dialogs; problems go to the status line
        if (StartEngine()) {
            EnableWindow(g_startButton, FALSE);
            EnableWindow(g_stopButton, TRUE);
        }
        g_silent = false;
        UpdateTray(true);
    }
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (instanceMutex) CloseHandle(instanceMutex);
    if (uninitializeCom) CoUninitialize();
    return static_cast<int>(msg.wParam);
}
