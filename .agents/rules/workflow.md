---
trigger: model_decision
description: End-to-end C++17 feature implementation sequence, doctest TDD, local PortableBuildTools compilation, and GitHub Actions CI/Release verification workflow. Load when implementing features or verifying builds/releases.
---

# Feature Implementation, Local Build & GitHub Actions QA Workflow

```text
[1. Scout & Plan] ➔ [2. Config & DSP Core] ➔ [3. Doctest Cases] ➔ [4. ControlCenter GUI] ➔ [5. Local Build & CTest] ➔ [6. Push & GitHub Actions Release]
```

## 1. Execution Sequence
1. **Config & Core DSP (`ac3core`)**: Update `engine/src/Config.h` and `engine/src/SpdifEncoder.{h,cpp}` (using FFmpeg 8 `AVChannelLayout` / `swr_alloc_set_opts2`). Keep `ac3core` decoupled from WASAPI so it remains unit-testable.
2. **WASAPI Engine (`engine`)**: Wire CLI/config parsing and live hot-reload (`ac3-control.ini` `reload_id`) in `engine/src/main.cpp`, `WasapiCapture.cpp`, `WasapiPassthrough.cpp`, or `WasapiPcmOutput.cpp`.
3. **Unit Tests (`unit_tests`)**: Add or update `doctest` cases in `engine/test/test_encoder.cpp`, `test_upmix.cpp`, or `test_ringbuffer.cpp` verifying frame counts, IEC 61937 burst sizes, and RMS channel energy.
4. **Control Center GUI (`control_center`)**: Expose new options in `engine/src/ControlCenter.cpp` following `.agents/rules/design.md` and persist them in `SaveConfiguration()` / `LoadConfiguration()`.

## 2. Local Build & CTest (`C:\BuildTools` PortableBuildTools)
Standalone MSVC `14.44` + Windows SDK `26100` (`x64`) is installed at `C:\BuildTools`. Run local build + `ctest` before pushing:
```powershell
powershell -ExecutionPolicy Bypass -File scripts/build-local.ps1
```
- `scripts/build-local.ps1` automatically sources `C:\BuildTools\devcmd.ps1` into the session, builds all targets (`ac3core`, `engine`, `encode_wav`, `control_center`, `unit_tests`) into `engine/build-local` via `NMake Makefiles`, and runs `ctest --output-on-failure`.

## 3. GitHub Actions CI & Release (`PatalJunior/virtual-ac3-encoder`)
- Pushing to `main` on `fork` triggers `.github/workflows/release.yml` (builds Release x64, runs `ctest`, builds Inno Setup installer `ISCC.exe`, and publishes a patch-bumped GitHub Release).
- Monitor runs via:
  ```powershell
  & "C:\Program Files\GitHub CLI\gh.exe" run list --repo PatalJunior/virtual-ac3-encoder --limit 3
  ```
