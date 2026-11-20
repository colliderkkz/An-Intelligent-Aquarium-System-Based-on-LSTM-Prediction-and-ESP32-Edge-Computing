# 手工构建「眼镜显示测试」APK（不依赖 Gradle / Android Studio / Kotlin 编译器）
#
# 背景：
#   * 现有 FishTankConsole 是 aapt2 + javac + d8 手工打包的，没有 Gradle；
#   * Rokid CXR-L SDK 是 Kotlin 写的 AAR，但公开 API 可以从 Java 调用；
#   * 所以这里复用同一套手工链路，把 AAR + 几个 Kotlin/JSON 运行时 jar 一起 dex 进 APK。
#
# 用法：
#   pwsh tools/build_glasses_test.ps1
#   pwsh tools/build_glasses_test.ps1 -Clean        # 先清工作目录
#
# 依赖第一次会自动下载到 tools/glasses_test/deps/（用本机 Python 下载：
#   PowerShell/curl 在本机走 schannel 会报 SEC_E_NO_CREDENTIALS，Python 的 OpenSSL 正常）。

param(
    [switch]$Clean,
    [switch]$KeepWork,
    [switch]$DependenciesOnly
)

$ErrorActionPreference = 'Stop'

# ------------------------------------------------------------------ 路径

$Proj    = Split-Path -Parent $PSScriptRoot          # code/fish_panzer_code1.0
$AppDir  = Join-Path $PSScriptRoot 'glasses_test'
$Deps    = Join-Path $AppDir 'deps'
$Dist    = Join-Path $Proj 'dist'
$Tools   = 'D:\android_build'
$Jdk     = Join-Path $Tools 'jdk'
$Bt      = Join-Path $Tools 'sdk\build-tools\34.0.0'
$PlatJar = Join-Path $Tools 'sdk\platforms\android-34\android.jar'
$Ks      = Join-Path $Tools 'fishtank.keystore'
$KsPass  = $env:FISHTANK_KEYSTORE_PASSWORD
$KsAlias = 'fishtank'
$Work    = Join-Path $env:TEMP 'glasses_test_build'
$OutApk  = Join-Path $Dist 'FishTankGlassesTest.apk'

function Step($m) { Write-Host ("[build] " + $m) }
function Fail($m) { Write-Host ("[build][ERR] " + $m) -ForegroundColor Red; exit 1 }
function Have($p) { Test-Path -LiteralPath $p }

# ------------------------------------------------------------------ 找 Python（只用于下载 + 解压）

function Resolve-Python {
    foreach ($cand in @(
            'C:\Users\Administrator\AppData\Local\Python\bin\python.exe',
            (Join-Path $env:LOCALAPPDATA 'Python\bin\python.exe'))) {
        if (Test-Path -LiteralPath $cand) { return $cand }
    }
    $g = Get-Command python -ErrorAction SilentlyContinue
    if ($g) { return $g.Source }
    return $null
}
$Py = Resolve-Python

# ------------------------------------------------------------------ 依赖清单

$Artifacts = @(
    @{ File = 'client-l-1.1.2.aar';        Url = 'https://maven.rokid.com/repository/maven-public/com/rokid/cxr/client-l/1.1.2/client-l-1.1.2.aar'; Kind = 'aar' },
    @{ File = 'cxr-service-bridge.aar';    Url = 'https://maven.rokid.com/repository/maven-public/com/rokid/cxr/cxr-service-bridge/1.0-SNAPSHOT/cxr-service-bridge-1.0-20260715.121510-107.aar'; Kind = 'aar' },
    @{ File = 'kotlin-stdlib-1.9.0.jar';   Url = 'https://repo1.maven.org/maven2/org/jetbrains/kotlin/kotlin-stdlib/1.9.0/kotlin-stdlib-1.9.0.jar'; Kind = 'jar' },
    @{ File = 'gson-2.10.1.jar';           Url = 'https://repo1.maven.org/maven2/com/google/code/gson/gson/2.10.1/gson-2.10.1.jar'; Kind = 'jar' },
    @{ File = 'coroutines-core-1.9.0.jar'; Url = 'https://repo1.maven.org/maven2/org/jetbrains/kotlinx/kotlinx-coroutines-core-jvm/1.9.0/kotlinx-coroutines-core-jvm-1.9.0.jar'; Kind = 'jar' },
    @{ File = 'coroutines-android-1.9.0.jar'; Url = 'https://repo1.maven.org/maven2/org/jetbrains/kotlinx/kotlinx-coroutines-android/1.9.0/kotlinx-coroutines-android-1.9.0.jar'; Kind = 'jar' }
)

