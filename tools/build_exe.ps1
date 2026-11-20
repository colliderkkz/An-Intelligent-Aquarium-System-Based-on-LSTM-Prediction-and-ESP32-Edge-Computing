# =============================================================================
#  fish_tank exe builder  --  packages the host tool + firmware into ONE exe
# -----------------------------------------------------------------------------
#  Result:  <project>\dist\FishTank_Tool.exe
#           double-click -> choose "host debugger" or "flash firmware"
#
#  The exe bundles:
#    - the Tk host debugger      (tools/fish_tank_debugger.py)
#    - the launcher / first page (tools/launcher.py)
#    - the firmware images       (build\bootloader / partition_table / app)
#    - esptool + pyserial        (flashing works with no Python installed)
#
#  Usage:
#    powershell -ExecutionPolicy Bypass -File tools\build_exe.ps1
#    powershell -ExecutionPolicy Bypass -File tools\build_exe.ps1 -Clean
#
#  Notes:
#    * builds happen in an ASCII path (D:\fish_tank_build\pack) because the
#      project lives under a Chinese-named folder and some tools choke on it
#    * --collect-all esptool is REQUIRED: esptool 5.x keeps its flasher stub
#      as JSON data files; without them flashing fails with
#      "Flasher stub data is missing for ESP32-S3"
#
#  ASCII-only on purpose: Windows PowerShell 5.1 decodes BOM-less .ps1 files
#  as the local ANSI code page, and non-ASCII bytes there break the parser.
# =============================================================================
[CmdletBinding()]
param(
    [string]$ProjectDir = '',
    [string]$WorkDir = 'D:\fish_tank_build\pack',
    [string]$VenvDir = 'D:\fish_tank_build\packenv',
    [string]$PublishDir = 'D:\FishTankTool',
    [string]$ExeName = 'FishTank_Tool',
    [switch]$Clean,
    [switch]$NoShortcut
)

$ErrorActionPreference = 'Stop'

function Write-Step($m)  { Write-Host ""; Write-Host "== $m" -ForegroundColor Cyan }
function Write-Ok($m)    { Write-Host "   [ok] $m" -ForegroundColor Green }
function Write-Warn2($m) { Write-Host "   [! ] $m" -ForegroundColor Yellow }
function Write-Err2($m)  { Write-Host "   [xx] $m" -ForegroundColor Red }

# pip / pyinstaller write normal progress to stderr. With
# $ErrorActionPreference='Stop' that becomes a terminating error, so run
# external programs inside a 'Continue' scope and check the exit code.
function Invoke-External {
    param(
        [Parameter(Mandatory = $true)][string]$ExePath,
        [string[]]$ExeArgs = @(),
        [int]$TailLines = 0
    )
    $saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $out = & $ExePath @ExeArgs 2>&1
        $code = $LASTEXITCODE
        if ($TailLines -gt 0) {
            $out | Select-Object -Last $TailLines | ForEach-Object { Write-Host $_ }
        } else {
            $out | ForEach-Object { Write-Host $_ }
        }
        return $code
    } finally {
        $ErrorActionPreference = $saved
    }
}

function Test-PyModule {
    param([string]$Py, [string]$ModuleName)
    $saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $Py -c "import $ModuleName" *> $null
        return ($LASTEXITCODE -eq 0)
    } catch {
        return $false
    } finally {
        $ErrorActionPreference = $saved
    }
}

if ([string]::IsNullOrWhiteSpace($ProjectDir)) {
    if ($PSScriptRoot) { $ProjectDir = Split-Path -Parent $PSScriptRoot }
    else               { $ProjectDir = Split-Path -Parent (Join-Path (Get-Location).Path 'tools') }
}
$ProjectDir = (Resolve-Path $ProjectDir).Path

Write-Host ""
Write-Host "  fish tank exe builder" -ForegroundColor White
Write-Host "  project : $ProjectDir"

# --------------------------------------------------------------------------
# 1) firmware check
# --------------------------------------------------------------------------
Write-Step "1/5 checking firmware"
$firmware = @(
    @{ Src = 'build\bootloader\bootloader.bin';           Dest = 'build\bootloader' },
    @{ Src = 'build\partition_table\partition-table.bin'; Dest = 'build\partition_table' },
    @{ Src = 'build\fish_gundam_panzer.bin';              Dest = 'build' }
)
foreach ($f in $firmware) {
    $p = Join-Path $ProjectDir $f.Src
    if (-not (Test-Path $p)) {
        Write-Err2 ("firmware missing: " + $f.Src)
        Write-Host "   build the firmware first: powershell -ExecutionPolicy Bypass -File tools\build.ps1"
        exit 1
    }
    Write-Ok ("{0}  ({1:N0} B)" -f $f.Src, (Get-Item $p).Length)
}

