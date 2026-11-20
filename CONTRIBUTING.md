# Contributing

Contributions to the Virtual AC3 Encoder Control Center are welcome. Please use a focused branch and pull request so changes can be reviewed before they reach the default branch.

## Before you start

- Check existing issues and pull requests for related work.
- For substantial changes, open an issue or discussion first to agree on the intended behavior.
- Keep changes scoped. Separate documentation, UI, audio-engine, and driver changes when practical.

## Development setup

Use Windows 10 or 11 with Visual Studio 2022 or Build Tools, the Desktop development with C++ workload, CMake, FFmpeg shared development libraries, and Qt 6.2+ for Qt interface changes. See [Building from Source](docs/BUILDING.md).

Create a branch from the default branch and build locally:

```powershell
git switch -c feature/short-description
.\build.ps1
```

For DSP or engine changes, run the unit test target when possible:

```powershell
.\build.ps1 -RunTests
```

Audio-engine and DSP contributions should include focused tests where practical. Driver changes should include clear Windows and WDK validation notes because the kernel driver cannot be covered by the user-mode unit tests.

## Pull requests

A pull request should include:

- A concise summary of the change and its reason.
- The Windows version, compiler, Qt version, and FFmpeg build used for validation when relevant.
- Commands run and their results. Distinguish code you compiled or tested from code you only inspected.
- Screenshots for visible UI changes and short reproduction steps for behavior changes.
- Updates to README or `docs/` when user setup, behavior, command-line options, or architecture changes.

Do not commit generated output from `engine/build`, the staged `bin` directory, downloaded FFmpeg binaries, archives, logs, or other local build artifacts. Preserve Microsoft's copyright headers in the driver and update `NOTICE.md` when third-party code or dependencies change.

## Style

- Keep the engine compatible with C++17 and the project's CMake/MSVC build.
- Match the surrounding naming, formatting, and error-handling style.
- Keep real-time audio callbacks bounded and avoid blocking I/O or allocation in the callback path.
- Avoid claiming a feature is tested unless you ran the relevant build, test, or hardware check and can report the result.
