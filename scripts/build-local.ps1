param(
    [string]$BuildDir = "engine/build-local",
    [switch]$Clean
)

$ErrorActionPreference = "Stop"

# 1. Load PortableBuildTools environment into current session (no logout required)
if (Test-Path "C:\BuildTools\devcmd.ps1") {
    . "C:\BuildTools\devcmd.ps1"
} else {
    $userEnv = Get-ItemProperty HKCU:\Environment
    if ($userEnv.INCLUDE) { $env:INCLUDE = $userEnv.INCLUDE }
    if ($userEnv.LIB)     { $env:LIB     = $userEnv.LIB }
    if ($userEnv.LIBPATH) { $env:LIBPATH = $userEnv.LIBPATH }
    if ($userEnv.Path)    { $env:PATH    = "$($userEnv.Path);$env:PATH" }
}

if ($Clean -and (Test-Path $BuildDir)) {
    Remove-Item -Recurse -Force $BuildDir
}

# 2. Configure with NMake Makefiles (bundled with PortableBuildTools)
if (-not (Test-Path "$BuildDir/Makefile")) {
    cmake -S engine -B $BuildDir -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

# 3. Build all targets
cmake --build $BuildDir
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# 4. Run doctest unit tests
ctest --test-dir $BuildDir --output-on-failure
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
