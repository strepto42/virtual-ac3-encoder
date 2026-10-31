// Config.h — engine runtime configuration (CLI-driven for now; file-based in Phase 4).
#pragma once

#include <string>

struct Config
{
  bool listDevices = false;
  bool probe = false;    // probe all render endpoints for AC3 passthrough support, then exit
  bool loopback = false; // capture the input as a RENDER endpoint via WASAPI loopback
                         // (the render-only virtual-driver architecture)
  bool monitor = false;  // capture-only diagnostic: report input throughput, then exit
  int  monitorSeconds = 5;
  int  durationSeconds = 0; // 0 = run until Ctrl+C; otherwise auto-stop after N seconds

  // Capture (input) endpoint — the virtual cable's recording side.
  std::wstring inName = L"CABLE Output"; // friendly-name substring (VB-CABLE default)
  std::wstring inId;                     // exact endpoint id (overrides inName)

  // Output endpoint — the optical / Toslink device.
  std::wstring outName;  // friendly-name substring
  std::wstring outId;    // exact endpoint id (overrides outName / auto)
  bool outAutoSpdif = false; // pick the first render endpoint with SPDIF form factor

  int64_t  bitRate = 640000;
  uint32_t safeFrames = 1536;
  bool     customSafeFrames = false;
  std::string latencyMode = "balanced"; // "extreme", "fast", "balanced", "safe", "relaxed"

  uint32_t GetEffectiveSafeFrames() const
  {
    if (customSafeFrames && safeFrames > 0)
      return safeFrames;

    const bool dts = (outputMode == "dts");
    if (dts)
    {
      // DTS frames are 512 samples. WASAPI shared-mode capture operates on a 480-sample (10 ms) quantum.
      // Generous buffer reserve floors absorb packet boundary jitter and OS thread scheduling latency.
      if (latencyMode == "extreme" || latencyMode == "ultra")
        return 512;
      if (latencyMode == "fast" || latencyMode == "low")
        return 768;
      if (latencyMode == "balanced")
        return 1024;
      if (latencyMode == "safe")
        return 1536;
      return 2048;
    }

    if (latencyMode == "extreme" || latencyMode == "ultra")
      return 256;
    if (latencyMode == "fast" || latencyMode == "low")
      return 512;
    if (latencyMode == "balanced")
      return 768;
    if (latencyMode == "safe")
      return 1024;
    // "relaxed", "normal", or default
    return 1536;
  }

  // Stereo->5.1 upmix mode for <=2ch input: "off" (swr default) or "surround"
  // (FFmpeg `surround` FFT upmix). Multichannel input is always downmixed to 5.1.
  // Default "surround": this tool targets a 5.1 optical path, so stereo sources use all speakers.
  // Physical output format: "ac3" (Dolby Digital), "dts" (DTS Core), or "pcm" (stereo PCM).
  std::string outputMode = "ac3";
  int64_t dtsBitRate = 1536000; // DTS Core: 1536000 (full-rate 1.5 Mbps) or 768000 (half-rate)
  std::string upmix = "surround";
};
