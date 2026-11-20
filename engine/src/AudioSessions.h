// AudioSessions.h — which applications are making sound right now (for the auto profiles).
//
// Every application that plays through a render endpoint has an "audio session" on it. For the endpoint the
// engine captures from (loopback) we list the sessions that are active and not silent and return the exe
// names of their processes. Cheap enough to call twice a second.
#pragma once

#include "ComUtil.h"

#include <audiopolicy.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>

#include <cwchar>
#include <string>
#include <vector>

class AudioSessionWatcher
{
public:
  bool Init(IMMDevice* device)
  {
    mgr_.Reset();
    if (!device)
      return false;
    const HRESULT hr = device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                        reinterpret_cast<void**>(mgr_.GetAddressOf()));
    if (FAILED(hr))
    {
      std::printf("[auto] audio sessions unavailable on this device (%s): auto profiles will not work\n", HrStr(hr).c_str());
      mgr_.Reset();
      return false;
    }
    return true;
  }

  // Exe names (e.g. "vlc.exe") of the processes whose session is active and louder than about -54 dBFS.
  std::vector<std::string> Poll()
  {
    std::vector<std::string> names;
    if (!mgr_)
      return names;
    ComPtr<IAudioSessionEnumerator> sessions;
    if (FAILED(mgr_->GetSessionEnumerator(sessions.GetAddressOf())))
      return names;
    int count = 0;
    if (FAILED(sessions->GetCount(&count)))
      return names;
    for (int i = 0; i < count; ++i)
    {
      ComPtr<IAudioSessionControl> control;
      if (FAILED(sessions->GetSession(i, control.GetAddressOf())))
        continue;
      AudioSessionState state = AudioSessionStateInactive;
      if (FAILED(control->GetState(&state)) || state != AudioSessionStateActive)
        continue;
      ComPtr<IAudioSessionControl2> control2;
      if (FAILED(control.As(&control2)) || control2->IsSystemSoundsSession() == S_OK)
        continue;
      DWORD pid = 0;
      if (FAILED(control2->GetProcessId(&pid)) || pid == 0)
        continue;
      float peak = 0.0f;
      ComPtr<IAudioMeterInformation> meter;
      if (SUCCEEDED(control.As(&meter)))
        meter->GetPeakValue(&peak);
      if (peak < 0.002f)
        continue;
      const std::string name = ExeName(pid);
      if (!name.empty())
        names.push_back(name);
    }
    return names;
  }

private:
  static std::string ExeName(DWORD pid)
  {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
      return std::string();
    wchar_t path[MAX_PATH * 2];
    DWORD length = static_cast<DWORD>(sizeof(path) / sizeof(path[0]));
    std::string name;
    if (QueryFullProcessImageNameW(process, 0, path, &length))
    {
      const wchar_t* slash = std::wcsrchr(path, L'\\');
      name = Narrow(slash ? slash + 1 : path);
    }
    CloseHandle(process);
    return name;
  }

  ComPtr<IAudioSessionManager2> mgr_;
};
