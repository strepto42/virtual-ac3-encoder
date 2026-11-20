# Project guidance

This repository contains the v3 Virtual AC3 Encoder Control Center: a Windows 10/11 C++17 audio engine, optional Qt control panels and DSP Studio, and an optional test-signed virtual audio driver.

## Main components

- `engine/src` — WASAPI capture/output, AC-3/DTS handling, configuration, and engine loop.
- `engine/src/dsp` — equalizer, compressor, upmix, settings, metering, and profile logic.
- `engine/qt` and `engine/studio` — Qt control panels and DSP Studio.
- `driver` — optional PortCls/WaveRT virtual 5.1 endpoint.
- `scripts` — FFmpeg setup, driver installation, autostart, and offline audio helpers.

See `docs/ARCHITECTURE.md` for the data flow and `docs/BUILDING.md` for the Windows build setup.

## Build and validation

Use the root build script from PowerShell:

```powershell
.\build.ps1
.\build.ps1 -RunTests
```

The first command builds Release binaries and stages them in `bin`. The second also runs `unit_tests.exe`. Qt 6.2 or later for MSVC 2022 x64 is needed for the Qt panels and DSP Studio; without Qt, the engine and legacy Win32 panel are still built. FFmpeg development libraries are fetched by the build script when missing.

For changes to engine or DSP behavior, run the relevant unit tests when possible and describe exactly what was built or tested. Driver changes also need Windows/WDK integration validation; unit tests do not validate kernel loading or hardware behavior.

## Change boundaries

- Keep the engine compatible with C++17 and the existing MSVC/CMake build.
- Keep audio callback work bounded; avoid blocking I/O and allocations in real-time callback paths.
- Preserve Microsoft copyright headers in driver files and update `NOTICE.md` for third-party changes.
- Do not commit generated `engine/build`, staged `bin`, downloaded `third_party/ffmpeg`, archive, log, or machine-specific files.
- Keep public setup and behavior documentation in English and update it when user-facing behavior changes.

## Git workflow

Work on a feature branch, open a pull request, and merge only after required checks pass. Do not push directly to `main`. Keep commits focused and provide build/test results in the pull request. Do not claim that audio behavior was validated unless it was tested with the relevant Windows devices and receiver.
