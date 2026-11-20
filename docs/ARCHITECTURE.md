# Architecture

## Components

The project has three layers:

1. **Capture endpoint.** Audio applications render to a virtual 5.1 playback endpoint. This can be provided by VB-CABLE or by the optional PortCls/WaveRT driver in `driver`. With the driver's render-only endpoint, the engine captures through WASAPI loopback.
2. **User-mode engine (`engine`).** The C++17 engine enumerates Windows audio devices, captures PCM through WASAPI, buffers audio to handle capture/output clock differences, applies DSP, encodes AC-3 or DTS where selected, and sends audio to a Windows render endpoint. The PCM Stereo mode sends two-channel PCM using shared-mode WASAPI output. Encoded AC-3/DTS output uses exclusive-mode IEC 61937 passthrough.
3. **Control interfaces.** `engine/qt` contains the two Qt control-panel variants. `engine/studio` contains DSP Studio. `engine/src/ControlCenter.cpp` is the legacy Win32 panel. The panels write configuration and live control files consumed by the engine; they do not process the audio themselves.

## Audio data flow

```text
Audio application
    │
    ▼
Virtual playback endpoint (VB-CABLE or optional driver)
    │ WASAPI capture or loopback capture
    ▼
Engine capture and ring buffer
    │
    ├── Optional DTS-core bitstream detection and decode to PCM
    ├── Optional stereo-to-5.1 upmix
    ├── DSP chain: channel-aware graphic EQ → compressor
    ├── Channel trims, headroom, and limiter (encoded modes)
    │
    ├── AC-3 encoder → IEC 61937 → exclusive-mode digital output
    ├── DTS encoder → IEC 61937 → exclusive-mode digital output
    └── PCM Stereo → shared-mode two-channel output
```

The real-time AC-3/DTS render clock is the master clock. A lock-free ring buffer absorbs clock drift between the capture and output devices. The encoder and DSP run in user mode because they depend on FFmpeg and do not belong in a kernel driver.

## DSP and settings

The audio processing code is under `engine/src/dsp`. It includes the 30-band equalizer, compressor, matrix upmix, settings parser, metering, and support code for DSP Studio. The engine checks `dsp-settings.json` next to its executable for changes while running. The control panels use `ac3-control.ini` for per-channel trims and stop control; general engine configuration is stored in `virtual-ac3-encoder.conf`.

The Qt DSP Studio UI is in `engine/studio`. It also provides spectrum and level views and tools for managing presets and application-based profiles. Preset JSON files shipped with the source are under `installer/dsp-presets` and `installer/upmix-presets`; the build script stages them in `bin`.

## Source tree

- `engine/src` — WASAPI capture/output, AC-3/DTS encoding, and engine entry point.
- `engine/src/dsp` — DSP algorithms, settings, meters, upmixing, and profile logic.
- `engine/qt` — Qt control panels and shared UI backend.
- `engine/studio` — DSP Studio.
- `engine/test` — unit and offline audio tools.
- `driver` — optional Windows virtual audio endpoint driver.
- `scripts` — FFmpeg fetch, driver setup, autostart, and audio test helpers.
- `installer` — default settings and preset JSON files.
- `docs` — user and developer documentation.

## Boundaries and limitations

- The optional driver is a test-signed development driver. Driver signing and Secure Boot policy determine whether it loads on a given system. VB-CABLE can serve as the virtual capture endpoint without using this driver.
- The receiver and digital output must support the selected compressed format for AC-3/DTS passthrough.
- PCM mode is stereo. It does not provide multichannel PCM over S/PDIF.
- DTS input support is for DTS core bitstreams carried through IEC 61937. Unsupported IEC 61937 data types are not decoded by the DTS core decoder.
