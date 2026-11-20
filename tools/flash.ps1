# =============================================================================
#  fish_tank flasher  --  flashes the ESP32-S3 board over USB-serial
# -----------------------------------------------------------------------------
#  Usage:
#    powershell -ExecutionPolicy Bypass -File tools\flash.ps1
#    powershell -ExecutionPolicy Bypass -File tools\flash.ps1 -Port COM5
#    powershell -ExecutionPolicy Bypass -File tools\flash.ps1 -Monitor
#    powershell -ExecutionPolicy Bypass -File tools\flash.ps1 -Erase
#    powershell -ExecutionPolicy Bypass -File tools\flash.ps1 -ListPorts
#
#  Strategy:
#    - finds the ESP-IDF installation automatically (D:\Espressif by default)
#    - if build\ holds fresh images   -> flashes them directly with esptool
#      (this does NOT trigger a rebuild, which matters because the project
#       path contains a Chinese character and ESP-IDF cannot build there)
#    - if build\ is stale AND the project path is pure ASCII -> idf.py flash
#      (which rebuilds automatically)
#    - otherwise it tells you to run tools\build.ps1 first
#
#  NOTE: ASCII-only on purpose (PowerShell 5.1 reads BOM-less files as ANSI).
# =============================================================================
[CmdletBinding()]
param(
    [string]$Port = '',
    [int]$Baud = 460800,
    [switch]$Monitor,
    [switch]$Erase,
    [switch]$Force,
    [switch]$ListPorts,
    [string]$ProjectDir = ''
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
$IsAsciiPath = -not ($ProjectDir -match '[^\x00-\x7F]')

# --------------------------------------------------------------------------
# serial ports
# --------------------------------------------------------------------------
function Get-SerialDevices {
    $list = @()
    try { $devs = Get-CimInstance Win32_PnPEntity -ErrorAction Stop }
    catch { return $list }
    foreach ($d in $devs) {
        if ($d.Name -match '\((COM\d+)\)') {
            $id = [string]$d.PNPDeviceID
            $list += [pscustomobject]@{
                Port        = $Matches[1]
                Name        = $d.Name
                IsBluetooth = ($id -match '^BTHENUM')
                IsUsbSerial = ($id -match '^USB\\|^FTDIBUS')
            }
        }
    }
    return $list
}

function Select-EspPort {
    $devs = Get-SerialDevices
    if (-not $devs -or $devs.Count -eq 0) { return '' }
    $usb = @($devs | Where-Object { $_.IsUsbSerial -and -not $_.IsBluetooth })
    if ($usb.Count -ge 1) {
        if ($usb.Count -gt 1) {
            Write-Warn2 "multiple USB serial ports found, using the first one:"
            foreach ($u in $usb) { Write-Host ("        {0}  {1}" -f $u.Port, $u.Name) }
            Write-Host  "        (use -Port COMx to pick another)"
        }
        return $usb[0].Port
    }
    $other = @($devs | Where-Object { -not $_.IsBluetooth })
    if ($other.Count -ge 1) { return $other[0].Port }
    return ''
}

if ($ListPorts) {
    Write-Step "serial ports on this PC"
    $devs = Get-SerialDevices
    if (-not $devs -or $devs.Count -eq 0) {
        Write-Err2 "no COM port found at all"
    } else {
        foreach ($d in $devs) {
            $tag = if ($d.IsBluetooth) { 'bluetooth' } elseif ($d.IsUsbSerial) { 'USB-SERIAL' } else { '?' }
            Write-Host ("   {0,-6} {1,-12} {2}" -f $d.Port, $tag, $d.Name)
        }
    }
    Write-Host ""
    Write-Host "   The board's USB-UART bridge shows up as USB-SERIAL (CP210x / CH340 / CH343)."
    Write-Host "   Plug the cable into the board's UART port, not the USB-CDC port."
    exit 0
}

# --------------------------------------------------------------------------
# ESP-IDF discovery
# --------------------------------------------------------------------------
function Find-IdfRoot {
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

$idfRoot = Find-IdfRoot

function Find-IdfPython {
    # prefer the python that ESP-IDF created (it has esptool + esp-idf-monitor)
    if ($idfRoot) {
        $envRoot = Join-Path $idfRoot.ToolsPath 'python_env'
        $venv = Get-ChildItem $envRoot -Directory -ErrorAction SilentlyContinue |
                Sort-Object Name -Descending | Select-Object -First 1
        if ($venv) {
            $exe = Join-Path $venv.FullName 'Scripts\python.exe'
            if (Test-Path $exe) { return $exe }
        }
    }
    $py = Get-Command python -ErrorAction SilentlyContinue
    if ($py) { return $py.Source }
    $py = Get-Command python3 -ErrorAction SilentlyContinue
    if ($py) { return $py.Source }
    return $null
}

function Test-Esptool($pythonExe) {
    if (-not $pythonExe) { return $false }
    $saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $ok = $false
    try {
        & $pythonExe -c "import esptool" *> $null
        $ok = ($LASTEXITCODE -eq 0)
    } catch { $ok = $false }
    $ErrorActionPreference = $saved
    return $ok
}

function Initialize-IdfEnv {
    if (Get-Command idf.py -ErrorAction SilentlyContinue) { return $true }
    if (-not $idfRoot) { return $false }
    Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force -ErrorAction SilentlyContinue
    $env:IDF_TOOLS_PATH = $idfRoot.ToolsPath
    $env:PYTHONUTF8 = '1'
    $pyDir = Join-Path $idfRoot.ToolsPath 'tools\idf-python'
    $py = Get-ChildItem $pyDir -Directory -ErrorAction SilentlyContinue |
          Sort-Object Name -Descending | Select-Object -First 1
    if ($py) { $env:PATH = $py.FullName + ';' + $env:PATH }
    $saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { . (Join-Path $idfRoot.IdfPath 'export.ps1') *> $null } catch { }
    $ErrorActionPreference = $saved
    return [bool](Get-Command idf.py -ErrorAction SilentlyContinue)
}

# --------------------------------------------------------------------------
# build artefacts
# --------------------------------------------------------------------------
$Script:BinLayout = @(
    @{ Addr = '0x0';     File = 'build\bootloader\bootloader.bin' },
    @{ Addr = '0x8000';  File = 'build\partition_table\partition-table.bin' },
    @{ Addr = '0x10000'; File = 'build\fish_gundam_panzer.bin' }
)

function Test-BinLayout {
    foreach ($item in $Script:BinLayout) {
        if (-not (Test-Path (Join-Path $ProjectDir $item.File))) { return $false }
    }
    return $true
}

function Test-BuildFresh {
    $appBin = Join-Path $ProjectDir 'build\fish_gundam_panzer.bin'
    if (-not (Test-Path $appBin)) { return @{ Fresh = $false; Reason = 'app binary missing' } }

    $fa = Join-Path $ProjectDir 'build\flasher_args.json'
    if (Test-Path $fa) {
        try {
            $j = Get-Content $fa -Raw | ConvertFrom-Json
            $size = [string]$j.flash_settings.flash_size
            if ($size -and $size -ne '16MB') {
                return @{ Fresh = $false; Reason = ("built for {0} flash, need 16MB" -f $size) }
            }
        } catch { }
    }

    $binTime = (Get-Item $appBin).LastWriteTimeUtc
    $newest = Get-ChildItem (Join-Path $ProjectDir 'main') -Recurse -Include *.c, *.h -ErrorAction SilentlyContinue |
              Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
    if ($newest -and $newest.LastWriteTimeUtc -gt $binTime) {
        return @{ Fresh = $false; Reason = ("source newer than binary: {0}" -f $newest.Name) }
    }
    return @{ Fresh = $true; Reason = '' }
}

function Invoke-Idf([string[]]$IdfArgs) {
    & idf.py @IdfArgs
}

# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------
Write-Host ""
Write-Host "  fish tank flasher  --  ESP32-S3 (N16R8)" -ForegroundColor White
Write-Host "  project: $ProjectDir"

Write-Step "1/4 checking tools"
$pythonExe = Find-IdfPython
$hasEsptool = Test-Esptool $pythonExe
$hasIdf = Initialize-IdfEnv
if ($pythonExe)  { Write-Ok "python  : $pythonExe" }
if ($hasEsptool) { Write-Ok "esptool : available" } else { Write-Warn2 "esptool : not available" }
if ($hasIdf)     { Write-Ok "idf.py  : available" } else { Write-Warn2 "idf.py  : not available" }

Write-Step "2/4 checking build\ artifacts"
$hasLayout = Test-BinLayout
$fresh = @{ Fresh = $false; Reason = 'no build folder' }
if ($hasLayout) {
    $fresh = Test-BuildFresh
    if ($fresh.Fresh) { Write-Ok "bootloader / partition-table / app present and up to date" }
    else              { Write-Warn2 ("build is stale: " + $fresh.Reason) }
} else {
    Write-Warn2 "build\ is incomplete (bootloader or partition-table missing)"
}

# --- how are we going to flash? -------------------------------------------
$useEsptool = ($hasLayout -and $hasEsptool -and ($fresh.Fresh -or $Force))
$useIdf     = (-not $useEsptool) -and $hasIdf -and $IsAsciiPath

if (-not $useEsptool -and -not $useIdf) {
    Write-Host ""
    if (-not $hasLayout) {
        Write-Err2 "cannot flash: no usable build\ folder"
    } elseif (-not $fresh.Fresh) {
        Write-Err2 ("cannot flash: " + $fresh.Reason)
    } elseif (-not $IsAsciiPath) {
        Write-Err2 "cannot rebuild here: project path contains non-ASCII characters"
    } else {
        Write-Err2 "cannot flash: neither esptool nor idf.py is available"
    }
    Write-Host ""
    Write-Host "   Build the firmware first:" -ForegroundColor Yellow
    Write-Host "     powershell -ExecutionPolicy Bypass -File tools\build.ps1"
    Write-Host ""
    Write-Host "   (or pass -Force to flash the existing images anyway)" -ForegroundColor Yellow
    exit 1
}

Write-Step "3/4 resolving serial port"
if ([string]::IsNullOrWhiteSpace($Port)) {
    $Port = Select-EspPort
    if ([string]::IsNullOrWhiteSpace($Port)) {
        Write-Err2 "no USB-serial port detected"
        Write-Host "       1. use a DATA usb cable (not charge-only)"
        Write-Host "       2. plug it into the board's UART port (not USB-CDC)"
        Write-Host "       3. install CH340/CP210x driver if device manager shows '!'"
        Write-Host "       4. re-run with -ListPorts to inspect"
        exit 1
    }
}
Write-Ok "using $Port"

Write-Step "4/4 flashing"
$rc = 1
$nvsBak = Join-Path $env:TEMP 'fishtank_nvs_backup.bin'
$nvsOk = $false

if ($useEsptool) {
    Write-Host ("   esptool write_flash @ {0} baud" -f $Baud)
    $common = @('-m', 'esptool', '--chip', 'esp32s3', '-p', $Port, '-b', "$Baud",
                '--before', 'default_reset', '--after', 'hard_reset')
    if ($Erase) {
        # A full erase also wipes NVS (wifi credentials, mqtt account, every
        # parameter and the schedules). Back the 24KB partition up first and
        # write it back after flashing, so "reflash" never loses settings.
        Write-Host "   backing up NVS (wifi / params / schedules) ..."
        if (Test-Path $nvsBak) { Remove-Item $nvsBak -Force -ErrorAction SilentlyContinue }
        & $pythonExe @common read_flash 0x9000 0x6000 $nvsBak | Out-Null
        if ($LASTEXITCODE -eq 0 -and (Test-Path $nvsBak) -and ((Get-Item $nvsBak).Length -eq 24576)) {
            $nvsOk = $true
            Write-Ok "NVS backup saved"
        } else {
            Write-Warn2 "NVS backup failed - settings will be lost"
        }
        Write-Host "   erasing flash ..."
        & $pythonExe @common erase_flash
        if ($LASTEXITCODE -ne 0) { Write-Err2 "erase failed"; exit $LASTEXITCODE }
    }
    $write = @('write_flash', '--flash_mode', 'dio', '--flash_freq', '80m', '--flash_size', '16MB')
    foreach ($item in $Script:BinLayout) {
        $write += $item.Addr
        $write += (Join-Path $ProjectDir $item.File)
    }
    & $pythonExe @common @write
    $rc = $LASTEXITCODE
    if ($rc -eq 0 -and $nvsOk) {
        Write-Host "   restoring NVS ..."
        & $pythonExe @common write_flash 0x9000 $nvsBak
        if ($LASTEXITCODE -ne 0) { Write-Warn2 "NVS restore failed (settings fall back to defaults)" }
        else { Write-Ok "wifi / params / schedules restored" }
    }
} else {
    Write-Host "   idf.py -p $Port -b $Baud flash  (rebuilds first)"
    if ($Erase) {
        Invoke-Idf @('-p', $Port, 'erase-flash')
        if ($LASTEXITCODE -ne 0) { Write-Err2 "erase failed"; exit $LASTEXITCODE }
    }
    Invoke-Idf @('-p', $Port, '-b', "$Baud", 'flash')
    $rc = $LASTEXITCODE
}

if ($rc -ne 0) {
    Write-Err2 "flash failed (exit $rc)"
    Write-Host ""
    Write-Host "   If it says 'Failed to connect':" -ForegroundColor Yellow
    Write-Host "     - hold BOOT, tap RST, release BOOT  (manual download mode)"
    Write-Host "     - lower the baud rate:  -Baud 115200"
    Write-Host "     - check the port:       -ListPorts"
    exit $rc
}

Write-Ok "flash done"

# --------------------------------------------------------------------------
# optional monitor
# --------------------------------------------------------------------------
if ($Monitor) {
    $elf = Join-Path $ProjectDir 'build\fish_gundam_panzer.elf'
    $monitorPy = if ($idfRoot) { Join-Path $idfRoot.IdfPath 'tools\idf_monitor.py' } else { '' }
    Write-Host ""
    Write-Host "   opening monitor -- press Ctrl+] to quit" -ForegroundColor Cyan
    if ($monitorPy -and (Test-Path $monitorPy) -and (Test-Path $elf) -and $pythonExe) {
        & $pythonExe $monitorPy -p $Port -b 115200 $elf
    } else {
        Write-Warn2 "idf_monitor.py or elf missing; use the host tool instead:"
        Write-Host "     python tools\fish_tank_debugger.py --port $Port"
    }
}

Write-Host ""
Write-Host "  Next:" -ForegroundColor Green
Write-Host "    python tools\fish_tank_debugger.py --port $Port     # debug GUI"
Write-Host "    (expect '#READY,fw=fish_gundam_panzer,ver=1.1.1,...' after reset)"
Write-Host ""
exit 0