function Get-Artifact($file, $url) {
    $dest = Join-Path $Deps $file
    if ((Have $dest) -and ((Get-Item $dest).Length -gt 0)) { return $dest }
    if (-not $Py) {
        Fail "缺少 $file 且本机没有可用的 Python；请手工下载放到 $Deps`n         $url"
    }
    Step "下载 $file ..."
    New-Item -ItemType Directory -Force -Path $Work | Out-Null
    # 用脚本文件而不是 python -c：Windows PowerShell 5.1 传参会把内层引号吃掉
    $pyFile = Join-Path $Work 'download.py'
    $pyCode = "import sys, urllib.request`nu, p = sys.argv[1], sys.argv[2]`ndata = urllib.request.urlopen(u, timeout=180).read()`nopen(p, 'wb').write(data)`nprint(len(data))`n"
    Set-Content -Path $pyFile -Value $pyCode -Encoding ASCII
    $null = & $Py $pyFile $url $dest
    if ($LASTEXITCODE -ne 0 -or -not (Have $dest)) { Fail "下载失败：$url" }
    return $dest
}

if ($DependenciesOnly) {
    New-Item -ItemType Directory -Force -Path $Deps | Out-Null
    foreach ($a in $Artifacts) { $null = Get-Artifact $a.File $a.Url }
    return
}

if (-not $KsPass) { throw "Set FISHTANK_KEYSTORE_PASSWORD before building." }
# ------------------------------------------------------------------ 前置检查

if (-not (Have (Join-Path $Jdk 'bin\javac.exe'))) { Fail "缺少 JDK17：$Jdk" }
if (-not (Have $PlatJar)) { Fail "缺少 android.jar：$PlatJar" }
foreach ($exe in 'aapt2.exe', 'aapt.exe', 'd8.bat', 'zipalign.exe', 'apksigner.bat') {
    if (-not (Have (Join-Path $Bt $exe))) { Fail "缺少 build-tools 组件：$exe" }
}
if (-not (Have $Ks)) { Fail "缺少签名 keystore：$Ks（可先跑 tools/build_apk.ps1 生成）" }

$env:JAVA_HOME = $Jdk
$env:PATH = "$Jdk\bin;$env:PATH"
$env:PYTHONIOENCODING = 'utf-8'
try { [Console]::OutputEncoding = [Text.Encoding]::UTF8 } catch { }
New-Item -ItemType Directory -Force -Path $Deps, $Dist | Out-Null

# ------------------------------------------------------------------ 准备目录

if ($Clean -and (Have $Work)) { Remove-Item $Work -Recurse -Force }
$Src     = Join-Path $Work 'src'
$Classes = Join-Path $Work 'classes'
$DexDir  = Join-Path $Work 'dex'
$Stage   = Join-Path $Work 'stage'
$ResFlat = Join-Path $Work 'res.zip'
$BaseApk = Join-Path $Work 'base.apk'
foreach ($d in $Classes, $DexDir, $Stage) {
    New-Item -ItemType Directory -Force -Path $d | Out-Null
}
Remove-Item (Join-Path $DexDir '*') -Force -ErrorAction SilentlyContinue
Remove-Item (Join-Path $Stage '*') -Recurse -Force -ErrorAction SilentlyContinue

# aapt2 处理不了中文路径（仓库路径含“总”），把工程复制到纯 ASCII 的工作目录再编译
$AppSrc = Join-Path $Work 'app'
if (Have $AppSrc) { Remove-Item $AppSrc -Recurse -Force }
New-Item -ItemType Directory -Force -Path $AppSrc | Out-Null
Copy-Item (Join-Path $AppDir 'AndroidManifest.xml') $AppSrc -Force
foreach ($sub in 'res', 'assets', 'java') {
    Copy-Item (Join-Path $AppDir $sub) (Join-Path $AppSrc $sub) -Recurse -Force
}
Step ("工程已复制到 ASCII 路径：" + $AppSrc)

# ------------------------------------------------------------------ 依赖：下载并解 AAR

$Jars = New-Object System.Collections.Generic.List[string]
$JniRoot = Join-Path $Work 'jni'
if (Have $JniRoot) { Remove-Item $JniRoot -Recurse -Force }

