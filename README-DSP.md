# DSP Overview

The engine provides a 30-band graphic equalizer, a compressor, channel-aware settings, live parameter reload, level metering, and optional stereo-to-5.1 upmixing. DSP Studio is the Qt application for editing these settings and inspecting levels and spectrum information.

## Settings and presets

The running engine reads `dsp-settings.json` from the directory containing the executable. DSP Studio writes to that file, and the engine reloads changes while running. DSP presets are JSON files staged in `bin\dsp-presets`; upmix presets are staged in `bin\upmix-presets`.

The DSP Studio window can be opened from either Qt control panel or by running `VirtualAc3-DSP-Studio.exe` from `bin`. Its controls include the 30 EQ bands, compressor settings, channel selection, level and spectrum views, A/B comparison, and preset/profile tools.

## Upmix options

The project includes the FFmpeg `surround` upmix path and a time-domain matrix upmix. The matrix mode is configured with `upmix_algo=matrix` in `virtual-ac3-encoder.conf`; its parameters are stored in the `upmix` section of `dsp-settings.json`. The FFT-based `surround` path has processing latency; the time-domain matrix path is designed to avoid look-ahead latency. Neither is a substitute for a discrete multichannel source.

## Audio paths

The DSP chain is used by the encoded AC-3/DTS paths and PCM Stereo output. The PCM output is stereo and does not carry 5.1 PCM over S/PDIF. Encoded modes apply the configured headroom and limiter before encoding; PCM output has no output limiter, so reduce preamp or EQ boosts if the signal clips.

For build and setup instructions, see [Building from Source](docs/BUILDING.md) and [Quick Start](docs/QUICKSTART.md). Implementation details and data flow are in [Architecture](docs/ARCHITECTURE.md).
