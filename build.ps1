<#
    One-click build script for the sdr2hdr tool.

    Default generator: Ninja (works around VS 2026 not yet having CUDA 12.x
    MSBuild integration). We import vcvars64.bat so cl.exe is on PATH, then
    let CMake drive nvcc + cl directly via ninja.

    Usage (from any working directory):
        powershell -ExecutionPolicy Bypass -File .\build.ps1
        powershell -ExecutionPolicy Bypass -File .\build.ps1 -Clean
        powershell -ExecutionPolicy Bypass -File .\build.ps1 -Config Debug
        powershell -ExecutionPolicy Bypass -File .\build.ps1 -GUI
#>
param(
    [string]$Config = "Release",
    [switch]$Clean,
    [switch]$GUI
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

# Auto-detect an installed NVIDIA SDK by a signature file, so the build does not
# prompt every time. Checks explicit candidates first, then globs common roots
# (D:\, E:\, C:\, the user's Downloads, and next to / inside the repo).
function Find-NvSdk {
    param(
        [Parameter(Mandatory)][string]$Signature,  # relative path that must exist, e.g. "include\nvsdk_ngx.h"
        [Parameter(Mandatory)][string]$NameGlob,    # folder name pattern, e.g. "RTX_Video_SDK*"
        [string[]]$Extra = @()                      # explicit full-path candidates, checked first
    )
    foreach ($c in $Extra) {
        if ($c -and (Test-Path (Join-Path $c $Signature))) { return $c }
    }
    $roots = @("D:\", "E:\", "C:\", (Join-Path $env:USERPROFILE "Downloads"),
               (Split-Path $ScriptDir -Parent), $ScriptDir)
    foreach ($root in $roots) {
        if (-not (Test-Path $root)) { continue }
        $dirs = Get-ChildItem -Path $root -Directory -Filter $NameGlob -ErrorAction SilentlyContinue
        foreach ($d in $dirs) {
            if (Test-Path (Join-Path $d.FullName $Signature)) { return $d.FullName }
        }
    }
    return $null
}

# 1. Resolve SDK roots --------------------------------------------------------

# 1a. RTX Video SDK
while ($true) {
    if (-not $env:NV_RTX_VIDEO_SDK) {
        $autoRtx = Find-NvSdk -Signature "include\nvsdk_ngx.h" -NameGlob "RTX_Video_SDK*" `
                              -Extra @((Join-Path $ScriptDir "vendor\rtx_video_sdk"))
        if ($autoRtx) {
            $env:NV_RTX_VIDEO_SDK = $autoRtx
            Write-Host "RTX Video SDK: $autoRtx (auto-detected)" -ForegroundColor Green
            break
        }
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
        $autoNvc = Find-NvSdk -Signature "Interface\nvcuvid.h" -NameGlob "Video_Codec_SDK*" `
                             -Extra @((Join-Path $ScriptDir "vendor\nvcodec"))
        if ($autoNvc) {
            $env:NV_VIDEO_CODEC_SDK = $autoNvc
            Write-Host "Video Codec SDK: $autoNvc (auto-detected)" -ForegroundColor Green
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
# 2a. vcvars64.bat. IMPORTANT: prefer Visual Studio 2022 (v143). CUDA 12.x's
#     nvcc/cudafe++ crashes against VS 2026 (v145+) headers, and the RTX Video
#     SDK v1.1.0 in turn needs CUDA 12.x (CUDA 13.x crashes its NGX runtime).
#     So the only fully-working toolchain is VS 2022 + CUDA 12.x.
$VcVars = $null
$vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path $vsWhere) {
    # VS 2022 = v17.x; VS 2026 = v18.x. Pick 2022 first.
    $vsRoot = & $vsWhere -version "[17.0,18.0)" -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null | Select-Object -First 1
    if ($vsRoot) {
        $cand = Join-Path $vsRoot "VC\Auxiliary\Build\vcvars64.bat"
        if (Test-Path $cand) { $VcVars = $cand }
    }
}
if (-not $VcVars) {
    foreach ($p in @(
        "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat",
        "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat",
        "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat",
        "C:\Program Files (x86)\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
    )) {
        if (Test-Path $p) { $VcVars = $p; break }
    }
}
# Last resort: newest VS (e.g. VS 2026). WARNS, because this forces CUDA 13.x
# which crashes the RTX Video SDK NGX runtime at convert time.
if (-not $VcVars -and (Test-Path $vsWhere)) {
    $vsRoot = & $vsWhere -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
    if ($vsRoot) {
        $cand = Join-Path $vsRoot "VC\Auxiliary\Build\vcvars64.bat"
        if (Test-Path $cand) {
            $VcVars = $cand
            Write-Host "[!] Visual Studio 2022 not found; falling back to a newer VS." -ForegroundColor Yellow
            Write-Host "    That forces CUDA 13.x, which CRASHES the RTX Video SDK v1.1.0 NGX" -ForegroundColor Yellow
            Write-Host "    runtime during conversion. Install VS 2022 Build Tools (C++ workload)." -ForegroundColor Yellow
        }
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

# 2c. Pin CUDA to a 12.x toolkit (highest available). The RTX Video SDK v1.1.0
#     NGX runtime crashes (stack overrun) when the app is built against CUDA
#     13.x, so we force CUDA 12.x — overriding whatever CUDA_PATH points at.
#     Honour an explicit override via $env:SDR2HDR_CUDA_ROOT.
$cudaRoot = $env:SDR2HDR_CUDA_ROOT
if (-not $cudaRoot) {
    $cuda12 = Get-ChildItem "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA" -Directory -Filter "v12.*" `
              -ErrorAction SilentlyContinue | Sort-Object Name -Descending | Select-Object -First 1
    if ($cuda12) { $cudaRoot = $cuda12.FullName }
}
if ($cudaRoot -and (Test-Path (Join-Path $cudaRoot "bin\nvcc.exe"))) {
    $env:CUDA_PATH        = $cudaRoot
    $env:CUDAToolkit_ROOT = $cudaRoot
    $env:CUDACXX          = Join-Path $cudaRoot "bin\nvcc.exe"
    $env:PATH             = (Join-Path $cudaRoot "bin") + ";" + $env:PATH
    Write-Host "nvcc:    $($env:CUDACXX)  (pinned to CUDA 12.x for RTX Video SDK compatibility)" -ForegroundColor Green
} else {
    Write-Host "[!] No CUDA 12.x toolkit found under 'C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA'." -ForegroundColor Yellow
    Write-Host "    RTX Video SDK v1.1.0 needs CUDA 12.x; building with CUDA 13.x crashes NGX at" -ForegroundColor Yellow
    Write-Host "    conversion time. Install CUDA 12.9 (Custom -> CUDA only) and re-run with -Clean." -ForegroundColor Yellow
    $nvcc = Get-Command nvcc -ErrorAction SilentlyContinue
    if ($nvcc) { Write-Host "nvcc:    $($nvcc.Source)  (NOTE: not a 12.x toolkit)" }
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
cmake -S "$ScriptDir" -B "$BuildDir" -G Ninja -DCMAKE_BUILD_TYPE=$Config -DCMAKE_TOOLCHAIN_FILE="$ScriptDir\cmake\cuda-no-probe.cmake" $(if ($GUI) { "-DBUILD_GUI=ON" } else { "" })
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

# 4b. Optional WinUI 3 GUI (MSBuild + Windows App SDK NuGet) -----------------
if ($GUI) {
    Write-Host "`n=== Building WinUI GUI ===" -ForegroundColor Cyan
    $GuiDir = Join-Path $ScriptDir "gui"
    $NugetExe = Join-Path $GuiDir "nuget.exe"
    if (-not (Get-Command nuget -ErrorAction SilentlyContinue)) {
        if (-not (Test-Path $NugetExe)) {
            Write-Host "Downloading nuget.exe..." -ForegroundColor Yellow
            Invoke-WebRequest -Uri "https://dist.nuget.org/win-x86-commandline/latest/nuget.exe" -OutFile $NugetExe
        }
        $NugetCmd = $NugetExe
    } else {
        $NugetCmd = (Get-Command nuget).Source
    }

    & $NugetCmd restore (Join-Path $GuiDir "sdr2hdr_gui.vcxproj") -ConfigFile (Join-Path $GuiDir "nuget.config")
    if ($LASTEXITCODE -ne 0) { throw "NuGet restore for GUI failed." }

    $GuiBat = Join-Path $BuildDir "gui_build.bat"
    $GuiBatBody = @"
@echo off
call "$VcVars"
set "PATH=$NinjaDir;%PATH%"
set "CUDAFLAGS=-allow-unsupported-compiler %CUDAFLAGS%"
"$vsRootGuess\MSBuild\Current\Bin\MSBuild.exe" "$GuiDir\sdr2hdr_gui.vcxproj" /p:Configuration=$Config /p:Platform=x64 /p:Sdr2HdrBuildDir="$BuildDir" /p:Sdr2HdrSourceDir="$ScriptDir" /m /v:minimal
"@
    # Resolve MSBuild from same VS as vcvars
    $VsRootFromVc = ($VcVars -replace "\\VC\\Auxiliary\\Build\\vcvars64\.bat$","")
    $MsbuildPath = Join-Path $VsRootFromVc "MSBuild\Current\Bin\MSBuild.exe"
    if (-not (Test-Path $MsbuildPath)) {
        throw "MSBuild.exe not found at $MsbuildPath (WinUI GUI requires Visual Studio with Windows App SDK / WinUI workload)."
    }
    $GuiBatBody = @"
@echo off
call "$VcVars"
"$MsbuildPath" "$GuiDir\sdr2hdr_gui.vcxproj" /p:Configuration=$Config /p:Platform=x64 /p:Sdr2HdrBuildDir="$BuildDir" /p:Sdr2HdrSourceDir="$ScriptDir" /m /v:minimal
"@
    Set-Content -Path $GuiBat -Value $GuiBatBody -Encoding Ascii
    & cmd.exe /c "`"$GuiBat`""
    if ($LASTEXITCODE -ne 0) {
        Write-Host ""
        Write-Host "[!] GUI build failed. Install Visual Studio with 'Windows application development' workload" -ForegroundColor Yellow
        Write-Host "    (includes WinUI 3, C++/WinRT, Windows App SDK). Then open gui/sdr2hdr_gui.vcxproj." -ForegroundColor Yellow
        throw "GUI build failed (exit $LASTEXITCODE)."
    }
}

# 5. Report output -----------------------------------------------------------
$ExePath = Join-Path $BuildDir "sdr2hdr.exe"
if (Test-Path $ExePath) {
    Write-Host "`nBuilt: $ExePath" -ForegroundColor Green
    Write-Host "Try: `"$ExePath`" input.mp4 output.mp4 --hdr"
} else {
    throw "Build completed but sdr2hdr.exe not found at $ExePath"
}

if ($GUI) {
    $GuiExe = Join-Path $ScriptDir "gui\x64\$Config\sdr2hdr.exe"
    if (-not (Test-Path $GuiExe)) {
        $GuiExe = Join-Path $ScriptDir "gui\out\$Config\sdr2hdr_gui\sdr2hdr.exe"
    }
    if (Test-Path $GuiExe) {
        Write-Host "GUI:   $GuiExe" -ForegroundColor Green
        if ($isChinese) {
            Write-Host "       这是合并后的单一二进制：双击打开图形界面，带参数则当命令行用。" -ForegroundColor DarkGray
            Write-Host "       例如: `"$GuiExe`" input.mp4 output.mp4 --hdr" -ForegroundColor DarkGray
            Write-Host "       (上面的控制台版 sdr2hdr.exe 仅用于无界面/CI 场景，发布只需带上这一个 GUI 程序。)" -ForegroundColor DarkGray
        } else {
            Write-Host "       This is the merged single binary: double-click for the GUI, pass" -ForegroundColor DarkGray
            Write-Host "       arguments to use it as the command-line tool. e.g." -ForegroundColor DarkGray
            Write-Host "       `"$GuiExe`" input.mp4 output.mp4 --hdr" -ForegroundColor DarkGray
            Write-Host "       (The console sdr2hdr.exe above is only for headless/CI use; ship just this one.)" -ForegroundColor DarkGray
        }
    } else {
        Write-Host "GUI build requested; if sdr2hdr_gui.exe is missing, open gui/sdr2hdr_gui.vcxproj in Visual Studio (WinUI workload + nuget restore)." -ForegroundColor Yellow
    }
}
