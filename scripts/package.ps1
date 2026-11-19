# package.ps1 -- assemble a distributable zip for obs-guard
#
# Produces:
#   dist\obs-guard-<version>\            (overlay layout, also usable by hand-copy)
#   dist\obs-guard-<version>.zip         (what you hand to other people)
#
# Usage:
#   .\package.ps1                          # build + package
#   .\package.ps1 -SkipBuild
#   .\package.ps1 -BuildDir build_x64-nmake -SkipBuild
#
# The package layout mirrors an OBS installation, so extracting the zip over the
# OBS folder (or running install.cmd) both work.

param(
    [string]$BuildDir = "build_x64-nmake",
    [string]$OutDir   = "dist",
    [string]$Version,
    [switch]$SkipBuild,
    [switch]$NoOrt
)

$ErrorActionPreference = "Stop"
# scripts/ lives one level below the project root
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)

function Fail($m) { Write-Host "[x] $m" -ForegroundColor Red; exit 1 }
function Info($m) { Write-Host "[*] $m" -ForegroundColor Cyan }

if (-not $Version) {
    $cml = Get-Content (Join-Path $root "CMakeLists.txt") -Raw
    if ($cml -match 'project\(obs-guard VERSION ([0-9]+\.[0-9]+\.[0-9]+)') {
        $Version = $Matches[1]
    } else {
        $Version = "0.0.0"
    }
}
Info "version: $Version"

# ---------------------------------------------------------------------------
# 1. build
# ---------------------------------------------------------------------------
if (-not $SkipBuild) {
    $buildArgs = @{ BuildDir = $BuildDir }
    $sdk = Join-Path $root "sdk"
    if (Test-Path (Join-Path $sdk "lib\obs.lib")) { $buildArgs.SdkDir = $sdk }
    if ($NoOrt) { $buildArgs.NoOrt = $true }
    & (Join-Path $root "scripts\build.ps1") @buildArgs
    if ($LASTEXITCODE -ne 0) { Fail "build failed" }
}

$rundir = Join-Path $root "$BuildDir\rundir"
$srcDll = Join-Path $rundir "obs-plugins\64bit\obs-guard.dll"
$srcData = Join-Path $rundir "data\obs-plugins\obs-guard"
if (-not (Test-Path $srcDll)) { Fail "plugin not found: $srcDll (run .\build.ps1 first)" }
if (-not (Test-Path $srcData)) { Fail "data dir not found: $srcData" }

# ---------------------------------------------------------------------------
# 2. stage
# ---------------------------------------------------------------------------
$stage = Join-Path $root (Join-Path $OutDir "obs-guard-$Version")
$pPlugin = Join-Path $stage "obs-plugins\64bit"
$pData = Join-Path $stage "data\obs-plugins\obs-guard"
New-Item -ItemType Directory -Force -Path $pPlugin, $pData | Out-Null

# plugin + any runtime DLLs sitting next to it (onnxruntime, DirectML)
Get-ChildItem (Join-Path $rundir "obs-plugins\64bit") -Filter *.dll | ForEach-Object {
    Copy-Item $_.FullName $pPlugin -Force
}
Copy-Item (Join-Path $srcData "*") $pData -Recurse -Force

# installer entry points
foreach ($f in @("install.cmd", "install.ps1", "uninstall.cmd")) {
    $src = Join-Path $root "packaging\$f"
    if (Test-Path $src) { Copy-Item $src $stage -Force }
}

$readmeSrc = Join-Path $root "docs\install-readme.txt"
if (-not (Test-Path $readmeSrc)) { Fail "docs\install-readme.txt missing" }
$readme = (Get-Content $readmeSrc -Encoding UTF8) -replace '@VERSION@', $Version
Set-Content -Path (Join-Path $stage "README.txt") -Value $readme -Encoding UTF8

Info "staged: $stage"

# ---------------------------------------------------------------------------
# 3. zip
# ---------------------------------------------------------------------------
$zip = Join-Path $root (Join-Path $OutDir "obs-guard-$Version.zip")
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip -Force
$size = (Get-Item $zip).Length

Write-Host ""
Write-Host "[+] package ok" -ForegroundColor Green
Write-Host ("    zip    : {0} ({1:N0} KB)" -f $zip, ($size / 1KB))
Write-Host "    layout : obs-plugins/64bit/obs-guard.dll + data/obs-plugins/obs-guard/"
Write-Host "    entry  : install.cmd"
