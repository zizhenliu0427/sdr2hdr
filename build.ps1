<#
    One-click build script for the sdr2hdr tool.

    Default generator: Ninja (works around VS 2026 not yet having CUDA 12.x
    MSBuild integration). We import vcvars64.bat so cl.exe is on PATH, then
    let CMake drive nvcc + cl directly via ninja.

    Usage (from any working directory):
        powershell -ExecutionPolicy Bypass -File .\build.ps1
        powershell -ExecutionPolicy Bypass -File .\build.ps1 -Clean
        powershell -ExecutionPolicy Bypass -File .\build.ps1 -Config Debug
#>
param(
    [string]$Config = "Release",
    [switch]$Clean
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$BuildDir  = Join-Path $ScriptDir "build"

if ($Clean -and (Test-Path $BuildDir)) {
    Write-Host "Removing $BuildDir..." -ForegroundColor Yellow
    Remove-Item -Recurse -Force $BuildDir
}

# 1. Resolve SDK root ---------------------------------------------------------
if (-not $env:NV_RTX_VIDEO_SDK) {
    $SdkRoot = (Resolve-Path (Join-Path $ScriptDir "..\..")).Path
    Write-Host "NV_RTX_VIDEO_SDK not set - using auto-detected $SdkRoot" -ForegroundColor Yellow
    $env:NV_RTX_VIDEO_SDK = $SdkRoot
} else {
    Write-Host "Using NV_RTX_VIDEO_SDK = $env:NV_RTX_VIDEO_SDK"
}

if (-not (Test-Path (Join-Path $env:NV_RTX_VIDEO_SDK "include\nvsdk_ngx.h"))) {
    throw "Invalid NV_RTX_VIDEO_SDK '$env:NV_RTX_VIDEO_SDK' - include\nvsdk_ngx.h missing."
}

# 2. Locate toolchain pieces --------------------------------------------------
# 2a. vcvars64.bat -- try vswhere first, then common paths.
$VcVars = $null
$vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path $vsWhere) {
    $vsRoot = & $vsWhere -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
    if ($vsRoot) {
        $cand = Join-Path $vsRoot "VC\Auxiliary\Build\vcvars64.bat"
        if (Test-Path $cand) { $VcVars = $cand }
    }
}
if (-not $VcVars) {
    foreach ($p in @(
        "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat",
        "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat",
        "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat",
        "C:\Program Files (x86)\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
    )) {
        if (Test-Path $p) { $VcVars = $p; break }
    }
}
if (-not $VcVars) {
    throw "vcvars64.bat not found. Install Visual Studio Build Tools with the 'Desktop development with C++' workload."
}
Write-Host "vcvars64: $VcVars"

# 2b. Ninja (bundled with VS's CMake component is the most reliable bet).
$Ninja = Get-Command ninja -ErrorAction SilentlyContinue
if (-not $Ninja) {
    $hits = Get-ChildItem -Path (Split-Path $VcVars) -Recurse -Filter "ninja.exe" `
                          -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $hits) {
        # Widen search a bit: VS bundles ninja under Common7 IDE CommonExtensions.
        $vsRootGuess = ($VcVars -replace "\\VC\\Auxiliary\\Build\\vcvars64\.bat$","")
        $hits = Get-ChildItem -Path $vsRootGuess -Recurse -Filter "ninja.exe" `
                              -ErrorAction SilentlyContinue | Select-Object -First 1
    }
    if (-not $hits) {
        throw "ninja.exe not found. Install it (scoop/choco) or the CMake component of Visual Studio."
    }
    $NinjaPath = $hits.FullName
} else {
    $NinjaPath = $Ninja.Source
}
Write-Host "ninja:   $NinjaPath"

# 2c. nvcc (for a sanity message only; CMake will find it via CUDAToolkit).
$nvcc = Get-Command nvcc -ErrorAction SilentlyContinue
if (-not $nvcc) {
    foreach ($p in @(
        "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.9\bin\nvcc.exe",
        "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.8\bin\nvcc.exe",
        "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.6\bin\nvcc.exe"
    )) {
        if (Test-Path $p) { Write-Host "nvcc:    $p"; break }
    }
} else {
    Write-Host "nvcc:    $($nvcc.Source)"
}

# 3. Make sure cmake is on PATH ----------------------------------------------
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    throw "cmake.exe not found on PATH. Install CMake >= 3.18 and re-run."
}

# 4. Write a temporary batch file that: vcvars64 + cmake configure + cmake build
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
$TmpBat = Join-Path $BuildDir "build_cmdline.bat"

# %~dp0 is the build directory itself. We:
#   1. Run vcvars64.bat so cl.exe/link.exe are on PATH.
#   2. Prepend ninja's directory to PATH.
#   3. cmake configure with -G Ninja and Release build type.
#   4. cmake --build.
$NinjaDir = Split-Path $NinjaPath
$BatBody = @"
@echo off
call "$VcVars"
if errorlevel 1 goto :eof

set "PATH=$NinjaDir;%PATH%"
rem nvcc 12.x's host-compiler version check is very strict (<= VS 2022) and
rem bails out even though the actual codegen works with newer cl.exe.
set "CUDAFLAGS=-allow-unsupported-compiler %CUDAFLAGS%"

echo.
echo === Configuring ===
cmake -S "$ScriptDir" -B "$BuildDir" -G Ninja -DCMAKE_BUILD_TYPE=$Config
if errorlevel 1 exit /b 1

echo.
echo === Building ($Config) ===
cmake --build "$BuildDir" --parallel
if errorlevel 1 exit /b 1
"@
Set-Content -Path $TmpBat -Value $BatBody -Encoding Ascii

& cmd.exe /c "`"$TmpBat`""
if ($LASTEXITCODE -ne 0) {
    throw "Build failed (exit $LASTEXITCODE). Scroll up for the first error."
}

# 5. Report output -----------------------------------------------------------
$ExePath = Join-Path $BuildDir "sdr2hdr.exe"
if (Test-Path $ExePath) {
    Write-Host "`nBuilt: $ExePath" -ForegroundColor Green
    Write-Host "Try: `"$ExePath`" input.mp4 output.mp4 --hdr"
} else {
    throw "Build completed but sdr2hdr.exe not found at $ExePath"
}
