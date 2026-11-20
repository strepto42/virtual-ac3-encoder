<#
.SYNOPSIS
  Сборка AC3/DTS Control Center v2 (engine.exe + control_center.exe) и раскладка в .\bin

.DESCRIPTION
  1. Ищет FFmpeg (dev-сборка с lib/include/bin): -FfmpegRoot, .\third_party\ffmpeg, затем старую папку
     с исходниками (-LegacyDir). Если не нашёл - запускает scripts\fetch-ffmpeg.ps1.
  2. Конфигурирует и собирает проект через CMake (Release, x64).
  3. (-RunTests) запускает unit_tests.exe.
  4. Копирует engine.exe, control_center.exe, DLL FFmpeg и среду VC в .\bin и, если в .\bin
     ещё нет настроек, переносит virtual-ac3-encoder.conf и ac3-control.ini из старой сборки.

  Нужны: Visual Studio 2022 с компонентом "Разработка классических приложений на C++" (включает CMake).

.EXAMPLE
  .\build.ps1
  .\build.ps1 -RunTests
  .\build.ps1 -FfmpegRoot "D:\ffmpeg"
#>
[CmdletBinding()]
param(
  [string]$FfmpegRoot = '',
  [string]$LegacyDir  = 'E:\SS data\src\github-publish-virtual-ac3-encoder-control-center',
  [string]$V2Dir      = 'C:\VirtualAc3-ControlCenter-v2',
  [string]$QtDir      = '',
  [switch]$RunTests,
  [switch]$Clean
)
$ErrorActionPreference = 'Stop'
$root     = $PSScriptRoot
$engine   = Join-Path $root 'engine'
$buildDir = Join-Path $engine 'build'
$binDir   = Join-Path $root 'bin'

function Find-FFmpeg {
  $candidates = @()
  if ($FfmpegRoot) { $candidates += $FfmpegRoot }
  $candidates += (Join-Path $root 'third_party\ffmpeg')
  $candidates += (Join-Path $LegacyDir 'third_party\ffmpeg')
  foreach ($c in $candidates) {
    if (Test-Path (Join-Path $c 'include\libavcodec\avcodec.h')) { return (Resolve-Path $c).Path }
  }
  return $null
}

function Find-Qt {
  if ($QtDir -and (Test-Path (Join-Path $QtDir 'lib\cmake\Qt6'))) { return (Resolve-Path $QtDir).Path }
  $cands = foreach ($r in @('C:\Qt', 'D:\Qt', (Join-Path $env:USERPROFILE 'Qt'))) {
    if (Test-Path $r) {
      Get-ChildItem $r -Directory | Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' } |
        ForEach-Object { Get-ChildItem $_.FullName -Directory -Filter 'msvc*_64' }
    }
  }
  $best = $cands | Sort-Object { [version]$_.Parent.Name } -Descending | Select-Object -First 1
  if ($best) { return $best.FullName }
  return $null
}

function Find-CMake {
  $cmd = Get-Command cmake.exe -ErrorAction SilentlyContinue
  if ($cmd) { return $cmd.Source }
  $patterns = @(
    "$env:ProgramFiles\Microsoft Visual Studio\*\*\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
    "${env:ProgramFiles(x86)}\Microsoft Visual Studio\*\*\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
    "$env:ProgramFiles\CMake\bin\cmake.exe"
  )
  foreach ($p in $patterns) {
    $hit = Get-ChildItem $p -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($hit) { return $hit.FullName }
  }
  throw 'CMake не найден. Установи Visual Studio 2022 (C++ desktop development) или CMake и повтори.'
}

# --- FFmpeg --------------------------------------------------------------------------------
$ff = Find-FFmpeg
if (-not $ff) {
  Write-Host 'FFmpeg не найден - запускаю scripts\fetch-ffmpeg.ps1 ...'
  & (Join-Path $root 'scripts\fetch-ffmpeg.ps1')
  $ff = Find-FFmpeg
}
if (-not $ff) { throw 'FFmpeg dev-файлы не найдены. Укажи -FfmpegRoot <папка с include, lib, bin>.' }
Write-Host "FFmpeg: $ff"

# --- Закрыть запущенные экземпляры из .\bin, иначе файлы заняты --------------------------------
if (Test-Path $binDir) {
  Get-Process control_center, engine -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -and $_.Path -like "$binDir\*" } |
    ForEach-Object { Write-Host "Останавливаю $($_.Name) (pid $($_.Id))"; Stop-Process -Id $_.Id -Force }
  Start-Sleep -Milliseconds 500
}

# --- CMake build ---------------------------------------------------------------------------
$cmake = Find-CMake
if ($Clean -and (Test-Path $buildDir)) { Remove-Item $buildDir -Recurse -Force }
$qt = Find-Qt
$qtArgs = @()
if ($qt) {
  Write-Host "Qt: $qt"
  $qtArgs = @("-DCMAKE_PREFIX_PATH=$qt", "-DQt6_DIR=$qt\lib\cmake\Qt6")
} else {
  Write-Warning 'Qt 6 (MSVC 64-bit) не найден: будут собраны только engine.exe и старая панель. Установи Qt 6 и укажи -QtDir "C:\Qt\6.x.x\msvc2022_64".'
}
& $cmake -S $engine -B $buildDir -A x64 "-DFFMPEG_ROOT=$ff" "-DVAC3_VERSION=3.0-qt1" @qtArgs
if ($LASTEXITCODE -ne 0) { throw 'CMake configure не удался.' }
& $cmake --build $buildDir --config Release
if ($LASTEXITCODE -ne 0) { throw 'Сборка не удалась. Пришли текст ошибок.' }