# --------------------------------------------------------------------------
# 2) python environment
# --------------------------------------------------------------------------
Write-Step "2/5 preparing python environment"
$venvPy = Join-Path $VenvDir 'Scripts\python.exe'
if ($Clean -and (Test-Path $VenvDir)) {
    Remove-Item $VenvDir -Recurse -Force
    Write-Ok "venv removed (-Clean)"
}
if (-not (Test-Path $venvPy)) {
    Write-Host "   creating venv at $VenvDir ..."
    $sysPy = $null
    $cands = @(
        'C:\Users\Administrator\.dsh\dsh-runtimes\dsh-primary-runtime\dependencies\python\python.exe',
        (Join-Path $env:LOCALAPPDATA 'Programs\Python\Python312\python.exe'),
        (Join-Path $env:LOCALAPPDATA 'Programs\Python\Python311\python.exe')
    )
    foreach ($cand in $cands) {
        if (Test-Path $cand) { $sysPy = $cand; break }
    }
    if (-not $sysPy) {
        $c = Get-Command python -ErrorAction SilentlyContinue
        if ($c) { $sysPy = $c.Source }
    }
    if (-not $sysPy) {
        Write-Err2 "no python found; install Python 3.10+ first"
        exit 1
    }
    Write-Host "   base python: $sysPy"
    $rc = Invoke-External -ExePath $sysPy -ExeArgs @('-m', 'venv', $VenvDir)
    if ($rc -ne 0) { Write-Err2 "venv creation failed"; exit 1 }
    $rc = Invoke-External -ExePath $venvPy -ExeArgs @('-m', 'pip', 'install',
                       '--disable-pip-version-check', '-q', '--upgrade', 'pip')
}
Write-Ok "venv: $VenvDir"

$need = @()
if (-not (Test-PyModule $venvPy 'PyInstaller')) { $need += 'pyinstaller' }
if (-not (Test-PyModule $venvPy 'esptool'))     { $need += 'esptool' }
if (-not (Test-PyModule $venvPy 'serial'))      { $need += 'pyserial' }
if ($need.Count -gt 0) {
    Write-Host ("   installing: " + ($need -join ', '))
    $pipArgs = @('-m', 'pip', 'install', '--disable-pip-version-check', '-q') + $need
    $rc = Invoke-External -ExePath $venvPy -ExeArgs $pipArgs
    if ($rc -ne 0) { Write-Err2 "pip install failed"; exit 1 }
}
$espVer = & $venvPy -c "import esptool; print(esptool.__version__)" 2>$null
Write-Ok ("esptool " + $espVer)
$pyiVer = & $venvPy -c "import PyInstaller; print(PyInstaller.__version__)" 2>$null
Write-Ok ("pyinstaller " + $pyiVer)

