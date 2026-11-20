# build.ps1 -- configure + build obs-guard
#
# Examples:
#   .\build.ps1 -SdkDir .\sdk -NoOrt
#   .\build.ps1 -ObsSourceDir E:\obs-studio -ObsBuildDir E:\obs-studio\build_x64
#   .\build.ps1 -Toolchain nmake -SdkDir .\sdk
#
# Toolchain auto-detection: the Visual Studio generator is preferred, but it
# needs a registered VS instance. If the VS installer left an unregistered tree
# (no ProgramData\...\Packages\_Instances), CMake cannot see it and this script
# falls back to NMake with a manually assembled MSVC/SDK environment.

param(
    [string]$ObsSourceDir = $env:OBS_SOURCE_DIR,
    [string]$ObsBuildDir  = $env:OBS_BUILD_DIR,
    [string]$SdkDir       = $env:OBS_SDK_DIR,
    [string]$OrtRoot      = $env:ONNXRUNTIME_ROOT,
    [string]$Config       = "RelWithDebInfo",
    [string]$BuildDir     = "build_x64",
    [ValidateSet("auto", "vs", "nmake")]
    [string]$Toolchain    = "auto",
    [switch]$NoOrt,
    [switch]$Reconfigure
)

$ErrorActionPreference = "Stop"
# scripts/ lives one level below the project root
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)

$cmake = Get-Command cmake -ErrorAction SilentlyContinue
if (-not $cmake) {
    Write-Host "[x] cmake not found in PATH. Install CMake >= 3.20 or add it to PATH." -ForegroundColor Red
    exit 1
}

# ---------------------------------------------------------------------------
# MSVC / Windows SDK environment (used by the NMake fallback)
# ---------------------------------------------------------------------------
function Get-MsvcEnvironment {
    $probe = @()
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $p = & $vswhere -latest -products * -property installationPath 2>$null
        if ($p) { $probe += @($p) }
    }
    foreach ($base in @("$env:ProgramFiles\Microsoft Visual Studio", "${env:ProgramFiles(x86)}\Microsoft Visual Studio")) {
        if (-not (Test-Path $base)) { continue }
        foreach ($year in @(Get-ChildItem $base -Directory -ErrorAction SilentlyContinue)) {
            foreach ($prod in @(Get-ChildItem $year.FullName -Directory -ErrorAction SilentlyContinue)) {
                $probe += $prod.FullName
            }
        }
    }

    foreach ($c in $probe) {
        $msvc = Join-Path $c "VC\Tools\MSVC"
        if (-not (Test-Path $msvc)) { continue }
        $ver = Get-ChildItem $msvc -Directory | Sort-Object Name -Descending | Select-Object -First 1
        if (-not $ver) { continue }
        $vcBin = Join-Path $ver.FullName "bin\Hostx64\x64"
        if (-not (Test-Path (Join-Path $vcBin "cl.exe"))) { continue }

        $sdkRoot = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10"
        $sdkVer = Get-ChildItem (Join-Path $sdkRoot "Include") -Directory -ErrorAction SilentlyContinue |
                  Where-Object { Test-Path (Join-Path $_.FullName "um") } |
                  Sort-Object Name -Descending | Select-Object -First 1
        if (-not $sdkVer) { continue }

        $env:Path = "$vcBin;" + (Join-Path $sdkRoot "bin\$($sdkVer.Name)\x64") + ";$env:Path"
        $env:INCLUDE = (@(
            (Join-Path $ver.FullName "include"),
            (Join-Path $sdkVer.FullName "ucrt"),
            (Join-Path $sdkVer.FullName "um"),
            (Join-Path $sdkVer.FullName "shared"),
            (Join-Path $sdkVer.FullName "winrt")
        ) -join ";")
        $env:LIB = (@(
            (Join-Path $ver.FullName "lib\x64"),
            (Join-Path $sdkRoot "Lib\$($sdkVer.Name)\ucrt\x64"),
            (Join-Path $sdkRoot "Lib\$($sdkVer.Name)\um\x64")
        ) -join ";")
        Write-Host "[*] toolchain: MSVC $($ver.Name) + SDK $($sdkVer.Name)" -ForegroundColor Cyan
        return $true
    }
    return $false
}

function Invoke-Configure($generator, $dir) {
    $a = @("-S", $root, "-B", (Join-Path $root $dir), "-G", $generator)
    if ($generator -like "Visual Studio*") {
        $a += @("-A", "x64")
    } else {
        $a += @("-DCMAKE_BUILD_TYPE=$Config")
    }
    if ($ObsSourceDir) { $a += @("-DOBS_SOURCE_DIR=$ObsSourceDir") }
    if ($ObsBuildDir)  { $a += @("-DOBS_BUILD_DIR=$ObsBuildDir") }
    if ($SdkDir)       { $a += @("-DOBS_SDK_DIR=$SdkDir") }
    # Pass the flag explicitly: a previous -NoOrt run would otherwise leave
    # OFF in the CMake cache and every later build would silently follow it.
    if ($NoOrt) { $a += @("-DGUARD_WITH_ORT=OFF") } else { $a += @("-DGUARD_WITH_ORT=ON") }
    if ($OrtRoot) { $a += @("-DONNXRUNTIME_ROOT=$OrtRoot") }

    Write-Host "[*] cmake $($a -join ' ')" -ForegroundColor Cyan
    $old = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    # Write-Host keeps the output out of the function's return stream, otherwise
    # the caller receives an array instead of the exit code.
    & cmake @a 2>&1 | ForEach-Object { Write-Host $_ }
    $code = $LASTEXITCODE
    $ErrorActionPreference = $old
    return $code
}

function Invoke-Build($dir) {
    $a = @("--build", (Join-Path $root $dir))
    if ($dir -notlike "*nmake*") { $a += @("--config", $Config) }
    $a += @("--parallel")
    $old = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & cmake @a 2>&1 | ForEach-Object { Write-Host $_ }
    return $LASTEXITCODE
}

# ---------------------------------------------------------------------------
# 1. configure
# ---------------------------------------------------------------------------
$dir = $BuildDir
$configured = $false

if ($Toolchain -ne "nmake") {
    $code = Invoke-Configure "Visual Studio 17 2022" $dir
    if ($code -eq 0) {
        $configured = $true
    } else {
        Write-Host "[!] Visual Studio generator failed; falling back to NMake" -ForegroundColor Yellow
        if ($Toolchain -eq "vs") { exit $code }
        if ($dir -notlike "*-nmake") { $dir = "$BuildDir-nmake" }
    }
}

if (-not $configured) {
    if (-not (Get-MsvcEnvironment)) {
        Write-Host "[x] No usable MSVC toolset found." -ForegroundColor Red
        exit 1
    }
    $code = Invoke-Configure "NMake Makefiles" $dir
    if ($code -ne 0) { exit $code }
}

# ---------------------------------------------------------------------------
# 2. build
# ---------------------------------------------------------------------------
$code = Invoke-Build $dir
if ($code -ne 0) { exit $code }

$rundir = if ([System.IO.Path]::IsPathRooted($dir)) { Join-Path $dir "rundir" } else { Join-Path $root "$dir\rundir" }
Write-Host ""
Write-Host "[+] build ok" -ForegroundColor Green
Write-Host "    build dir: $dir"
Write-Host "    plugin   : $rundir\obs-plugins\64bit\obs-guard.dll"
Write-Host "    data     : $rundir\data\obs-plugins\obs-guard"
Write-Host "    deploy   : .\scripts\deploy.ps1 -ObsDir 'C:\Program Files\obs-studio' -BuildDir '$dir'"