$engineExe = Get-ChildItem $buildDir -Recurse -Filter engine.exe | Select-Object -First 1
if (-not $engineExe) { throw 'engine.exe не найден после сборки.' }
$outDir = $engineExe.DirectoryName

if ($RunTests) {
  $tests = Join-Path $outDir 'unit_tests.exe'
  if (Test-Path $tests) {
    Write-Host '--- unit_tests ---'
    & $tests
    if ($LASTEXITCODE -ne 0) { throw 'Unit-тесты не прошли.' }
  } else { Write-Warning 'unit_tests.exe не найден.' }
}

# --- Раскладка в .\bin ----------------------------------------------------------------------
New-Item -ItemType Directory -Force -Path $binDir | Out-Null
function Copy-Binary([string]$source, [string]$destDir) {
  for ($try = 1; $try -le 5; $try++) {
    try { Copy-Item $source $destDir -Force -ErrorAction Stop; return }
    catch {
      if ($try -eq 5) {
        throw "Не удалось заменить $(Split-Path $source -Leaf) в $destDir (файл занят). Закрой панель (значок в трее, Выход) и останови engine.exe, затем повтори .\build.ps1. Старая версия осталась на месте."
      }
      Start-Sleep -Milliseconds 700
    }
  }
}
Copy-Binary (Join-Path $outDir 'engine.exe')         $binDir
if (Test-Path (Join-Path $outDir 'upmix_wav.exe')) { Copy-Binary (Join-Path $outDir 'upmix_wav.exe') $binDir }   # offline upmix test tool
Copy-Binary (Join-Path $outDir 'control_center.exe') $binDir
Get-ChildItem (Join-Path $outDir '*.dll') | ForEach-Object { Copy-Binary $_.FullName $binDir }

# Qt panels (variants A and B) + Qt runtime
$qtExes = @('ControlCenter_A_Mixer.exe', 'ControlCenter_B_Modes.exe', 'VirtualAc3-DSP-Studio.exe') | Where-Object { Test-Path (Join-Path $outDir $_) }
foreach ($name in $qtExes) { Copy-Binary (Join-Path $outDir $name) $binDir }
if ($qtExes.Count -gt 0 -and $qt) {
  $deploy = Join-Path $qt 'bin\windeployqt.exe'
  if (Test-Path $deploy) {
    $targets = $qtExes | ForEach-Object { Join-Path $binDir $_ }
    & $deploy --release --no-translations --no-opengl-sw --no-system-d3d-compiler --no-compiler-runtime @targets
  } else { Write-Warning "windeployqt.exe не найден в $qt\bin: скопируй Qt DLL вручную." }
}
foreach ($d in 'VCRUNTIME140.dll','VCRUNTIME140_1.dll','MSVCP140.dll') {
  $p = Join-Path $env:WINDIR "System32\$d"
  if (Test-Path $p) { Copy-Item $p $binDir -Force }
}

function Import-IfMissing([string]$name, [string[]]$sources) {
  $target = Join-Path $binDir $name
  if (Test-Path $target) { return }
  foreach ($s in $sources) {
    if (Test-Path $s) { Copy-Item $s $target; Write-Host "Перенесён $name из $s"; return }
  }
}
Import-IfMissing 'virtual-ac3-encoder.conf' @(
  (Join-Path $V2Dir 'bin\virtual-ac3-encoder.conf'),
  (Join-Path $LegacyDir 'engine\build\Release\virtual-ac3-encoder.conf'),
  (Join-Path $env:LOCALAPPDATA 'virtual-ac3-encoder\virtual-ac3-encoder.conf'),
  (Join-Path $root 'installer\default.conf'))
Import-IfMissing 'ac3-control.ini' @(
  (Join-Path $V2Dir 'bin\ac3-control.ini'),
  (Join-Path $LegacyDir 'engine\build\Release\ac3-control.ini'),
  (Join-Path $env:LOCALAPPDATA 'virtual-ac3-encoder\ac3-control.ini'))

# DSP (эквалайзер + компрессор): рабочий файл настроек (создаётся один раз, твои правки не затираются)
# и готовые пресеты. Чтобы применить пресет, скопируй его поверх bin\dsp-settings.json.
Import-IfMissing 'dsp-settings.json' @((Join-Path $root 'installer\dsp-presets\00-Off.json'))
$presetSrc = Join-Path $root 'installer\dsp-presets'
if (Test-Path $presetSrc) {
  $presetDst = Join-Path $binDir 'dsp-presets'
  New-Item -ItemType Directory -Force -Path $presetDst | Out-Null
  Copy-Item (Join-Path $presetSrc '*.json') $presetDst -Force
}
# Пресеты апмикса (кино / музыка / речь): вкладка «Апмикс» в DSP Studio; свои пресеты Studio сохраняет туда же.
$upmixSrc = Join-Path $root 'installer\upmix-presets'
if (Test-Path $upmixSrc) {
  $upmixDst = Join-Path $binDir 'upmix-presets'
  New-Item -ItemType Directory -Force -Path $upmixDst | Out-Null
  Copy-Item (Join-Path $upmixSrc '*.json') $upmixDst -Force
}

Write-Host ''
Write-Host "Готово: $binDir"
Write-Host "DSP: настройки $binDir\dsp-settings.json (читаются движком на лету), пресеты в $binDir\dsp-presets"
Write-Host "Панели Qt: $binDir\ControlCenter_A_Mixer.exe (вариант A) и $binDir\ControlCenter_B_Modes.exe (вариант B)"
Write-Host "DSP Studio: $binDir\VirtualAc3-DSP-Studio.exe (также кнопка 'DSP Studio…' в панелях)"
Write-Host "Старая панель Win32: $binDir\control_center.exe"
Write-Host 'Первый запуск: закрой старую панель и старый engine.exe (или согласись на их закрытие в диалоге).'
