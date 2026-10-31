# AGENTS.md — virtual-ac3-encoder Workspace Index

**virtual-ac3-encoder** (**Dolby Control Center**) is a Windows 10/11 real-time software **Dolby Digital Live / DTS Connect** alternative built on **C++17 WASAPI + FFmpeg 8 (`avcodec-62`) + Dear ImGui/DX11 (`control_center`) + WDK WaveRT Virtual 5.1 Driver (`driver/`)**.

## Core Invariants (Always Active)
- **Modern FFmpeg 8 API**: Strictly use `AVChannelLayout`, `swr_alloc_set_opts2`, and `avcodec_send_frame` / `avcodec_receive_packet`. Never use legacy `channel_layout` bitmasks or `encode2`.
- **Real-Time Audio Hot Path**: Keep `WasapiCapture` -> `RingBuffer` -> `SpdifEncoder` -> `WasapiPassthrough` / `WasapiPcmOutput` lock-free and allocation-free per packet. Always `Activate` `IAudioClient` before `IsFormatSupported`.
- **MSVC `/W4 /permissive-` & Unicode**: Zero warnings on `ac3core` and `control_center`. Always use wide Win32 calls (`LoadIconW`, etc.), include `<sstream>` explicitly, and never redefine `<ksmedia.h>` types (`WAVEFORMATEXTENSIBLE_IEC61937`).
- **Live Hot-Reload Sync**: Settings changes in `ControlCenter.cpp` write `virtual-ac3-encoder.conf` + `ac3-control.ini` (incrementing `reload_id`) so the running `engine.exe` reloads codec/bitrate/latency/upmix/gains live without manual stop/start.
- **Local Build (`C:\BuildTools`) & TDD**: Verify changes locally via `powershell -ExecutionPolicy Bypass -File scripts/build-local.ps1` (`C:\BuildTools\devcmd.ps1` + `cmake` + `ctest` `doctest`). Never commit `third_party/` or `build*`/`x64/`.

> Detailed specifications in `.agents/rules/architecture.md` (always on), `.agents/rules/design.md` (ImGui/Win32 UI), and `.agents/rules/workflow.md` (local build & GitHub Actions QA).
