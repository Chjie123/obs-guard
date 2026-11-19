# install.ps1 -- one-click installer for the obs-guard OBS filter
#
# Normally launched by install.cmd. Elevates itself when needed.
#
#   install.cmd                    normal use (double click)
#   .\install.ps1 -ObsDir <path>   non-standard OBS location
#   .\install.ps1 -Uninstall       remove the plugin
#   .\install.ps1 -NoElevate ...   only for testing against a writable dir
#
# Keep this file ASCII-only: Windows PowerShell 5.1 reads .ps1 without a BOM as
# ANSI and non-ASCII characters can break parsing.

param(
    [string]$ObsDir,
    [switch]$Uninstall,
    [switch]$NoElevate,
    [switch]$NoLaunch,
    [switch]$Silent,
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$marker = "obs-guard"

function Fail($m) {
    Write-Host ""
    Write-Host "[x] $m" -ForegroundColor Red
    exit 1
}
function Info($m) { Write-Host "[*] $m" -ForegroundColor Cyan }
function Ok($m)   { Write-Host "[+] $m" -ForegroundColor Green }

Write-Host ""
Write-Host "  obs-guard -- OBS content mask filter" -ForegroundColor White
Write-Host "  -----------------------------------" -ForegroundColor DarkGray

# ---------------------------------------------------------------------------
# 1. elevation
# ---------------------------------------------------------------------------
$isAdmin = $false
try {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $isAdmin = (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
} catch { $isAdmin = $false }

if (-not $isAdmin -and -not $NoElevate) {
    Info "requesting administrator privileges..."
    $argList = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-NoExit", "-File", ('"' + $PSCommandPath + '"'))
    if ($ObsDir)    { $argList += @("-ObsDir", ('"' + $ObsDir + '"')) }
    if ($Uninstall) { $argList += "-Uninstall" }
    if ($NoLaunch)  { $argList += "-NoLaunch" }
    try {
        # no -Wait: the elevated window is the UI, this one just gets out of the way
        Start-Process -FilePath "powershell.exe" -ArgumentList $argList -Verb RunAs | Out-Null
    } catch {
        Fail "elevation was cancelled. Run install.cmd as administrator."
    }
    exit 0
}

# ---------------------------------------------------------------------------
# 2. locate OBS
# ---------------------------------------------------------------------------
function Test-ObsDir($p) {
    if (-not $p) { return $false }
    if (Test-Path (Join-Path $p "bin\64bit\obs.dll")) { return $true }
    if (Test-Path (Join-Path $p "obs64.exe")) { return $true }
    return $false
}

if ($ObsDir -and -not (Test-ObsDir $ObsDir)) {
    Fail "not a valid OBS directory: $ObsDir"
}

if (-not $ObsDir) {
    $candidates = @()
    foreach ($k in @(
        "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\OBS Studio",
        "HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\OBS Studio"
    )) {
        $v = (Get-ItemProperty -Path $k -ErrorAction SilentlyContinue).InstallLocation
        if ($v) { $candidates += $v }
    }
    $candidates += @(
        (Join-Path $env:ProgramFiles "obs-studio"),
        (Join-Path ${env:ProgramFiles(x86)} "obs-studio")
    )
    foreach ($c in $candidates) {
        if (Test-ObsDir $c) { $ObsDir = $c; break }
    }
}

if (-not $ObsDir) {
    Write-Host "[!] OBS installation not found in the usual places." -ForegroundColor Yellow
    if ($Silent) { Fail "pass -ObsDir <path>" }
    $ObsDir = (Read-Host "Enter the OBS install folder (the one containing bin\ and data\)").Trim('"', ' ')
    if (-not (Test-ObsDir $ObsDir)) { Fail "not a valid OBS directory: $ObsDir" }
}
Info "OBS: $ObsDir"

$dstPlugin = Join-Path $ObsDir "obs-plugins\64bit"
$dstData   = Join-Path $ObsDir "data\obs-plugins\$marker"
$manifest  = Join-Path $dstPlugin "$marker.manifest.txt"

# ---------------------------------------------------------------------------
# 3. OBS must be closed: a loaded plugin DLL cannot be replaced or deleted,
#    and administrator rights do not change that (sharing violation, not ACL).
# ---------------------------------------------------------------------------
$running = @(Get-Process obs64 -ErrorAction SilentlyContinue)
if ($running.Count -gt 0 -and -not $Force) {
    Write-Host "[!] OBS is running (pid $($running[0].Id))." -ForegroundColor Yellow
    Write-Host "    the plugin DLL is loaded into that process and cannot be removed." -ForegroundColor Yellow
    if ($Silent) { Fail "close OBS first, or pass -Force" }
    $ans = Read-Host "Close OBS now? [y/N]"
    if ($ans -match '^[yY]') {
        $running | Stop-Process -Force
        Start-Sleep -Milliseconds 1500
        Info "OBS closed"
    } else {
        Fail "aborted. Close OBS and run again."
    }
} elseif ($running.Count -gt 0) {
    Info "OBS is running; continuing because -Force was given (deletion may fail)"
}

function Remove-Tracked($path) {
    if (-not (Test-Path $path)) { return $true }
    try {
        Remove-Item $path -Force -Recurse -ErrorAction Stop
        return $true
    } catch {
        $hr = $_.Exception.HResult
        if ($hr -eq -2147024864) {
            Write-Host "[!] in use: $path" -ForegroundColor Yellow
            Write-Host "    (file is locked by a running process, most likely OBS)" -ForegroundColor Yellow
        } elseif ($hr -eq -2147024891) {
            Write-Host "[!] access denied: $path" -ForegroundColor Yellow
            Write-Host "    (needs administrator rights)" -ForegroundColor Yellow
        } else {
            Write-Host "[!] failed: $path" -ForegroundColor Yellow
            Write-Host "    ($($_.Exception.Message))" -ForegroundColor Yellow
        }
        return $false
    }
}

# ---------------------------------------------------------------------------
# 4. uninstall
# ---------------------------------------------------------------------------
if ($Uninstall) {
    $removed = 0
    $failed = @()

    # Build one de-duplicated target list: manifest entries (if any) plus the
    # known file names, so a hand-copied install is still fully removed.
    $names = @()
    if (Test-Path $manifest) {
        $names += @(Get-Content $manifest | Where-Object { $_.Trim() } | ForEach-Object { $_.Trim() })
    }
    if ($names -notcontains "$marker.dll") { $names += "$marker.dll" }

    foreach ($name in $names) {
        $f = Join-Path $dstPlugin $name
        if (-not (Test-Path $f)) { continue }
        if (Remove-Tracked $f) { $removed++ } else { $failed += $f }
    }
    if (Test-Path $dstData) {
        if (Remove-Tracked $dstData) { $removed++ } else { $failed += $dstData }
    }

    if ($failed.Count -gt 0) {
        Write-Host ""
        Fail "$($failed.Count) item(s) could not be removed. Close OBS completely (including the tray icon) and run uninstall.cmd again; if it still fails, the file may be held by an antivirus scan."
    }

    # keep the manifest until everything above succeeded, so a retry still has it
    if (Test-Path $manifest) {
        if (Remove-Tracked $manifest) { $removed++ }
    }

    Write-Host ""
    Ok "uninstalled ($removed item(s) removed)"
    Write-Host "    plugin : $dstPlugin\$marker.dll (gone)"
    Write-Host "    data   : $dstData (gone)"
    exit 0
}

# ---------------------------------------------------------------------------
# 5. sanity checks
# ---------------------------------------------------------------------------
$srcDll = Join-Path $root "obs-plugins\64bit\obs-guard.dll"
if (-not (Test-Path $srcDll)) { Fail "package is incomplete: obs-guard.dll missing next to install.ps1" }

# not elevated and the target is not writable -> let the copy error explain it
try {
    New-Item -ItemType Directory -Force -Path $dstPlugin, $dstData -ErrorAction Stop | Out-Null
} catch {
    Fail "cannot write to $dstPlugin. Run install.cmd as administrator."
}

# ---------------------------------------------------------------------------
# 5. install
# ---------------------------------------------------------------------------
$installed = @()

Get-ChildItem (Join-Path $root "obs-plugins\64bit") -Filter *.dll | ForEach-Object {
    Copy-Item $_.FullName $dstPlugin -Force
    $installed += $_.Name
    Info ("installed {0} ({1:N0} KB)" -f $_.Name, ($_.Length / 1KB))
}

if (Test-Path (Join-Path $root "data\obs-plugins\$marker")) {
    Copy-Item (Join-Path $root "data\obs-plugins\$marker\*") $dstData -Recurse -Force
}
Set-Content -Path $manifest -Value $installed -Encoding ASCII

# verify the two files that actually matter
$okDll = Test-Path (Join-Path $dstPlugin "obs-guard.dll")
$okFx  = Test-Path (Join-Path $dstData "effects\guard_mask.effect")
if (-not $okDll) { Fail "copy failed: obs-guard.dll not present after install" }
if (-not $okFx)  { Fail "copy failed: mask effect not present after install" }

Write-Host ""
Ok "obs-guard installed to $ObsDir"
Write-Host "    plugin : $dstPlugin\obs-guard.dll"
Write-Host "    data   : $dstData"
Write-Host ""
Write-Host "  Next:" -ForegroundColor White
Write-Host "    1. start OBS"
Write-Host "    2. select the source you want to protect -> Filters -> + -> 'Content Mask'"
Write-Host "    3. tick 'Show detection region' to confirm it is live"
Write-Host ""

if (-not $NoLaunch -and -not $Silent) {
    $ans = Read-Host "Launch OBS now? [Y/n]"
    if ($ans -notmatch '^[nN]') {
        $exe = Join-Path $ObsDir "bin\64bit\obs64.exe"
        if (Test-Path $exe) {
            Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe -Parent) | Out-Null
        }
    }
}