# --------------------------------------------------------------------------
# 3) stage sources into an ASCII work dir
# --------------------------------------------------------------------------
Write-Step "3/5 staging sources"
if (Test-Path $WorkDir) { Remove-Item $WorkDir -Recurse -Force }
New-Item -ItemType Directory -Force -Path (Join-Path $WorkDir 'tools') | Out-Null
foreach ($f in $firmware) {
    $dst = Join-Path $WorkDir $f.Dest
    New-Item -ItemType Directory -Force -Path $dst | Out-Null
    Copy-Item (Join-Path $ProjectDir $f.Src) $dst -Force
}
foreach ($py in @('launcher.py', 'fish_tank_debugger.py')) {
    Copy-Item (Join-Path $ProjectDir ('tools\' + $py)) (Join-Path $WorkDir 'tools') -Force
}
Write-Ok "staged to $WorkDir"

# --------------------------------------------------------------------------
# 4) pyinstaller
# --------------------------------------------------------------------------
Write-Step "4/5 pyinstaller"
$pyiArgs = @(
    '--noconfirm', '--onefile', '--windowed',
    '--name', $ExeName,
    '--paths', 'tools',
    '--collect-all', 'esptool',
    '--add-data', 'build\bootloader\bootloader.bin;firmware\bootloader',
    '--add-data', 'build\partition_table\partition-table.bin;firmware\partition_table',
    '--add-data', 'build\fish_gundam_panzer.bin;firmware',
    'tools\launcher.py'
)
Push-Location $WorkDir
try {
    $rc = Invoke-External -ExePath (Join-Path $VenvDir 'Scripts\pyinstaller.exe') `
                          -ExeArgs $pyiArgs -TailLines 6
} finally {
    Pop-Location
}
if ($rc -ne 0) { Write-Err2 "pyinstaller failed (exit $rc)"; exit $rc }

$builtExe = Join-Path $WorkDir ("dist\" + $ExeName + ".exe")
if (-not (Test-Path $builtExe)) { Write-Err2 "exe not produced"; exit 1 }
Write-Ok ("built " + $builtExe)

# --------------------------------------------------------------------------
# 5) publish
# --------------------------------------------------------------------------
# IMPORTANT: a PyInstaller --onefile exe does NOT start when it sits under a
# non-ASCII path (e.g. ...\Desktop\<Chinese>\...). It silently dies before the
# Python interpreter comes up. So we publish into an ASCII directory and put a
# shortcut on the desktop instead of shipping the exe inside the project tree.
Write-Step "5/5 publishing"
if ($PublishDir -match '[^\x00-\x7F]') {
    Write-Warn2 "publish dir contains non-ASCII characters: $PublishDir"
    Write-Warn2 "one-file exes may fail to start there; choose an ASCII path"
}
New-Item -ItemType Directory -Force -Path $PublishDir | Out-Null
$final = Join-Path $PublishDir ($ExeName + '.exe')
try {
    Copy-Item $builtExe $final -Force -ErrorAction Stop
} catch {
    # 目标 exe 正在运行会锁文件；退回到带后缀的新文件，别让整个打包失败
    $altName = $ExeName + '_new.exe'
    $alt = Join-Path $PublishDir $altName
    Copy-Item $builtExe $alt -Force
    Write-Warn2 ("target exe is in use, wrote: " + $alt)
    Write-Warn2 ("close the running app, then rename " + $altName + " -> " + $ExeName + ".exe")
    $final = $alt
}
$size = [math]::Round((Get-Item $final).Length / 1MB, 1)
Write-Ok ("$final  ($size MB)")

if (-not $NoShortcut) {
    try {
        $desktop = [Environment]::GetFolderPath('Desktop')
        if ($desktop) {
            $lnk = Join-Path $desktop ($ExeName + '.lnk')
            $ws = New-Object -ComObject WScript.Shell
            $sc = $ws.CreateShortcut($lnk)
            $sc.TargetPath = $final
            $sc.WorkingDirectory = $PublishDir
            $sc.Description = 'Fish tank host debugger and firmware flasher'
            $sc.Save()
            Write-Ok ("shortcut: " + $lnk)
        }
    } catch {
        Write-Warn2 ("shortcut creation failed: " + $_.Exception.Message)
    }
}

# keep a copy in the project for archiving, but warn that it must be moved
$distDir = Join-Path $ProjectDir 'dist'
New-Item -ItemType Directory -Force -Path $distDir | Out-Null
Copy-Item $builtExe (Join-Path $distDir ($ExeName + '.exe')) -Force
$note = Join-Path $distDir 'README-FIRST.txt'
$noteText = @(
    'The exe in this folder only runs when its path is pure ASCII.',
    'This project folder contains non-ASCII characters, so DO NOT double-click',
    'the exe from here -- copy it to an English path first, e.g.:',
    '',
    '    D:\FishTankTool\FishTank_Tool.exe',
    '',
    'The ready-to-run copy already exists at:',
    ('    ' + $final),
    'and a desktop shortcut was created by tools\build_exe.ps1.'
) -join [Environment]::NewLine
[System.IO.File]::WriteAllText($note, $noteText, (New-Object System.Text.UTF8Encoding($false)))
Write-Ok ("archived copy + note in " + $distDir)

Write-Host ""
Write-Host "  Done. Launch it from:" -ForegroundColor Green
Write-Host ("    " + $final)
Write-Host "    (or the desktop shortcut)"
Write-Host ""
Write-Host "  First page lets you choose:" -ForegroundColor Green
Write-Host "    [ Host debugger ]  live monitor / actuator test / parameters"
Write-Host "    [ Flash firmware]  one-click flash of the bundled firmware"
Write-Host ""
exit 0
