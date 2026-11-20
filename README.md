# Virtual AC3 Encoder Control Center

A Windows audio control center and real-time encoder for sending multichannel PC audio to a receiver over an optical S/PDIF connection. The v3 codebase adds Qt control panels, DSP Studio, a 30-band equalizer, a compressor, channel meters, and matrix upmixing to the Virtual AC3 Encoder engine.

## What it does

- Encodes PCM audio to Dolby Digital (AC-3) or DTS for compatible S/PDIF outputs.
- Sends ordinary two-channel PCM when compressed surround is not needed.
- Supports AC-3, DTS, and PCM workflows, including decoding an incoming DTS bitstream to AC-3.
- Applies a 30-band graphic equalizer and compressor to supported audio paths. Settings can be changed while the engine is running.
- Provides two Qt control-panel layouts and a separate DSP Studio window.
- Captures audio through a virtual audio endpoint and sends the resulting stream to a selected digital output.

S/PDIF carries stereo PCM or a compressed surround bitstream; it does not carry uncompressed 5.1 PCM. For surround over optical, the PC must encode the audio to AC-3 or DTS before output.

## Requirements

- Windows 10 or 11, 64-bit.
- Visual Studio 2022 or Build Tools with the Desktop development with C++ workload, Windows SDK, and CMake.
- FFmpeg shared development libraries. `build.ps1` runs `scripts/fetch-ffmpeg.ps1` when it cannot find them locally.
- Qt 6.2 or later for MSVC 2022 64-bit to build the Qt panels and DSP Studio. Without Qt, the engine and legacy Win32 panel can still be built.
- A virtual audio input, such as [VB-CABLE](https://vb-audio.com/Cable/), and an S/PDIF output that accepts AC-3 or DTS passthrough for compressed surround.

See [Building](docs/BUILDING.md) for setup details and [Quick Start](docs/QUICKSTART.md) for first run steps.

## Build

From the repository root in PowerShell:

```powershell
.\build.ps1
```

If Qt is installed outside the auto-detected locations:

```powershell
.\build.ps1 -QtDir "C:\Qt\6.8.0\msvc2022_64"
```

The script builds into `engine/build` and stages executables, runtime DLLs, default settings, and presets in `bin`. It does not install the kernel driver. For optional unit tests, run `.uild.ps1 -RunTests`.

## Run

Launch either control-panel layout:

```powershell
.\bin\ControlCenter_A_Mixer.exe
# or
.\bin\ControlCenter_B_Modes.exe
```

Open DSP Studio from a panel or run it directly:

```powershell
.\bin\VirtualAc3-DSP-Studio.exe
```

The panels share the same engine and settings. Select the capture and digital output devices, choose an output mode, then start the engine. For a command-line device check:

```powershell
.\bin\engine.exe --list
.\bin\engine.exe --probe
```

More setup and audio-mode examples are in [Quick Start](docs/QUICKSTART.md).

## Architecture

The application is split into a user-mode audio engine, optional user interfaces, and an optional virtual audio driver. The engine captures from a virtual endpoint, processes and encodes the audio, then sends it to the selected output. See [Architecture](docs/ARCHITECTURE.md).

## Project documentation

- [Building from source](docs/BUILDING.md)
- [Quick Start and operating examples](docs/QUICKSTART.md)
- [Architecture and data flow](docs/ARCHITECTURE.md)
- [DSP overview](README-DSP.md)
- [Contributing](CONTRIBUTING.md)
- [Third-party notices](NOTICE.md)

## License

The project is distributed under the MIT License; see [LICENSE](LICENSE). Third-party components retain their respective licenses, documented in [NOTICE.md].
