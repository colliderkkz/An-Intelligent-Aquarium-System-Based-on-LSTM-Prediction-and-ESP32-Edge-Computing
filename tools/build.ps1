# =============================================================================
#  fish_tank builder  --  builds the firmware with ESP-IDF, handling the case
#                         where the project lives in a non-ASCII (Chinese) path.
# -----------------------------------------------------------------------------
#  Why this script exists:
#    ESP-IDF's toolchain (xtensa-gcc / ccache / idf tools) cannot handle
#    non-ASCII paths on Windows: gcc fails to open "-IC:/.../总/..." and ccache
#    throws "filesystem error: Cannot convert character sequence".
#    Workaround: mirror the sources into an ASCII build directory, build
#    there, then copy the images back into <project>\build\.
#
#  Usage:
#    powershell -ExecutionPolicy Bypass -File tools\build.ps1
#    powershell -ExecutionPolicy Bypass -File tools\build.ps1 -Clean
#    powershell -ExecutionPolicy Bypass -File tools\build.ps1 -BuildDir D:\my_build
#
#  Output:
#    <project>\build\bootloader\bootloader.bin        -> flash at 0x0
#    <project>\build\partition_table\partition-table.bin -> flash at 0x8000
#    <project>\build\fish_gundam_panzer.bin           -> flash at 0x10000
#
#  NOTE: ASCII-only on purpose (PowerShell 5.1 reads BOM-less files as ANSI).
# =============================================================================
[CmdletBinding()]
param(
    [string]$ProjectDir = '',
    [string]$BuildDir = 'D:\fish_tank_build',
    [switch]$Clean,
    [switch]$NoCopyBack
)

$ErrorActionPreference = 'Stop'

function Write-Step($m)  { Write-Host ""; Write-Host "== $m" -ForegroundColor Cyan }
function Write-Ok($m)    { Write-Host "   [ok] $m" -ForegroundColor Green }
function Write-Warn2($m) { Write-Host "   [! ] $m" -ForegroundColor Yellow }
function Write-Err2($m)  { Write-Host "   [xx] $m" -ForegroundColor Red }

# --------------------------------------------------------------------------
# project root
# --------------------------------------------------------------------------
if ([string]::IsNullOrWhiteSpace($ProjectDir)) {
    if ($PSScriptRoot) { $ProjectDir = Split-Path -Parent $PSScriptRoot }
    else               { $ProjectDir = Split-Path -Parent (Join-Path (Get-Location).Path 'tools') }
}
$ProjectDir = (Resolve-Path $ProjectDir).Path

# --------------------------------------------------------------------------
# locate ESP-IDF
# --------------------------------------------------------------------------
function Find-Idf {
    $roots = @()
    if ($env:IDF_TOOLS_PATH) { $roots += $env:IDF_TOOLS_PATH }
    $roots += @('D:\Espressif', 'C:\Espressif', (Join-Path $env:USERPROFILE 'esp'))
    foreach ($r in $roots) {
        $fw = Join-Path $r 'frameworks'
        if (-not (Test-Path $fw)) { continue }
        $cand = Get-ChildItem $fw -Directory -Filter 'esp-idf-*' -ErrorAction SilentlyContinue |
                Sort-Object Name -Descending | Select-Object -First 1
        if ($cand -and (Test-Path (Join-Path $cand.FullName 'export.ps1'))) {
            return @{ ToolsPath = $r; IdfPath = $cand.FullName }
        }
    }
    return $null
}

$idf = Find-Idf
if (-not $idf) {
    Write-Err2 "ESP-IDF not found (looked for <root>\frameworks\esp-idf-*)"
    Write-Host "   install it first: https://dl.espressif.com/dl/esp-idf/"
    exit 1
}

Write-Host ""
Write-Host "  fish tank builder" -ForegroundColor White
Write-Host "  project : $ProjectDir"
Write-Host "  idf     : $($idf.IdfPath)"

# --------------------------------------------------------------------------
# environment
# --------------------------------------------------------------------------
Write-Step "1/4 setting up ESP-IDF environment"
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force
$env:IDF_TOOLS_PATH = $idf.ToolsPath
$env:PYTHONUTF8 = '1'
$env:PYTHONIOENCODING = 'utf-8'