foreach ($a in $Artifacts) {
    $path = Get-Artifact $a.File $a.Url
    if ($a.Kind -eq 'jar') {
        $Jars.Add($path) | Out-Null
        continue
    }
    if (-not $Py) { Fail "解 AAR 需要 Python" }
    $ex = Join-Path $Work ('aar_' + [IO.Path]::GetFileNameWithoutExtension($a.File))
    if (Have $ex) { Remove-Item $ex -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $ex | Out-Null
    $unzipPy = Join-Path $Work 'unzip.py'
    Set-Content -Path $unzipPy -Encoding ASCII -Value "import sys, zipfile`nzipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])`n"
    & $Py $unzipPy $path $ex
    if ($LASTEXITCODE -ne 0) { Fail "解压失败：$($a.File)" }
    $cj = Join-Path $ex 'classes.jar'
    if (-not (Have $cj)) { Fail "$($a.File) 里没有 classes.jar" }
    $jarname = Join-Path $Work ([IO.Path]::GetFileNameWithoutExtension($a.File) + '-classes.jar')
    Copy-Item $cj $jarname -Force
    $Jars.Add($jarname) | Out-Null
    $jn = Join-Path $ex 'jni'
    if (Have $jn) {
        Copy-Item $jn $JniRoot -Recurse -Force
    }
    Step ("依赖就绪：" + $a.File)
}

# ------------------------------------------------------------------ 校验 viewData（离线 schema 检查）

if ($Py) {
    Step "校验 assets/viewdata.json ..."
    & $Py (Join-Path $PSScriptRoot 'viewdata_check.py') (Join-Path $AppDir 'assets\viewdata.json')
    if ($LASTEXITCODE -ne 0) { Fail "viewData 校验未通过，先修模板再打包" }
} else {
    Step "警告：没有 Python，跳过 viewData 校验"
}

# ------------------------------------------------------------------ aapt2 compile / link

Step "aapt2 compile ..."
Remove-Item $ResFlat -Force -ErrorAction SilentlyContinue
& (Join-Path $Bt 'aapt2.exe') compile --dir (Join-Path $AppSrc 'res') -o $ResFlat
if ($LASTEXITCODE -ne 0) { Fail "aapt2 compile 失败" }

Step "aapt2 link ..."
Remove-Item $BaseApk -Force -ErrorAction SilentlyContinue
& (Join-Path $Bt 'aapt2.exe') link `
    -o $BaseApk `
    -I $PlatJar `
    --manifest (Join-Path $AppSrc 'AndroidManifest.xml') `
    -R $ResFlat `
    -A (Join-Path $AppSrc 'assets') `
    --auto-add-overlay `
    --min-sdk-version 28 `
    --target-sdk-version 34 `
    --version-code 1 --version-name 1.0
if ($LASTEXITCODE -ne 0) { Fail "aapt2 link 失败" }

# ------------------------------------------------------------------ javac

Step "javac ..."
$srcs = New-Object System.Collections.Generic.List[string]
Get-ChildItem (Join-Path $AppSrc 'java') -Recurse -Filter *.java | ForEach-Object { $srcs.Add($_.FullName) }
$cp = ($Jars + $PlatJar) -join ';'
& (Join-Path $Jdk 'bin\javac.exe') -encoding UTF-8 -source 17 -target 17 -Xlint:-options -nowarn `
    -classpath $cp -d $Classes @srcs
if ($LASTEXITCODE -ne 0) { Fail "javac 失败" }
Step ("  源文件 " + $srcs.Count + " 个")

# ------------------------------------------------------------------ d8

Step "d8 ..."
$appJar = Join-Path $Work 'app-classes.jar'
Remove-Item $appJar -Force -ErrorAction SilentlyContinue
& (Join-Path $Jdk 'bin\jar.exe') cf $appJar -C $Classes .
if ($LASTEXITCODE -ne 0) { Fail "打包 app-classes.jar 失败" }

$inputs = New-Object System.Collections.Generic.List[string]
$inputs.Add($appJar)
foreach ($j in $Jars) { $inputs.Add($j) }
& (Join-Path $Bt 'd8.bat') --lib $PlatJar --min-api 28 --release --output $DexDir @inputs
if ($LASTEXITCODE -ne 0) { Fail "d8 失败" }
$dexFiles = @(Get-ChildItem $DexDir -Filter 'classes*.dex')
if ($dexFiles.Count -eq 0) { Fail "d8 没有产出 classes.dex" }
Step ("  dex: " + (($dexFiles | ForEach-Object { $_.Name + '(' + [math]::Round($_.Length/1KB) + 'KB)' }) -join ', '))

# ------------------------------------------------------------------ 组装 APK

Step "组装 APK（dex + native）..."
foreach ($d in $dexFiles) { Copy-Item $d.FullName (Join-Path $Stage $d.Name) -Force }
if (Have $JniRoot) {
    $libDst = Join-Path $Stage 'lib'
    New-Item -ItemType Directory -Force -Path $libDst | Out-Null
    Get-ChildItem $JniRoot -Directory | ForEach-Object {
        $abi = $_.Name
        $dst = Join-Path $libDst $abi
        New-Item -ItemType Directory -Force -Path $dst | Out-Null
        Get-ChildItem $_.FullName -Filter *.so | ForEach-Object { Copy-Item $_.FullName $dst -Force }
    }
    Step ("  native: " + ((Get-ChildItem $libDst -Recurse -Filter *.so | Measure-Object).Count) + " 个 .so")
} else {
    Step "  警告：AAR 里没有 jni/，APK 将不含 native 库"
}

Push-Location $Stage
try {
    $adds = @()
    $adds += @($dexFiles | ForEach-Object { $_.Name })
    $libDir = Join-Path $Stage 'lib'
    if (Have $libDir) {
        Get-ChildItem $libDir -Directory | ForEach-Object {
            $abi = $_.Name
            Get-ChildItem $_.FullName -Filter *.so | ForEach-Object {
                $adds += ('lib/' + $abi + '/' + $_.Name)
            }
        }
    }
    foreach ($rel in $adds) {
        & (Join-Path $Bt 'aapt.exe') add $BaseApk $rel
        if ($LASTEXITCODE -ne 0) { Fail "aapt add 失败：$rel" }
    }
    Step ("  已加入 " + $adds.Count + " 个文件")
} finally {
    Pop-Location
}

# ------------------------------------------------------------------ zipalign + 签名

Step "zipalign ..."
$aligned = Join-Path $Work 'aligned.apk'
Remove-Item $aligned -Force -ErrorAction SilentlyContinue
& (Join-Path $Bt 'zipalign.exe') -f -p 4 $BaseApk $aligned
if ($LASTEXITCODE -ne 0) { Fail "zipalign 失败" }

Step "apksigner 签名 ..."
Remove-Item $OutApk -Force -ErrorAction SilentlyContinue
& (Join-Path $Bt 'apksigner.bat') sign `
    --ks $Ks --ks-pass env:FISHTANK_KEYSTORE_PASSWORD --key-pass env:FISHTANK_KEYSTORE_PASSWORD `
    --min-sdk-version 28 --out $OutApk $aligned
if ($LASTEXITCODE -ne 0) { Fail "apksigner 签名失败" }

# ------------------------------------------------------------------ 自检

Step "自检 ..."
# aapt.exe 同样处理不了中文路径：复制到 ASCII 工作目录再验
$verifyApk = Join-Path $Work 'verify.apk'
Copy-Item $OutApk $verifyApk -Force
$badging = & (Join-Path $Bt 'aapt.exe') dump badging $verifyApk 2>&1
$keyLines = $badging | Where-Object { $_ -match '^package:|^sdkVersion:|^targetSdkVersion:|^uses-permission:' }
$keyLines | ForEach-Object { "        " + $_ }

$signOut = & (Join-Path $Bt 'apksigner.bat') verify --print-certs $verifyApk 2>&1
$signRc = $LASTEXITCODE
$signOut | Select-Object -First 4 | ForEach-Object { "        " + $_ }
if ($signRc -ne 0) { Fail "签名校验失败" }

if ($Py) {
    $checkPy = Join-Path $Work 'check_apk.py'
    $checkCode = @(
        'import sys, zipfile',
        'p = sys.argv[1]',
        'z = zipfile.ZipFile(p)',
        'names = z.namelist()',
        'need = ["classes.dex", "assets/viewdata.json"]',
        'libs = [n for n in names if n.startswith("lib/") and n.endswith(".so")]',
        'missing = [n for n in need if n not in names]',
        'print("entries:", len(names), "| so:", len(libs))',
        'for n in libs[:6]:',
        '    print("  ", n, z.getinfo(n).file_size)',
        'if missing:',
        '    print("MISSING:", missing)',
        '    sys.exit(1)'
    ) -join "`n"
    Set-Content -Path $checkPy -Value $checkCode -Encoding ASCII
    & $Py $checkPy $OutApk
    if ($LASTEXITCODE -ne 0) { Fail "APK 内容自检失败" }
}

$size = [math]::Round((Get-Item $OutApk).Length / 1KB)
$hash = (Get-FileHash $OutApk -Algorithm SHA256).Hash
Step ("完成：" + $OutApk)
Write-Host ("        大小 " + $size + " KB")
Write-Host ("        SHA256 " + $hash)
Write-Host "        下一步：把 APK 装到手机，按 docs/ar_glasses/真机5分钟测试.md 操作"

if (-not $KeepWork -and (Have $Work)) {
    # 保留依赖缓存；工作目录清掉，避免占用空间
    Remove-Item (Join-Path $Work 'aar_client-l-1.1.2') -Recurse -Force -ErrorAction SilentlyContinue
}
