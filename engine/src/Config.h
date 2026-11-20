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

  // Stereo->5.1 upmix mode: "off" (swr default) or "surround" (FFmpeg `surround` FFT upmix) for
  // <=2ch input. For multichannel input (e.g. VB-CABLE 5.1 loopback): "auto" = pass 5.1 through but
  // upmix FL/FR while centre/surrounds are silent (stereo-only material), "always" = always upmix
  // FL/FR, anything else = plain pass-through/downmix to 5.1. ("surround" is treated like "auto"
  // for multichannel input: it never did anything useful there.)
  // Default "surround": this tool targets a 5.1 optical path, so stereo sources use all speakers.
  // Physical output format: "ac3" (Dolby Digital), "dts" (DTS Core), or "pcm" (stereo PCM).
  std::string outputMode = "ac3";
  std::string upmix = "surround";
  // Which algorithm does the upmix (when `upmix` is not "off"): "surround" = FFmpeg FFT filter (default),
  // "matrix" = time-domain adaptive matrix (no look-ahead delay, centre only from ~150 Hz..5 kHz, tuned in
  // DSP Studio). Needs an engine restart to change.
  std::string upmixAlgo = "surround";

  // Anti-clipping for the encoded (AC3 / DTS) paths. `headroomDb` is a fixed trim applied to all
  // six channels before the encoder (-6 dB by default); on top of it a linked look-ahead peak
  // limiter holds the signal under `limiterCeilingDb` so the encoder can never clip.
  // FFT window of the stereo->5.1 upmix filter: 4096 (default, best centre extraction, ~87 ms delay),
  // 2048 (~44 ms) or 1024 (~22 ms). Smaller = less delay for lip-sync, slightly rougher steering.
  int upmixWinSize = 4096;

  // Dolby Digital output only: when the input (loopback) carries a DTS bitstream as IEC 61937 in
  // FL/FR (player "S/PDIF passthrough"), decode it and encode the 5.1 to AC3. PCM still works.
  bool dtsIn = false;

  // Diagnostics: if set, the first 20 s of the raw input and of the encoder input are written to
  // "<dump_audio>_input.wav" and "<dump_audio>_encoder_in.wav" when the engine stops.
  std::string dumpAudio;

  float headroomDb = -6.0f;        // -24 .. 0
  bool  limiter = true;
  float limiterCeilingDb = -1.0f;  // -12 .. 0 dBFS
};
