# CLAUDE.md — virtual-ac3-encoder

Guidance for AI agents (and humans) working in this repo.

## What this is
A Windows 10/11 software **"Dolby Digital Live"** (a free **DTS Connect** / **SoundPusher-for-
Windows** alternative): a virtual 5.1 audio device that accepts any multichannel PCM stream,
encodes it to **AC3 (Dolby Digital)** in real time, wraps it as an **IEC 61937 / S-PDIF**
bitstream, and streams it out a chosen **Toslink (optical)** output to an AV receiver — i.e. real
5.1 surround over a single optical cable when your sound card can't do DDL/DTS Connect itself. (Optical carries stereo PCM *or* a compressed 5.1 bitstream — never 5.1 PCM — so
surround must be encoded before it leaves the port.)

Two components, because the encoder is FFmpeg and **cannot run in kernel mode**:
- **`engine/`** — user-mode WASAPI engine (all the value). CMake + MSVC, C++17.
- **`driver/`** — kernel virtual 5.1 *render-only* endpoint (WDK; derived from Microsoft's
  MIT SimpleAudioSample). The engine reads it via WASAPI loopback.

## Architecture / data flow
```
app → virtual 5.1 device (our driver OR VB-CABLE)
    → engine captures (driver: --loopback; cable: normal capture of "CABLE Output")
    → lock-free RingBuffer (absorbs clock drift)
    → SpdifEncoder: libswresample downmix→5.1 FLTP, libavcodec AC3 @640k,
      libavformat "spdif" muxer → 6144-byte IEC 61937 burst
    → WasapiPassthrough: EXCLUSIVE event-driven render = MASTER clock,
      1536-frame burst/cycle, SoundPusher-style 64-cycle drift-trim
    → optical out → receiver decodes Dolby Digital
```
The real-time design (output clock = master, ring buffer, periodic latency-trim) is modeled on
**SoundPusher** (MIT). See `NOTICE.md` for attributions.

## Build & test
**Engine** (do this first; FFmpeg dev libs are not committed):
```powershell
scripts\fetch-ffmpeg.ps1          # FFmpeg 8.x shared dev libs → third_party/ffmpeg
cmake -S engine -B engine\build -G "Visual Studio 17 2022" -A x64
cmake --build engine\build --config Release
ctest --test-dir engine\build -C Release --output-on-failure   # doctest unit tests
```
Offline encoder check: `scripts\make-test-wav.ps1` then
`engine\build\Release\encode_wav.exe engine\test\test_5p1.wav out.spdif` and
`ffmpeg -f spdif -i out.spdif -f null -` (should decode `ac3 48000 5.1`).

**Driver** (needs WDK 10.0.26100 + VS individual component `Component.Microsoft.Windows.DriverKit`):
```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" `
  driver\SimpleAudioSample.sln /p:Configuration=Release /p:Platform=x64 /p:SpectreMitigation=false
# output: driver\x64\Release\package\  (.sys/.inf/.cat, test-signed)
```

## Gotchas (learned the hard way — don't regress these)
- **Modern FFmpeg API only.** Use `AVChannelLayout`, `swr_alloc_set_opts2`,
  `avcodec_send_frame`/`receive_packet`. SoundPusher's old API (`codec->channel_layout`,
  `codec->encode2`) does NOT compile against FFmpeg 8 (avcodec-62).
- **AC3 encoder needs `AV_SAMPLE_FMT_FLTP`** (planar float); swr converts to it. Sample rate must
  be 48000/44100/32000.
- **`WasapiPassthrough::InitExclusive` must `Activate` the `IAudioClient` before
  `IsFormatSupported`** — otherwise null COM ptr → 0xC0000005.
- **Driver build from CLI: use the *amd64* MSBuild.** The 32-bit MSBuild makes the INF-verify step
  look for `x86\InfVerif.dll` (only x64/arm64 exist) → build fails. Also pass
  `/p:SpectreMitigation=false` (Spectre libs not installed) and, if multiple VS installs exist,
  pin CMake to Community via `-DCMAKE_GENERATOR_INSTANCE="...\\2022\\Community"`.
- **Driver is render-only:** `g_cCaptureEndpoints 0` (minipairs.h); the capture-install loop is
  `#if`-guarded so the unsigned `i < 0` doesn't trip C4296 under `/WX`.
- **Speaker format** = 5.1 / 48 kHz / 16-bit (speakerwavtable.h, `KSAUDIO_SPEAKER_5POINT1`).
  **INF OS decoration lowered to `19041`** (Win10 2004+) from the sample's `22000` (Win11-only).
