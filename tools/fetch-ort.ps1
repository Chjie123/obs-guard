# fetch-ort.ps1 -- assemble an ONNX Runtime (DirectML) SDK for the build
#
# Why: ONNX Runtime no longer publishes a usable Windows zip on GitHub releases
# for every version, but the official NuGet packages still carry the DLL, the
# import library and the C/C++ headers. This script lays them out in the shape
# CMake expects:
#
#   sdk\ort\include\*.h
#   sdk\ort\lib\onnxruntime.lib
#   sdk\ort\lib\onnxruntime.dll
#   sdk\ort\lib\DirectML.dll
#
# Usage:
#   .\tools\fetch-ort.ps1
#   .\tools\fetch-ort.ps1 -Version 1.19.2 -DirectMlVersion 1.15.4
#
# Keep this file ASCII-only (Windows PowerShell 5.1 reads .ps1 without a BOM as ANSI).

param(
    [string]$Version = "1.19.2",
    [string]$DirectMlVersion = "1.15.4",
    [string]$OutDir
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

function Fail($m) { Write-Host "[x] $m" -ForegroundColor Red; exit 1 }
function Info($m) { Write-Host "[*] $m" -ForegroundColor Cyan }
function Ok($m) { Write-Host "[+] $m" -ForegroundColor Green }

if (-not $OutDir) {
    $OutDir = Join-Path (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)) "sdk\ort"
}

function Download-File($url, $dest) {
    $curl = Join-Path $env:SystemRoot "System32\curl.exe"
    if (Test-Path $curl) {
        $old = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        & $curl -sSL --max-time 600 -o $dest $url 2>$null
        $code = $LASTEXITCODE
        $ErrorActionPreference = $old
        if ($code -eq 0 -and (Test-Path $dest) -and (Get-Item $dest).Length -gt 1MB) { return $true }
    }
    try { Invoke-WebRequest -Uri $url -OutFile $dest -UseBasicParsing } catch { return $false }
    return ((Test-Path $dest) -and (Get-Item $dest).Length -gt 1MB)
}

$work = Join-Path ([System.IO.Path]::GetTempPath()) ("ort-sdk-" + [Guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory -Force -Path $work | Out-Null
$tar = Join-Path $env:SystemRoot "System32\tar.exe"
if (-not (Test-Path $tar)) { Fail "tar.exe not found (Windows 10 1803 or newer is required)" }

# ---------------------------------------------------------------------------
# 1. ONNX Runtime: headers + x64 runtime + import library
# ---------------------------------------------------------------------------
$ortPkg = Join-Path $work "ort.nupkg"
Info "downloading Microsoft.ML.OnnxRuntime.DirectML $Version"
if (-not (Download-File "https://www.nuget.org/api/v2/package/Microsoft.ML.OnnxRuntime.DirectML/$Version" $ortPkg)) {
    Fail "download failed. Check the version: https://www.nuget.org/packages/Microsoft.ML.OnnxRuntime.DirectML"
}

$inc = Join-Path $OutDir "include"
$lib = Join-Path $OutDir "lib"
New-Item -ItemType Directory -Force -Path $inc, $lib | Out-Null

& $tar -xf $ortPkg -C $work "build/native/include" 2>$null
$srcInc = Join-Path $work "build\native\include"
if (-not (Test-Path $srcInc)) { Fail "unexpected package layout: build/native/include missing" }
Copy-Item (Join-Path $srcInc "*.h") $inc -Force

& $tar -xf $ortPkg -C $work "runtimes/win-x64/native/onnxruntime.dll" "runtimes/win-x64/native/onnxruntime.lib" 2>$null
$srcNative = Join-Path $work "runtimes\win-x64\native"
Copy-Item (Join-Path $srcNative "onnxruntime.dll") $lib -Force
Copy-Item (Join-Path $srcNative "onnxruntime.lib") $lib -Force

# ---------------------------------------------------------------------------
# 2. DirectML: the runtime DLL is a separate package
# ---------------------------------------------------------------------------
$dmPkg = Join-Path $work "directml.nupkg"
Info "downloading Microsoft.AI.DirectML $DirectMlVersion"
if (-not (Download-File "https://www.nuget.org/api/v2/package/Microsoft.AI.DirectML/$DirectMlVersion" $dmPkg)) {
    Fail "download failed. Check the version: https://www.nuget.org/packages/Microsoft.AI.DirectML"
}

& $tar -xf $dmPkg -C $work "bin/x64-win/DirectML.dll" 2>$null
$dmDll = Join-Path $work "bin\x64-win\DirectML.dll"
if (-not (Test-Path $dmDll)) { Fail "unexpected package layout: bin/x64-win/DirectML.dll missing" }
Copy-Item $dmDll $lib -Force

# ---------------------------------------------------------------------------
# 3. verify
# ---------------------------------------------------------------------------
$headers = @(Get-ChildItem $inc -Filter *.h)
if ($headers.Count -lt 5) { Fail "only $($headers.Count) headers extracted" }
foreach ($need in @("onnxruntime_cxx_api.h", "dml_provider_factory.h")) {
    if (-not (Test-Path (Join-Path $inc $need))) { Fail "$need missing" }
}
foreach ($need in @("onnxruntime.dll", "onnxruntime.lib", "DirectML.dll")) {
    $p = Join-Path $lib $need
    if (-not (Test-Path $p)) { Fail "$need missing" }
    Info ("{0,-18} {1,7:N1} MB" -f $need, ((Get-Item $p).Length / 1MB))
}

try { Remove-Item $work -Recurse -Force -ErrorAction Stop } catch { }

Ok "ORT SDK ready: $OutDir"
Write-Host "    next: .\scripts\build.ps1 -SdkDir .\sdk -OrtRoot .\sdk\ort"