# ESP-IDF ships its own python; put it first so activate.py picks the right venv
$idfPython = Get-ChildItem (Join-Path $idf.ToolsPath 'tools\idf-python') -Directory -ErrorAction SilentlyContinue |
             Sort-Object Name -Descending | Select-Object -First 1
if ($idfPython) {
    $env:PATH = $idfPython.FullName + ';' + $env:PATH
    Write-Ok ("python : " + $idfPython.FullName + "\python.exe")
}

. (Join-Path $idf.IdfPath 'export.ps1')
if (-not $env:IDF_PATH) {
    Write-Err2 "export.ps1 did not set IDF_PATH"
    exit 1
}
Write-Ok ("IDF_PATH = " + $env:IDF_PATH)
Write-Ok ("IDF_PYTHON_ENV_PATH = " + $env:IDF_PYTHON_ENV_PATH)

# --------------------------------------------------------------------------
# decide build location (ASCII vs mirrored)
# --------------------------------------------------------------------------
$hasNonAscii = ($ProjectDir -match '[^\x00-\x7F]')
Write-Step "2/4 choosing build directory"
if ($hasNonAscii) {
    Write-Warn2 "project path contains non-ASCII characters; ESP-IDF toolchain cannot handle it"
    Write-Host  "        mirroring sources to: $BuildDir"
    $buildIn = $BuildDir
    New-Item -ItemType Directory -Force -Path $buildIn | Out-Null

    # /MIR keeps it incremental: only changed files are copied
    $srcMain = Join-Path $ProjectDir 'main'
    $dstMain = Join-Path $buildIn 'main'
    $null = robocopy $srcMain $dstMain /MIR /NFL /NDL /NJH /NJS /NP /R:2 /W:2
    if ($LASTEXITCODE -ge 8) { Write-Err2 "robocopy failed ($LASTEXITCODE)"; exit 1 }
    foreach ($f in @('CMakeLists.txt', 'sdkconfig', 'sdkconfig.defaults', 'partitions.csv')) {
        $p = Join-Path $ProjectDir $f
        if (Test-Path $p) { Copy-Item $p (Join-Path $buildIn $f) -Force }
    }
    Write-Ok "sources mirrored"
} else {
    $buildIn = $ProjectDir
    Write-Ok "project path is ASCII, building in place"
}

if ($Clean) {
    $b = Join-Path $buildIn 'build'
    if (Test-Path $b) { Remove-Item $b -Recurse -Force; Write-Ok "build dir removed (-Clean)" }
}

# --------------------------------------------------------------------------
# build
# --------------------------------------------------------------------------
Write-Step "3/4 idf.py build"
Push-Location $buildIn
try {
    & idf.py build
    $rc = $LASTEXITCODE
} finally {
    Pop-Location
}

if ($rc -ne 0) {
    Write-Err2 "build failed (exit $rc)"
    Write-Host "   logs: $buildIn\build\log\"
    exit $rc
}

# --------------------------------------------------------------------------
# copy images back
# --------------------------------------------------------------------------
Write-Step "4/4 collecting images"
$from = Join-Path $buildIn 'build'
$to = Join-Path $ProjectDir 'build'
if ($hasNonAscii -and -not $NoCopyBack) {
    New-Item -ItemType Directory -Force -Path (Join-Path $to 'bootloader') | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $to 'partition_table') | Out-Null
    Copy-Item (Join-Path $from 'bootloader\bootloader.bin') (Join-Path $to 'bootloader') -Force
    Copy-Item (Join-Path $from 'partition_table\partition-table.bin') (Join-Path $to 'partition_table') -Force
    foreach ($f in @('fish_gundam_panzer.bin', 'fish_gundam_panzer.elf', 'flasher_args.json', 'flash_args')) {
        $p = Join-Path $from $f
        if (Test-Path $p) { Copy-Item $p $to -Force }
    }
    Write-Ok "images copied to $to"
}

$appBin = Join-Path $to 'fish_gundam_panzer.bin'
if (Test-Path $appBin) {
    $size = (Get-Item $appBin).Length
    Write-Ok ("app binary: {0:N0} bytes ({1:N1} KB)" -f $size, ($size / 1KB))
}
Write-Host ""
Write-Host "  Build complete. Flash with:" -ForegroundColor Green
Write-Host "    powershell -ExecutionPolicy Bypass -File tools\flash.ps1 -Port COMx"
Write-Host ""
exit 0