- **Engine reads our driver via `--loopback`** (WASAPI loopback can't be event-driven → polled).
  VB-CABLE uses normal capture (`--in "CABLE Output"`).

## Install / Secure Boot reality
A **test-signed** kernel driver loads **only with Secure Boot OFF** (and test signing on; Secure
Boot overrides/ignores test signing). With Secure Boot ON, either disable it, attestation-sign the
driver (Partner Center + EV cert), or use **VB-CABLE** (properly signed) as the source instead.
Install our driver: `scripts\install-driver.ps1 -EnableTestSigning` (elevated) → reboot →
`scripts\install-driver.ps1`. Uninstall: `scripts\uninstall-driver.ps1`.

## Engine flags
`--list` (endpoints) · `--probe` (AC3 support per output) · `--mon` (capture throughput diag) ·
`--loopback` · `--duration N` · `--in/--out` (name substr) · `--in-id/--out-id` · `--out-spdif` ·
`--bitrate <bps>` · `--safe <frames>` · `--config <path>` · `--hidden` (hide console) ·
`--log <path>` · `--upmix off|surround` (stereo->5.1 via FFmpeg `surround` filter). Config
precedence: defaults < config file (`virtual-ac3-encoder.conf` next to the exe; keys
`in/out/in_id/out_id/bitrate/safe/loopback/out_spdif/upmix`) < CLI.

**Surround upmix:** `SpdifEncoder` runs an FFmpeg `surround` libavfilter graph (abuffer → surround →
aformat → abuffersink) for <=2ch input when `upmix=surround`, accumulating output in an `AVAudioFifo`
and priming with silence (FFT latency) so the realtime consumer doesn't starve. Needs the `avfilter`
lib (linked in CMake). `log` is a VBScript reserved word — unrelated, but note prior gotchas list.

## Autostart ("set and forget")
`scripts/setup-autostart.ps1` stages the engine to `%LOCALAPPDATA%\virtual-ac3-encoder` (+ FFmpeg &
VC-runtime DLLs + conf) and installs a **Startup-folder VBScript supervisor** that runs
`engine --hidden --log ...` and relaunches it if it exits. `remove-autostart.ps1` undoes it.
**Why Startup folder, not Task Scheduler:** a Task Scheduler interactive task launching the exe from
`%LOCALAPPDATA%` failed with `0x80070002`, and wscript launched by the task hung spawning children
(interactive-task desktop quirk). The Startup folder runs in the genuine interactive logon session
where WASAPI + a hidden console work, and needs no elevation.

## More gotchas (autostart/launcher)
- **Batch files require CRLF** — generate `.cmd`/`.bat` via Set-Content line *arrays* (LF-only made
  cmd fail with "'engine.exe' is not recognized").
- **`log` is a reserved word in VBScript** (the `Log()` function) — don't use it as a variable
  (caused "Illegal assignment"). `wscript` shows a modal error dialog on script errors; test new
  scripts with `cscript //nologo` to see errors on the console.
- The engine links the **dynamic MSVC runtime**; the autostart installer copies
  `VCRUNTIME140*.dll` / `MSVCP140.dll` next to the exe so it's self-contained.

## Conventions
- C++17; `/W4` on `ac3core`. **MIT** licensed; keep MS copyright headers in `driver/`.
- **Never commit** `third_party/` (FFmpeg, VB-CABLE, reference clones) or build output
  (`**/build/`, `**/x64/`, `*.sys/.cat/.dll/.lib/.obj`). `.gitattributes` keeps the UTF-16 `.inx`
  byte-exact — don't let it get EOL-converted.
- Commit messages end with: `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`.

## Branching & releases (required workflow)
- **Never commit directly to `main`.** Work on a feature branch, open a PR, and **merge only when
  CI is green**. `main` is branch-protected (the "Engine build + unit tests (Windows)" check is
  required).
- **Every merge to `main` auto-releases**: `release.yml` (on push to main) builds + tests, then
  **auto patch-bumps** the version from the latest release, tags `vX.Y.Z`, builds the portable zip
  + Inno Setup installer, and publishes a GitHub Release.
- For a **minor/major** bump, run it manually: `gh workflow run release.yml -f version=X.Y.Z`.
- `ci.yml` runs on PRs only (the merge gate); `release.yml` owns building main.

## TDD
User-mode logic is unit-tested with **doctest** (`engine/test/test_*.cpp` — RingBuffer incl. an
SPSC stress test, and the encoder contract). Add tests first for new engine logic. The kernel
driver isn't unit-testable on-box; validate it by integration (endpoint appears → engine
loopback-captures → receiver locks Dolby Digital).
