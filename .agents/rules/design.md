---
trigger: model_decision
description: Dear ImGui + DirectX 11 Control Center Dark Cyber-Audio theme palette, static VU meter layout, and Win32 windowing rules. Load when modifying ControlCenter.cpp or UI styling.
---

# Design System Rules (Dark Cyber-Audio — Dear ImGui / DX11)

`ControlCenter.cpp` implements a native hardware-accelerated DirectX 11 + Dear ImGui studio interface.

## 1. Dark Cyber-Audio Color Palette (`SetupModernAudioStyle`)

| Role | ImVec4 / IM_COL32 | Hex | Usage |
| :--- | :--- | :--- | :--- |
| **Window Background** | `ImVec4(0.09f, 0.10f, 0.12f, 1.00f)` | `#17191F` | Root window background (`ImGuiCol_WindowBg`) |
| **Card / Child Surface** | `ImVec4(0.12f, 0.13f, 0.16f, 1.00f)` | `#1E2129` | Grouped panels & meter cards (`ImGuiCol_ChildBg`) |
| **Control Frame** | `ImVec4(0.15f, 0.17f, 0.21f, 1.00f)` | `#262B36` | Combo boxes, sliders, inputs (`ImGuiCol_FrameBg`) |
| **Border Subtle** | `ImVec4(0.20f, 0.24f, 0.28f, 0.70f)` | `#333D47` | Panel and frame borders (`ImGuiCol_Border`) |
| **Text Primary** | `ImVec4(0.92f, 0.94f, 0.96f, 1.00f)` | `#EBF0F5` | Primary labels and headings (`ImGuiCol_Text`) |
| **Text Muted** | `ImVec4(0.50f, 0.55f, 0.60f, 1.00f)` | `#808C99` | Hints, secondary captions (`ImGuiCol_TextDisabled`) |
| **Accent Cyan** | `ImVec4(0.13f, 0.75f, 0.85f, 1.00f)` | `#21BFD9` | Sliders, channel headers, active separators |
| **Accent Mint** | `ImVec4(0.15f, 0.85f, 0.65f, 1.00f)` | `#26D9A6` | Active checkmarks, running status badges |
| **Meter: Nominal** | `IM_COL32(32, 226, 128, 255)` | `#20E280` | VU level `<= -12 dB` |
| **Meter: Warm** | `IM_COL32(245, 170, 35, 255)` | `#F5AA23` | VU level `> -12 dB` and `<= -3 dB` |
| **Meter: Clip / Mute** | `IM_COL32(235, 60, 60, 255)` | `#EB3C3C` | VU level `> -3 dB`, MUTE badge, or error state |

## 2. Layout, Windowing & UX Rules
- **Win32 Dark Title Bar & Resizing**: Always apply `DwmSetWindowAttribute` with `DWMWA_USE_IMMERSIVE_DARK_MODE` (attributes `20` and `19`). Enforce minimum window track size (`760×650`) via `WM_GETMINMAXINFO` and vertical-only scrolling in the main ImGui viewport (never horizontal overflow).
- **Zero-Jitter 6-Channel VU Meters (`DrawSpeakerVUMeters`)**:
  - Position all 6 channel columns (`FL`, `FR`, `C`, `LFE`, `SL`, `SR`) using deterministic screen coordinates (`colStartX = startPos.x + i * (colWidth + colSpacing)`) via `ImDrawList` — never let dynamic dB label widths shift adjacent columns.
  - Maintain smooth ballistics (`~15 ms` fast attack, exponential decay, `45`-frame peak hold).
- **Instant Live Apply**: Every UI control change (device, output mode, bitrate, latency preset, stereo upmix, or speaker trim/mute) must immediately call `SaveConfiguration()` (updating `virtual-ac3-encoder.conf` and bumping `reload_id` in `ac3-control.ini`) without requiring the user to stop/restart the engine.
