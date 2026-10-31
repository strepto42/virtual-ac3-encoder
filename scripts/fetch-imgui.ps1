# fetch-imgui.ps1 — Downloads Dear ImGui into third_party/imgui
$ErrorActionPreference = "Stop"

$tag = "v1.91.4"
$destDir = Join-Path $PSScriptRoot "..\third_party\imgui"
$backendsDir = Join-Path $destDir "backends"

if (-not (Test-Path $destDir)) { New-Item -ItemType Directory -Path $destDir -Force | Out-Null }
if (-not (Test-Path $backendsDir)) { New-Item -ItemType Directory -Path $backendsDir -Force | Out-Null }

$coreFiles = @(
    "imgui.h",
    "imconfig.h",
    "imgui_internal.h",
    "imstb_rectpack.h",
    "imstb_textedit.h",
    "imstb_truetype.h",
    "imgui.cpp",
    "imgui_draw.cpp",
    "imgui_tables.cpp",
    "imgui_widgets.cpp"
)

$backendFiles = @(
    "backends/imgui_impl_win32.h",
    "backends/imgui_impl_win32.cpp",
    "backends/imgui_impl_dx11.h",
    "backends/imgui_impl_dx11.cpp"
)

Write-Host "Fetching Dear ImGui $tag..."

foreach ($f in $coreFiles) {
    $url = "https://raw.githubusercontent.com/ocornut/imgui/$tag/$f"
    $out = Join-Path $destDir $f
    if (-not (Test-Path $out)) {
        Write-Host "  Downloading $f..."
        Invoke-WebRequest -Uri $url -OutFile $out -UseBasicParsing
    }
}

foreach ($f in $backendFiles) {
    $url = "https://raw.githubusercontent.com/ocornut/imgui/$tag/$f"
    $baseName = Split-Path $f -Leaf
    $out = Join-Path $backendsDir $baseName
    if (-not (Test-Path $out)) {
        Write-Host "  Downloading $f..."
        Invoke-WebRequest -Uri $url -OutFile $out -UseBasicParsing
    }
}

Write-Host "Dear ImGui $tag fetch completed."
