# deploy.ps1 -- copy the built plugin into an OBS installation
#
#   .\deploy.ps1 -ObsDir "C:\Program Files\obs-studio"
#   .\deploy.ps1 -ObsDir "D:\obs-portable" -BuildDir build_x64

param(
    [Parameter(Mandatory = $true)][string]$ObsDir,
    [string]$BuildDir = "build_x64",
    [string]$Config   = "RelWithDebInfo",
    [switch]$Force
)

$ErrorActionPreference = "Stop"
# scripts/ lives one level below the project root
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$rundir = Join-Path $root "$BuildDir\rundir"

if (-not (Test-Path $rundir)) {
    Write-Host "[x] build output not found: $rundir" -ForegroundColor Red
    Write-Host "    run .\build.ps1 first" -ForegroundColor Yellow
    exit 1
}
$exeCandidates = @(
    (Join-Path $ObsDir "obs64.exe"),
    (Join-Path $ObsDir "bin\64bit\obs64.exe")
)
if (-not ($exeCandidates | Where-Object { Test-Path $_ })) {
    Write-Host "[!] $ObsDir does not look like an OBS install (obs64.exe not found)" -ForegroundColor Yellow
    if (-not $Force) { Write-Host "    pass -Force to continue anyway"; exit 1 }
}

$running = Get-Process obs64 -ErrorAction SilentlyContinue
if ($running) {
    Write-Host "[!] OBS is running (pid $($running.Id)). Close it before deploying." -ForegroundColor Yellow
    if (-not $Force) { exit 1 }
}

$dll = Join-Path $rundir "obs-plugins\64bit\obs-guard.dll"
if (-not (Test-Path $dll)) {
    Write-Host "[x] $dll not found" -ForegroundColor Red
    exit 1
}

$dstPlugin = Join-Path $ObsDir "obs-plugins\64bit"
$dstData   = Join-Path $ObsDir "data\obs-plugins\obs-guard"

try {
    New-Item -ItemType Directory -Force -Path $dstPlugin -ErrorAction Stop | Out-Null
    New-Item -ItemType Directory -Force -Path $dstData -ErrorAction Stop | Out-Null

    Copy-Item $dll $dstPlugin -Force -ErrorAction Stop
    Copy-Item (Join-Path $rundir "data\obs-plugins\obs-guard\*") $dstData -Recurse -Force -ErrorAction Stop

    # ONNX Runtime runtime DLLs sit next to the plugin
    Get-ChildItem (Join-Path $rundir "obs-plugins\64bit") -Filter *.dll | ForEach-Object {
        if ($_.Name -ne "obs-guard.dll") {
            Copy-Item $_.FullName $dstPlugin -Force -ErrorAction Stop
        }
    }
} catch {
    Write-Host "[x] write access denied: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host ""
    Write-Host "    $ObsDir is protected. Close OBS, then run this from an" -ForegroundColor Yellow
    Write-Host "    elevated PowerShell (right click -> Run as administrator):" -ForegroundColor Yellow
    Write-Host ""
    Write-Host "      cd `"$(Split-Path -Parent $MyInvocation.MyCommand.Path)`"" -ForegroundColor White
    Write-Host "      .\deploy.ps1 -ObsDir `"$ObsDir`" -BuildDir `"$BuildDir`"" -ForegroundColor White
    exit 1
}

Write-Host "[+] deployed to $ObsDir" -ForegroundColor Green
Write-Host "    plugin : $dstPlugin\obs-guard.dll"
Write-Host "    data   : $dstData"
Write-Host ""
Write-Host "Next: start OBS -> select the source -> Filters -> + -> 'Content Mask'"
