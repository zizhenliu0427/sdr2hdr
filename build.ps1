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

# Detect system language (Chinese or English)
$isChinese = (Get-Culture).Name -match "zh|CN|TW|HK"

if ($Clean -and (Test-Path $BuildDir)) {
    Write-Host "Removing $BuildDir..." -ForegroundColor Yellow
    Remove-Item -Recurse -Force $BuildDir
}

# 1. Resolve SDK roots --------------------------------------------------------

# 1a. RTX Video SDK
while ($true) {
    if (-not $env:NV_RTX_VIDEO_SDK) {
        if ($isChinese) {
            Write-Host "未设置 NV_RTX_VIDEO_SDK 环境变量。" -ForegroundColor Yellow
            Write-Host "    请从以下地址下载 SDK:" -ForegroundColor Yellow
            Write-Host "    https://developer.nvidia.com/rtx-video-sdk" -ForegroundColor Cyan
            $input = Read-Host "如果已完成下载，请输入 RTX Video SDK 路径 (例如 D:\RTX_Video_SDK_v1.1.0)"
        } else {
            Write-Host "NV_RTX_VIDEO_SDK not set." -ForegroundColor Yellow
            Write-Host "    Please download the SDK from:" -ForegroundColor Yellow
            Write-Host "    https://developer.nvidia.com/rtx-video-sdk" -ForegroundColor Cyan
            $input = Read-Host "If already downloaded, enter RTX Video SDK path (e.g. D:\RTX_Video_SDK_v1.1.0)"
        }
        $env:NV_RTX_VIDEO_SDK = $input.Trim('"')
    }

    if (Test-Path (Join-Path $env:NV_RTX_VIDEO_SDK "include\nvsdk_ngx.h")) {
        break
    }

    Write-Host ""
    if ($isChinese) {
        Write-Host "[!] RTX Video SDK 路径无效: $env:NV_RTX_VIDEO_SDK" -ForegroundColor Red
        Write-Host "    缺少 include\nvsdk_ngx.h 文件，请检查路径是否正确。" -ForegroundColor Yellow
    } else {
        Write-Host "[!] Invalid RTX Video SDK path: $env:NV_RTX_VIDEO_SDK" -ForegroundColor Red
        Write-Host "    Missing include\nvsdk_ngx.h file, please check the path." -ForegroundColor Yellow
    }
    Write-Host ""
    $env:NV_RTX_VIDEO_SDK = $null
}
Write-Host "RTX Video SDK: $env:NV_RTX_VIDEO_SDK" -ForegroundColor Green

# 1b. Video Codec SDK
while ($true) {
    if (-not $env:NV_VIDEO_CODEC_SDK) {
        $defaultNvcodec = Join-Path $ScriptDir "vendor\nvcodec"
        if (Test-Path (Join-Path $defaultNvcodec "Interface\nvcuvid.h")) {
            $env:NV_VIDEO_CODEC_SDK = $defaultNvcodec
            Write-Host "Video Codec SDK: $defaultNvcodec (auto-detected)" -ForegroundColor Green
            break
        }
        if ($isChinese) {
            Write-Host "未设置 NV_VIDEO_CODEC_SDK 环境变量。" -ForegroundColor Yellow
            Write-Host "    请从以下地址下载 SDK:" -ForegroundColor Yellow
            Write-Host "    https://developer.nvidia.com/video-codec-sdk" -ForegroundColor Cyan
            $input = Read-Host "如果已完成下载，请输入 Video Codec SDK 路径 (例如 D:\Video_Codec_SDK_13.0)"
        } else {
            Write-Host "NV_VIDEO_CODEC_SDK not set." -ForegroundColor Yellow
            Write-Host "    Please download the SDK from:" -ForegroundColor Yellow
            Write-Host "    https://developer.nvidia.com/video-codec-sdk" -ForegroundColor Cyan
            $input = Read-Host "If already downloaded, enter Video Codec SDK path (e.g. D:\Video_Codec_SDK_13.0)"
        }
        $env:NV_VIDEO_CODEC_SDK = $input.Trim('"')
    }

    if (Test-Path (Join-Path $env:NV_VIDEO_CODEC_SDK "Interface\nvcuvid.h")) {
        break
    }

    Write-Host ""
    if ($isChinese) {
        Write-Host "[!] Video Codec SDK 路径无效: $env:NV_VIDEO_CODEC_SDK" -ForegroundColor Red
        Write-Host "    缺少 Interface\nvcuvid.h 文件，请检查路径是否正确。" -ForegroundColor Yellow
    } else {
        Write-Host "[!] Invalid Video Codec SDK path: $env:NV_VIDEO_CODEC_SDK" -ForegroundColor Red
        Write-Host "    Missing Interface\nvcuvid.h file, please check the path." -ForegroundColor Yellow
    }
    Write-Host ""
    $env:NV_VIDEO_CODEC_SDK = $null
}
Write-Host "Video Codec SDK: $env:NV_VIDEO_CODEC_SDK" -ForegroundColor Green

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
    Write-Host ""
    if ($isChinese) {
        Write-Host "[!] 未找到 Visual Studio Build Tools。" -ForegroundColor Red
        Write-Host "    请安装 Visual Studio Build Tools，并勾选「使用 C++ 的桌面开发」工作负载。" -ForegroundColor Yellow
    } else {
        Write-Host "[!] Visual Studio Build Tools not found." -ForegroundColor Red
        Write-Host "    Please install Visual Studio Build Tools with the 'Desktop development with C++' workload." -ForegroundColor Yellow
    }
    Write-Host "    Download: https://visualstudio.microsoft.com/visual-cpp-build-tools/" -ForegroundColor Cyan
    Write-Host ""
    throw "vcvars64.bat not found."
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
        Write-Host ""
        if ($isChinese) {
            Write-Host "[!] 未找到 Ninja。" -ForegroundColor Red
            Write-Host "    通过 scoop 安装: scoop install ninja" -ForegroundColor Yellow
            Write-Host "    或在 Visual Studio Installer 中安装「CMake」组件。" -ForegroundColor Yellow
        } else {
            Write-Host "[!] Ninja not found." -ForegroundColor Red
            Write-Host "    Install via scoop: scoop install ninja" -ForegroundColor Yellow
            Write-Host "    Or install the 'CMake' component in Visual Studio Installer." -ForegroundColor Yellow
        }
        Write-Host "    Download: https://ninja-build.org/" -ForegroundColor Cyan
        Write-Host ""
        throw "ninja.exe not found."
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
    Write-Host ""
    if ($isChinese) {
        Write-Host "[!] PATH 中未找到 CMake。" -ForegroundColor Red
        Write-Host "    请安装 CMake >= 3.18 并添加到 PATH。" -ForegroundColor Yellow
    } else {
        Write-Host "[!] CMake not found on PATH." -ForegroundColor Red
        Write-Host "    Install CMake >= 3.18 and add it to PATH." -ForegroundColor Yellow
    }
    Write-Host "    Download: https://cmake.org/download/" -ForegroundColor Cyan
    Write-Host ""
    throw "cmake.exe not found on PATH."
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
