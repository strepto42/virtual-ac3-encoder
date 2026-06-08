# virtual-ac3-encoder

[![CI](https://github.com/strepto42/virtual-ac3-encoder/actions/workflows/ci.yml/badge.svg)](https://github.com/strepto42/virtual-ac3-encoder/actions/workflows/ci.yml)

A Windows 10/11 software implementation of **"Dolby Digital Live"**: a virtual 5.1 audio
device that accepts any multichannel PCM stream, encodes it to **AC3 (Dolby Digital)** in real
time, and streams it as an **IEC 61937 / S-PDIF** bitstream out a chosen **Toslink optical**
output to an AV receiver.

> **Also known as / what problem this solves:** a free, open-source software **Dolby Digital Live
> (DDL)** and **DTS Connect** alternative for Windows — for PCs whose sound card can't encode
> surround to optical, so you can get true **5.1 surround over a single Toslink / S-PDIF (optical)
> cable** to an AV receiver or soundbar. Essentially **SoundPusher for Windows**. Useful when
> "Dolby Digital Live" / "DTS Interactive" / "DTS Connect" isn't available on your motherboard or
> USB sound card (Realtek, ASUS Xonar, etc.).
>
> _Currently encodes **AC3 (Dolby Digital, 5.1, 640 kbps)**. DTS Connect-style **DTS** output is a
> feasible extension — FFmpeg ships a DTS encoder; see the design notes in `CLAUDE.md`._

**Keywords:** Dolby Digital Live, DTS Connect, DTS Interactive, software AC3 encoder, real-time
AC3/AC-3 encoder, 5.1 surround over optical/SPDIF/Toslink, IEC 61937 passthrough, WASAPI exclusive
passthrough, virtual audio device/cable, FFmpeg AC3, SoundPusher for Windows, HTPC surround sound.

## Download / install

Get the latest **[Release](https://github.com/strepto42/virtual-ac3-encoder/releases/latest)**:
- **`virtual-ac3-encoder-setup-x.y.z.exe`** — per-user installer (no admin); auto-starts hidden at
  logon, with Start-Menu shortcuts for config / log / device list.
- **`virtual-ac3-encoder-x.y.z-win64.zip`** — portable build; extract and run `engine.exe`
  (see `QUICKSTART.txt`).

You also need a virtual audio cable to capture surround audio — install
**[VB-CABLE](https://vb-audio.com/Cable/)** and set *CABLE Input* as your default 5.1 device — then
connect the optical output to your receiver. Prefer to build from source? See **Build** below.

Optical (S-PDIF) can carry stereo PCM *or* a compressed 5.1 bitstream, never 5.1 PCM — so a
surround stream has to be encoded to AC3 on the PC before it leaves the optical port. Many
onboard sound chips don't offer this; this project does it in software.

## Architecture

Two components (the encoder is FFmpeg and **cannot** live in a kernel driver, so the work is
split):

1. **Kernel virtual audio driver** (`driver/`) — a PortCls/WaveRT virtual cable (SYSVAD-derived)
   that publishes a 5.1 render endpoint plus a paired loopback-capture endpoint. *Phase 3.*
2. **User-mode engine** (`engine/`) — captures the 5.1 PCM, encodes AC3, wraps it as IEC 61937,
   and renders it to the optical endpoint via WASAPI exclusive passthrough.

The engine's real-time design (output clock = master, lock-free ring buffer between capture and
output, drift correction by periodic latency-trim) is modeled on
[SoundPusher](https://codeberg.org/q-p/SoundPusher) (MIT), the equivalent macOS tool. The AC3
encode + S-PDIF mux uses **FFmpeg** (`libavcodec` AC3 encoder + `spdif` muxer + `libswresample`),
the same engine Kodi uses internally. See `third_party/reference/` for the cloned references.

## Status

- [x] **Phase 1 — offline encoder core.** `SpdifEncoder` (PCM → AC3 640 kbps 5.1 → IEC 61937)
      + lock-free `RingBuffer` + WAV test harness. Verified: output decodes as
      `ac3, 48000 Hz, 5.1, 640 kb/s`.
- [x] **Phase 2 — live WASAPI engine.** Shared capture/loopback → ring buffer → exclusive
      IEC 61937 passthrough (output = master clock; SoundPusher-style 64-cycle drift-trim).
- [x] **TDD harness.** doctest `unit_tests` (RingBuffer incl. SPSC stress + encoder contract).
- [x] **Phase 3 — kernel virtual audio driver, built & test-signed.** 5.1 render-only virtual
      endpoint ("Virtual AC3 Encoder (5.1)"). Loads only with Secure Boot OFF (see Install).
- [x] **Working end-to-end (confirmed).** Because Secure Boot is ON here, the live system uses
      **VB-CABLE** as the 5.1 source: `engine --in "CABLE Output" --out "Realtek Digital Output"`
      → receiver decodes **Dolby Digital**. (Our own driver is ready for when Secure Boot is off.)
- [ ] **Phase 4 — packaging** (config file + auto-start at logon; optional tray UI).

## Components / engine flags

`engine.exe`:
- `--list` — list render + capture endpoints.
- `--probe` — which outputs accept AC3 passthrough (IsFormatSupported, non-intrusive).
- `--mon` — capture-only throughput diagnostic (non-intrusive).
- `--loopback` — treat `--in` as a *render* endpoint and capture it via WASAPI loopback
  (used with the virtual driver).
- `--in <name>` / `--in-id <id>` / `--out <name>` / `--out-id <id>` / `--out-spdif`
- `--bitrate <bps>` (default 640000) / `--safe <frames>` (drift target, default 1536)
- `--config <path>` (defaults to `virtual-ac3-encoder.conf` next to the exe) ·
  `--hidden` (hide console) · `--log <path>` (log to file) · `--duration <s>` (auto-stop)
- `--upmix surround|off` — for stereo input, upmix to 5.1 via FFmpeg's `surround` filter
  (a free DTS Neo:PC / Pro Logic II-style matrix upmix). **Default `surround`**; use `off` for
  untouched stereo→front. Multichannel input is downmixed regardless.

Config precedence: built-in defaults < config file (`key=value`: `in`, `out`, `in_id`, `out_id`,
`bitrate`, `safe`, `loopback`, `out_spdif`, `upmix`) < command-line flags.

## Driver (Phase 3)

Derived from Microsoft's SimpleAudioSample (`third_party/reference/wds`), trimmed to a single
5.1 render endpoint and rebranded. Build (WDK 10.0.26100 + VS2022):

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" `
  driver\SimpleAudioSample.sln /p:Configuration=Release /p:Platform=x64 /p:SpectreMitigation=false
# output: driver\x64\Release\package\  (SimpleAudioSample.sys / .inf / .cat)
```

### Install (requires Secure Boot OFF for a test-signed driver)

```powershell
# 1. Disable Secure Boot in your UEFI/BIOS (manual).
# 2. Elevated PowerShell:
scripts\install-driver.ps1 -EnableTestSigning   # enables test signing + trusts the test cert
# 3. Reboot.
scripts\install-driver.ps1                        # creates the ROOT\SimpleAudioSample device
# Uninstall: scripts\uninstall-driver.ps1 [-DisableTestSigning]
```

## Running it

```powershell
# With the virtual driver installed (render-only + loopback):
engine\build\Release\engine.exe --loopback --in "Virtual AC3 Encoder" --out "Realtek Digital Output"

# Or, without the driver, using an existing virtual cable (works with Secure Boot ON):
engine\build\Release\engine.exe --in "CABLE Output" --out "Realtek Digital Output"
```

Set the virtual device as the Windows default 5.1 output, play surround content, and switch the
receiver to the matching optical input — it should report Dolby Digital.

## Set and forget (autostart)

Install the engine to a stable per-user location and have it start hidden at every logon, with
restart-on-failure (no elevation, no Task Scheduler — a Startup-folder supervisor that runs in the
real interactive session):

```powershell
scripts\setup-autostart.ps1                                   # VB-CABLE -> Realtek (defaults)
scripts\setup-autostart.ps1 -In "CABLE Output" -Out "Realtek Digital Output" -Bitrate 640000
scripts\setup-autostart.ps1 -In "Virtual AC3 Encoder" -Loopback   # when using our own driver
scripts\remove-autostart.ps1 [-DeleteInstall]                 # undo
```

This stages `engine.exe` + DLLs to `%LOCALAPPDATA%\virtual-ac3-encoder`, writes
`virtual-ac3-encoder.conf` there (edit it to change devices/bitrate), and drops a supervisor in the
Startup folder that runs `engine --hidden --log` and relaunches it if it exits.

## Build (engine)

Requires Visual Studio 2022 (or Build Tools) and CMake. FFmpeg shared dev libs are fetched
into `third_party/ffmpeg/`.

```powershell
# 1. Fetch FFmpeg dev libraries (DLLs + import libs + headers)
scripts\fetch-ffmpeg.ps1

# 2. Configure + build
cmake -S engine -B engine\build -G "Visual Studio 17 2022" -A x64
cmake --build engine\build --config Release
```

## Phase 1 test

```powershell
scripts\make-test-wav.ps1                       # generates engine\test\test_5p1.wav (5.1 tones)
engine\build\Release\encode_wav.exe engine\test\test_5p1.wav engine\test\out.spdif

# Verify the result is real AC3 inside IEC 61937:
ffmpeg -f spdif -i engine\test\out.spdif -f null -
```

## License

**MIT** — see [`LICENSE`](LICENSE). The kernel driver is derived from Microsoft's MIT-licensed
SimpleAudioSample, and the engine design is modeled on SoundPusher (MIT); the engine dynamically
links FFmpeg (LGPL). Full attributions are in [`NOTICE.md`](NOTICE.md). No third-party binaries
(FFmpeg, VB-CABLE) are committed — they're fetched/installed separately.
