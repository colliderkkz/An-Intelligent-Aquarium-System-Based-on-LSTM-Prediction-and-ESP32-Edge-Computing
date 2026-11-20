# Host-side syntax check (no ESP-IDF installation required).
#
# Uses MSVC cl.exe to syntax/type-check every source file under main/.
# ESP-IDF APIs come from the stub headers in tools/hosttest/stub, so this
# validates braces, semicolons, identifiers, function signatures, struct
# fields and enums -- it does NOT replace `idf.py build`.
#
# Usage:  powershell -ExecutionPolicy Bypass -File tools\hosttest\check_syntax.ps1
param(
    [string]$VcVersion = '',
    [switch]$KeepObjs
)

$ErrorActionPreference = 'Stop'

if ($PSScriptRoot) {
    $here = $PSScriptRoot
} else {
    $here = Join-Path (Get-Location).Path 'tools\hosttest'
}
$root = Split-Path -Parent (Split-Path -Parent $here)

$vcRoot = 'C:\Program Files\Microsoft Visual Studio'

function Find-Cl {
    $found = @()
    if ($VcVersion -ne '') { $found += $VcVersion }
    $patterns = @(
        'C:\Program Files\Microsoft Visual Studio\*\*\VC\Tools\MSVC\*\bin\Hostx64\x64\cl.exe',
        'C:\Program Files\Microsoft Visual Studio\*\VC\Tools\MSVC\*\bin\Hostx64\x64\cl.exe',
        'C:\Program Files (x86)\Microsoft Visual Studio\*\*\VC\Tools\MSVC\*\bin\Hostx64\x64\cl.exe',
        'C:\Program Files (x86)\Microsoft Visual Studio\*\VC\Tools\MSVC\*\bin\Hostx64\x64\cl.exe'
    )
    foreach ($p in $patterns) {
        $items = Get-Item $p -ErrorAction SilentlyContinue
        foreach ($i in ($items | Sort-Object FullName -Descending)) { $found += $i.FullName }
    }
    foreach ($exe in $found) {
        if (Test-Path $exe) {
            # <ver>\bin\Hostx64\x64\cl.exe  ->  <ver>
            $vc = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $exe)))
            return @{ Cl = $exe; Vc = $vc }
        }
    }
    throw 'cl.exe not found (install Visual Studio C++ build tools)'
}

function Find-Sdk {
    $base = 'C:\Program Files (x86)\Windows Kits\10'
    $ver = Get-ChildItem "$base\Include" -Directory -ErrorAction SilentlyContinue |
           Sort-Object Name -Descending | Select-Object -First 1
    if (-not $ver) { throw 'Windows SDK not found' }
    return @{ Inc = (Join-Path $base ('Include\' + $ver.Name)); Lib = (Join-Path $base ('Lib\' + $ver.Name)) }
}

$tool = Find-Cl
$sdk = Find-Sdk

$env:INCLUDE = ($tool.Vc + '\include;' + $sdk.Inc + '\ucrt;' + $sdk.Inc + '\shared;' + $sdk.Inc + '\um')
$env:LIB = ($tool.Vc + '\lib\x64;' + $sdk.Lib + '\ucrt\x64;' + $sdk.Lib + '\um\x64')
# MSVC deprecation noise is irrelevant here (GCC/IDF does not warn about sscanf)
$env:CL = '/D_CRT_SECURE_NO_WARNINGS'

$stub = Join-Path $root 'tools\hosttest\stub'
$main = Join-Path $root 'main'
$outDir = Join-Path $env:TEMP ('fishtank_syntax_' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

# Keep in sync with SRCS in main/CMakeLists.txt
$srcs = @(
    'main.c',
    'bus\i2c_bus.c', 'bus\onewire_bus.c', 'bus\spi_bus.c',
    'drivers\pcf8563.c', 'drivers\sgp30.c', 'drivers\sd_card_spi.c', 'drivers\ds18b20.c',
    'drivers\adc\adc_manager.c', 'drivers\adc\ph_sensor_adc.c', 'drivers\adc\tds_sensor.c',
    'drivers\adc\turbidity.c', 'drivers\adc\light_sensor.c', 'drivers\adc\water_level.c',
    'sensors\co2_sensor.c', 'sensors\light_sensor.c', 'sensors\ph_sensor.c',
    'sensors\tds_sensor.c', 'sensors\temp_sensor.c', 'sensors\turbidity_sensor.c',
    'sensors\water_level.c',
    'actuators\gpio_switch.c', 'actuators\light_actuator.c', 'actuators\pump.c',
    'actuators\water_pump.c', 'actuators\heater.c', 'actuators\co2_valve.c',
    'actuators\air_pump.c', 'actuators\uv_lamp.c', 'actuators\feeder.c',
    'service\sensor_hub.c', 'service\time_service.c', 'service\control_mode.c',
    'service\schedule.c', 'service\sd_logger.c',
    'control\temp_control.c', 'control\light_control.c', 'control\feeding_control.c',
    'control\water_control.c', 'control\uv_control.c',
    'algorithm\data_fusion.c', 'algorithm\feature_extraction.c', 'algorithm\kalman_filter.c',
    'algorithm\outlier_detection.c',
    'utils\filter.c', 'utils\queue_helper.c', 'utils\timer_helper.c',
    'communication\uart_receiver.c', 'communication\voice_control.c',
    'config\error_codes.c', 'config\param_store.c',
    'debug\debug_protocol.c', 'debug\debug_commands.c', 'debug\debug_console.c'
)

Push-Location $outDir
$fail = 0
try {
    foreach ($src in $srcs) {
        $path = Join-Path $main $src
        if (-not (Test-Path $path)) {
            Write-Host ("MISSING: " + $src) -ForegroundColor Red
            $fail++
            continue
        }
        $output = & $tool.Cl /nologo /c /Zs /std:c11 /utf-8 /W3 /I"$stub" /I"$main" $path 2>&1
        if ($LASTEXITCODE -ne 0) {
            $output | ForEach-Object { Write-Host $_ }
            Write-Host ("FAILED: " + $src) -ForegroundColor Red
            $fail++
        } elseif ($output) {
            $output | ForEach-Object { Write-Host $_ }
        }
    }
}
finally {
    Pop-Location
    if (-not $KeepObjs) { Remove-Item -Recurse -Force $outDir -ErrorAction SilentlyContinue }
}

Write-Host ''
if ($fail -eq 0) {
    Write-Host ("SYNTAX OK: " + $srcs.Count + " files") -ForegroundColor Green
    exit 0
} else {
    Write-Host ("SYNTAX FAILED: " + $fail + " file(s)") -ForegroundColor Red
    exit 1
}
