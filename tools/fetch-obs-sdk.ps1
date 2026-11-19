# fetch-obs-sdk.ps1 -- build a minimal OBS plugin SDK from an installed OBS Studio
#
# Why this exists: OBS ships no import library (obs.lib) and no headers with the
# installer, and building libobs from source takes ~1h plus ~2GB of deps. This
# script takes the headers from the matching source tag and regenerates obs.lib
# from the installed obs.dll exports (dumpbin + lib), which is enough to link a
# video filter plugin.
#
# NOTE: keep this file ASCII-only. Windows PowerShell 5.1 reads .ps1 without a
# BOM as ANSI, and non-ASCII comments can break parsing.
#
# Usage:
#   .\tools\fetch-obs-sdk.ps1
#   .\tools\fetch-obs-sdk.ps1 -ObsDir "D:\obs-portable" -OutDir "E:\obs-sdk"
#   .\tools\fetch-obs-sdk.ps1 -Version 31.1.2
#
# Output layout (matches -DOBS_SDK_DIR=<OutDir>):
#   <OutDir>\include\*.h, include\graphics\*.h, include\util\*.h
#   <OutDir>\lib\obs.lib

param(
    [string]$ObsDir,
    [string]$OutDir = (Join-Path (Split-Path -Parent $PSScriptRoot) "sdk"),
    [string]$Version,
    [switch]$KeepSource
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
try { [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12 } catch { }

function Fail($msg) {
    Write-Host "[x] $msg" -ForegroundColor Red
    exit 1
}

function Download-File($url, $dest) {
    # curl first: it is a Windows built-in and far more reliable than
    # Invoke-WebRequest on PowerShell 5.1 (TLS / proxy quirks).
    $curl = Join-Path $env:SystemRoot "System32\curl.exe"
    if (Test-Path $curl) {
        $old = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        & $curl -sSL --max-time 300 -o $dest $url 2>$null
        $code = $LASTEXITCODE
        $ErrorActionPreference = $old
        if ($code -eq 0 -and (Test-Path $dest) -and (Get-Item $dest).Length -gt 1MB) { return $true }
    }
    try {
        Invoke-WebRequest -Uri $url -OutFile $dest -UseBasicParsing
    } catch {
        return $false
    }
    return ((Test-Path $dest) -and (Get-Item $dest).Length -gt 1MB)
}

# ---------------------------------------------------------------- 1. locate OBS
if (-not $ObsDir) {
    $obsCandidates = @(
        (Join-Path $env:ProgramFiles "obs-studio"),
        (Join-Path ${env:ProgramFiles(x86)} "obs-studio")
    )
    foreach ($c in $obsCandidates) {
        if (Test-Path (Join-Path $c "bin\64bit\obs.dll")) { $ObsDir = $c; break }
    }
}
if (-not $ObsDir -or -not (Test-Path (Join-Path $ObsDir "bin\64bit\obs.dll"))) {
    Fail "OBS not found. Install OBS Studio first, or pass -ObsDir <path>"
}
$obsDll = Join-Path $ObsDir "bin\64bit\obs.dll"
Write-Host "[*] OBS: $ObsDir" -ForegroundColor Cyan

# ------------------------------------------------------------ 2. resolve version
if (-not $Version) {
    $vi = (Get-Item $obsDll).VersionInfo
    $raw = $vi.ProductVersion
    if (-not $raw) { $raw = $vi.FileVersion }
    if ($raw -match '(\d+\.\d+\.\d+)') {
        $Version = $Matches[1]
    } else {
        Fail "Cannot detect OBS version from obs.dll. Pass -Version <x.y.z> explicitly."
    }
}
Write-Host "[*] version: $Version" -ForegroundColor Cyan

# ------------------------------------------------------- 3. locate MSVC toolset
# Do not rely on the vswhere product registry: the Visual Studio installer does
# not write it until the very end, while the toolset is already usable. Ask
# vswhere first, then scan the standard install roots directly.
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

$vcvars = $null
$vsPath = $null
foreach ($c in $probe) {
    $v = Join-Path $c "VC\Auxiliary\Build\vcvars64.bat"
    if (Test-Path $v) { $vcvars = $v; $vsPath = $c; break }
}
if (-not $vcvars) {
    Fail "MSVC C++ toolset not found. Install 'Desktop development with C++' (VCTools workload) and wait for the Visual Studio Installer to finish."
}
Write-Host "[*] MSVC: $vsPath" -ForegroundColor Cyan

# Windows SDK is a hard link-time dependency (windows.h, ucrt). Detect it early
# so the user gets a readable cause instead of a wall of linker errors.
$sdkInc = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\Include"
if (-not (Test-Path $sdkInc)) {
    $setup = @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -match 'vs_setup|winsdksetup' })
    if ($setup.Count -gt 0) {
        Fail "Windows SDK is still being installed (setup processes running). Wait for the Visual Studio Installer to finish, then re-run."
    }
    Fail "Windows SDK not found at $sdkInc. Add the Windows SDK component to Visual Studio Build Tools."
}

