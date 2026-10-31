---
trigger: always_on
description: Core real-time WASAPI audio pipeline, FFmpeg 8 codec/filter contracts, live hot-reload IPC, and WDK driver constraints for virtual-ac3-encoder
---

# Architecture Rules

## 1. Stack & Components
- **Audio Engine (`engine/`)**: C++17, CMake + MSVC (`/W4 /permissive-`), WASAPI, FFmpeg 8 (`avcodec`, `avformat`, `avfilter`, `avutil`, `swresample`). Targets: `ac3core`, `engine`, `encode_wav`, `control_center`, `unit_tests`.
- **Control Center (`engine/src/ControlCenter.cpp`)**: Dear ImGui + DirectX 11 + Win32 (`UNICODE`), single-instance mutex, system tray, shared-memory 5.1 VU meters (`MeterSharedState.h`), and live config/gain reload (`ac3-control.ini`).
- **Kernel Driver (`driver/`)**: WDK WaveRT virtual 5.1 render-only endpoint (`SimpleAudioSample.sln`, `KSAUDIO_SPEAKER_5POINT1` 48 kHz 16-bit, `g_cCaptureEndpoints = 0`, INF target `19041+`).

## 2. Real-Time Pipeline & Clock-Drift Contract
`WasapiCapture (loopback polled OR normal event-driven) ➔ Lock-free SPSC RingBuffer ➔ SpdifEncoder (AC3 1536f / 6144B burst OR DTS 512f / 2048B burst) ➔ WasapiPassthrough (EXCLUSIVE event-driven master clock, 64-cycle drift trim) or WasapiPcmOutput`

- **Codec Frame Quanta**:
  - **AC3 (`Codec::AC3`)**: `1536` frames/packet (`6144`-byte IEC 61937 burst), `AV_SAMPLE_FMT_FLTP`, `640k` / `448k` bps, `48000`/`44100`/`32000` Hz.
  - **DTS (`Codec::DTS`)**: `512` frames/packet (`2048`-byte IEC 61937 burst), FLTP converted via `dtsS32Swr_` to packed `AV_SAMPLE_FMT_S32`, `1536k` (full) / `768k` (half) bps at `48000` Hz (`strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL` for `dca`).
- **Upmix Modes (`SpdifEncoder::Upmix`)**:
  - `Off` (swr default), `Surround` (dynamic stereo-in-multichannel detection -> 5.1 `surround` filter), `FrontStage` (dynamic 3.1 FL+FR+FC+LFE, silent rears), `Always` (unconditional 5.1 upmix), `AlwaysFront` (unconditional 3.1 upmix).
  - Keep both the `libavfilter` graph and `swr_` initialized in dynamic mode to avoid FFT re-priming latency spikes.

## 3. WASAPI, COM & Live Reload Invariants (Non-Negotiable)
- **COM Activation Order**: `WasapiPassthrough::InitExclusive` MUST call `device->Activate(IID_IAudioClient, ...)` BEFORE `IsFormatSupported` (prevent null dereference `0xC0000005`).
- **Loopback vs. Cable Capture**: WASAPI loopback (`--loopback`) cannot use `AUDCLNT_STREAMFLAGS_EVENTCALLBACK`; poll at low sleep intervals so 512-sample DTS bursts never starve.
- **Hot-Reload IPC (`ac3-control.ini` & `MeterSharedState.h`)**:
  - Per-channel gains (`fl_db`, `fr_db`, `c_db`, `lfe_db`, `sl_db`, `sr_db` in `-60..0` dB; `-60` = mute) and `reload_id` are polled by `engine.exe` to apply gain changes immediately and trigger seamless in-process pipeline re-init on mode/bitrate/latency/upmix changes.
