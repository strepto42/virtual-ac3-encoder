# Virtual Audio Driver

This directory contains the optional Windows PortCls/WaveRT virtual audio driver used by Virtual AC3 Encoder Control Center. It is derived from Microsoft's [Windows Driver Samples SimpleAudioSample](https://github.com/microsoft/Windows-driver-samples) and exposes a virtual 5.1 render endpoint. The user-mode engine captures that endpoint through WASAPI loopback.

## Build

Driver development requires Visual Studio 2022 with the Windows Driver Kit (WDK) and driver development components. From the repository root, build the solution in `driver` for x64 Release using the 64-bit MSBuild tool. The package output is under `driver\x64\Release\package`.

The driver is test-signed. It is intended for development and testing and may not load under the machine's Secure Boot and driver-signing policy. Use the supplied scripts in `scripts` for installation and removal; review them before running, and use an elevated PowerShell session as required. Windows may require test-signing mode and a reboot. Do not disable security features on a system unless you understand the consequences and can restore them afterward.

Most users can use a separately installed virtual cable such as VB-CABLE as the capture endpoint and avoid installing this driver.

See [Building from Source](../docs/BUILDING.md), [Quick Start](../docs/QUICKSTART.md), and [Third-party notices](../NOTICE.md).