# --------------------------------------------------- 4. import MSVC environment
$envDump = cmd /c "`"$vcvars`" >nul 2>&1 && set"
foreach ($line in $envDump) {
    if ($line -match '^([^=]+)=(.*)$') {
        Set-Item -Path ("Env:" + $Matches[1]) -Value $Matches[2] -ErrorAction SilentlyContinue
    }
}
foreach ($tool in @("dumpbin.exe", "lib.exe")) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        # vcvars can fail silently while the SDK is still unregistered; fall back
        # to the newest MSVC toolset directory directly.
        $msvcRoot = Join-Path $vsPath "VC\Tools\MSVC"
        if (Test-Path $msvcRoot) {
            $newest = Get-ChildItem $msvcRoot -Directory | Sort-Object Name -Descending | Select-Object -First 1
            $bin = Join-Path $newest.FullName "bin\Hostx64\x64"
            if (Test-Path $bin) { $env:Path = "$bin;$env:Path" }
        }
    }
}
foreach ($tool in @("dumpbin.exe", "lib.exe")) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { Fail "$tool not found (MSVC toolset incomplete)" }
}

# ------------------------------------------------------------- 5. fetch headers
$work = Join-Path ([System.IO.Path]::GetTempPath()) ("obs-sdk-" + [Guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory -Force -Path $work | Out-Null

$zip = Join-Path $work "obs.zip"
$url = "https://github.com/obsproject/obs-studio/archive/refs/tags/$Version.zip"
Write-Host "[*] downloading headers: $url" -ForegroundColor Cyan
if (-not (Download-File $url $zip)) {
    Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
    Fail "Download failed. Check that the tag exists: https://github.com/obsproject/obs-studio/tags"
}

# Expand-Archive: tar cannot be used here, the repo contains symlinks that fail on Windows
$oldProgress = $ProgressPreference
$ProgressPreference = "SilentlyContinue"
Expand-Archive -Path $zip -DestinationPath $work -Force
$ProgressPreference = $oldProgress
$srcRoot = Get-ChildItem $work -Directory | Where-Object { $_.Name -like "obs-studio-*" } | Select-Object -First 1
if (-not $srcRoot) { Fail "Unexpected archive layout" }

$libobs = Join-Path $srcRoot.FullName "libobs"
if (-not (Test-Path (Join-Path $libobs "obs.h"))) { Fail "libobs headers not found in archive" }

$inc = Join-Path $OutDir "include"
$lib = Join-Path $OutDir "lib"
# Overwrite in place instead of wiping the tree first (bulk recursive deletes are
# blocked by some host policies and are not needed: robocopy mirrors the headers).
New-Item -ItemType Directory -Force -Path $inc, $lib | Out-Null

# copy headers only, flattening libobs/ into include/
robocopy $libobs $inc /E /XF *.c *.cpp *.m *.mm *.rc *.in *.txt /NFL /NDL /NJH /NJS /NP | Out-Null
# obsconfig.h is generated by CMake upstream; provide the values a plugin needs
$obsconfig = @(
    "#pragma once",
    "#define OBS_DATA_PATH `"data`"",
    "#define OBS_INSTALL_PREFIX `"`"",
    "#define OBS_PLUGIN_DESTINATION `"obs-plugins/64bit`"",
    "#define OBS_RELEASE_CANDIDATE 0",
    "#define OBS_BETA 0"
)
Set-Content -Path (Join-Path $inc "obsconfig.h") -Value $obsconfig -Encoding ASCII

$headerCount = @(Get-ChildItem $inc -Recurse -Filter *.h).Count
if ($headerCount -lt 50) { Fail "Only $headerCount headers copied - robocopy step failed" }
Write-Host "[*] headers: $headerCount files -> $inc" -ForegroundColor Cyan

# --------------------------------------------------- 6. regenerate obs.lib (.def)
$defPath = Join-Path $work "obs.def"
$exports = & dumpbin /nologo /exports $obsDll
$names = @()
foreach ($line in $exports) {
    # entries look like: "   12   A 00080690 some_symbol = some_symbol"
    if ($line -match '^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\S+)(\s+=\s+\S+)?\s*$') {
        $names += $Matches[1]
    }
}
if ($names.Count -lt 100) { Fail "Only $($names.Count) exports parsed from obs.dll - unexpected dumpbin output" }

$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine("LIBRARY obs.dll")
[void]$sb.AppendLine("EXPORTS")
foreach ($n in $names) { [void]$sb.AppendLine("  $n") }
[System.IO.File]::WriteAllText($defPath, $sb.ToString(), [System.Text.Encoding]::ASCII)

Push-Location $work
& lib /nologo /def:obs.def /machine:x64 /out:obs.lib | Out-Null
$rc = $LASTEXITCODE
Pop-Location
if ($rc -ne 0) { Fail "lib.exe failed generating obs.lib" }

Move-Item (Join-Path $work "obs.lib") (Join-Path $lib "obs.lib") -Force
Write-Host "[*] obs.lib: $($names.Count) exports" -ForegroundColor Cyan

# ------------------------------------------------------------- 7. sanity check
$expected = @(
    "obs_register_source_s",
    "obs_source_process_filter_begin",
    "obs_filter_get_target",
    "gs_texrender_create",
    "gs_stagesurface_map",
    "obs_properties_add_list"
)
$missing = @()
foreach ($e in $expected) {
    if ($names -notcontains $e) { $missing += $e }
}
if ($missing.Count -gt 0) {
    Write-Host "[!] missing symbols: $($missing -join ', ')" -ForegroundColor Yellow
    Write-Host "    this OBS build may be incompatible with the plugin" -ForegroundColor Yellow
} else {
    Write-Host "[+] all probe symbols present" -ForegroundColor Green
}

if (-not $KeepSource) {
    try { Remove-Item $work -Recurse -Force -ErrorAction Stop }
    catch { Write-Host "[!] temp left behind: $work" -ForegroundColor DarkGray }
}

Write-Host ""
Write-Host "[+] SDK ready: $OutDir" -ForegroundColor Green
Write-Host "    next: .\build.ps1 -SdkDir `"$OutDir`""
