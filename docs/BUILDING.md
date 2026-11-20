# Building from Source

## Requirements

- Windows 10 or 11, 64-bit.
- Visual Studio 2022 or Build Tools with **Desktop development with C++** and a Windows SDK.
- CMake 3.20 or later.
- FFmpeg shared development libraries. The project uses the FFmpeg headers, import libraries, and runtime DLLs for its AC-3/DTS and audio-processing code. They are not maintained in this source tree; use the included fetch script.
- Qt 6.2 or later for MSVC 2022 64-bit, with the Widgets and Network components, to build both Qt control panels and DSP Studio. Qt is optional for the engine and legacy Win32 panel.
- PowerShell to run the build and dependency scripts.

## Build the application

Open PowerShell at the repository root and run:

```powershell
.\build.ps1
```

The script locates or fetches FFmpeg, configures the CMake project for x64, builds Release binaries, stages them in `bin`, and deploys Qt runtime libraries when Qt is available.

If the dependencies are installed in custom locations, pass their paths explicitly:

```powershell
.\build.ps1 -FfmpegRoot "D:\deps\ffmpeg" -QtDir "C:\Qt\6.8.0\msvc2022_64"
```

`FfmpegRoot` must contain `include`, `lib`, and `bin` subdirectories. `QtDir` should point to the MSVC 64-bit Qt installation directory containing `lib\cmake\Qt6`.

Useful build-script options:

```powershell
.\build.ps1 -RunTests  # Build and run unit_tests.exe
.\build.ps1 -Clean     # Remove the CMake build directory before configuring
```

The script writes generated files to `engine\build` and deploys runnable binaries to `bin`. Do not commit these generated outputs or downloaded FFmpeg binaries.

## Outputs

With Qt 6 detected, the `bin` directory includes:

- `engine.exe` — audio engine.
- `ControlCenter_A_Mixer.exe` — mixer-style Qt control panel.
- `ControlCenter_B_Modes.exe` — mode-list Qt control panel.
- `VirtualAc3-DSP-Studio.exe` — equalizer, compressor, metering, and DSP settings tool.
- `control_center.exe` — legacy Win32 control panel.

Without Qt 6, the build omits the two Qt panels and DSP Studio but still builds the engine and Win32 panel.

## Optional virtual driver

The `driver` directory contains a virtual audio endpoint derived from Microsoft's SimpleAudioSample. Building it requires the Windows Driver Kit (WDK) and Visual Studio driver development components. The included driver is test-signed; Windows test-signing and Secure Boot settings affect whether it can load. Most users can use VB-CABLE as the capture endpoint instead. Driver build and installation scripts are in `scripts`.

## Troubleshooting

- **FFmpeg headers not found:** run `scripts\fetch-ffmpeg.ps1`, or pass `-FfmpegRoot` to a directory that contains `include\libavcodec\avcodec.h`, `lib`, and `bin`.
- **Qt panels are missing:** install Qt 6 for MSVC 2022 x64 with Widgets and Network, then pass its directory with `-QtDir`.
- **A staged executable cannot be replaced:** close the panel and `engine.exe`, then rerun the build.
- **The receiver stays on PCM or does not lock:** confirm that the selected output supports AC-3/DTS passthrough and that the receiver is connected to that output. `engine.exe --probe` can check AC-3 passthrough support.
