# Quick Start

## 1. Build

Follow [Building from Source](BUILDING.md), then open PowerShell in the repository root and run:

```powershell
.\build.ps1
```

The runnable files are staged in `bin`. Install a virtual audio cable such as [VB-CABLE](https://vb-audio.com/Cable/) if you are not using the optional virtual driver. After installing a virtual cable, restart Windows if its installer asks you to.

## 2. Choose devices and mode

Start one of the control panels:

```powershell
.\bin\ControlCenter_A_Mixer.exe
# or
.\bin\ControlCenter_B_Modes.exe
```

Choose the audio capture device, digital output connected to your receiver, and an output mode. The panels expose these workflows:

| Mode | Use |
| --- | --- |
| Dolby Digital 5.1 | Encode captured audio to AC-3 without stereo upmix. |
| Dolby Digital 5.1 with upmix | Upmix stereo input when applicable, then encode to AC-3. |
| PCM Stereo 2.0 | Send ordinary two-channel PCM. |
| DTS 5.1 Core | Encode captured PCM to a DTS core bitstream for compatible S/PDIF outputs. |
| PCM 5.1 to DTS 5.1 | Capture multichannel audio through loopback and encode to DTS. |
| PCM 5.1 to Dolby Digital 5.1 | Capture multichannel audio through loopback and encode to AC-3. |
| DTS bitstream to Dolby Digital 5.1 | Decode an incoming DTS core bitstream and encode it to AC-3. |

Set the virtual cable's playback endpoint (often named `CABLE Input`) as the Windows playback device for the apps you want to capture. In Windows speaker configuration, select 5.1 when the chosen mode expects multichannel PCM. Select the matching recording endpoint (often `CABLE Output`) as the engine input.

Select an optical S/PDIF endpoint for AC-3 or DTS output. The receiver should indicate Dolby Digital or DTS when it locks to the encoded stream. PCM Stereo sends uncompressed stereo and should be reported as PCM by the receiver.

## 3. Start and adjust audio

Start the engine from the panel. Use the channel controls to adjust output levels. Open DSP Studio from the panel to edit the equalizer, compressor, meters, and DSP settings. DSP parameter changes are stored in `dsp-settings.json` next to the executables and are reloaded by the running engine.

Presets are staged in `bin\dsp-presets` and upmix presets in `bin\upmix-presets`. Use DSP Studio to load or save settings. Keep a copy of settings before editing them manually.

## Command-line examples

List available Windows audio endpoints and their IDs:

```powershell
.\bin\engine.exe --list
```

Check which outputs support AC-3 passthrough:

```powershell
.\bin\engine.exe --probe
```

Run the engine directly with a virtual-cable capture endpoint and a named digital output:

```powershell
.\bin\engine.exe --in "CABLE Output" --out "Realtek Digital Output"
```

Use `--out-spdif` to let the engine choose an output that advertises S/PDIF support:

```powershell
.\bin\engine.exe --in "CABLE Output" --out-spdif
```

For the optional virtual driver, use WASAPI loopback capture, for example:

```powershell
.\bin\engine.exe --loopback --in "Virtual AC3 Encoder" --out "Realtek Digital Output"
```

Endpoint names differ between systems. Use `--list` and `--probe` before copying examples literally. The engine also supports endpoint IDs, configuration files, duration-limited runs, and diagnostic options; see `engine.exe --version` output and `engine/src/main.cpp` for the current command-line switches.

## Notes

- S/PDIF carries stereo PCM or compressed multichannel formats such as AC-3 and DTS; it does not carry 5.1 PCM.
- The optional test-signed kernel driver may require Secure Boot to be disabled and test-signing mode enabled. Prefer a properly signed driver or VB-CABLE when Secure Boot must remain enabled.
- Third-party software such as VB-CABLE and FFmpeg is distributed separately and retains its own license.
