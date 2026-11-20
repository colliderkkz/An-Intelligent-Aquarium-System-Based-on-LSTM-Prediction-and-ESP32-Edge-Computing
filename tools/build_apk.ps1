# 手工构建 APK（不依赖 Gradle / Kotlin / AndroidX）
# 关键点：源码会被复制到纯 ASCII 路径再构建 —— aapt2 处理不了中文路径
$ErrorActionPreference = 'Continue'

if (-not $env:FISHTANK_KEYSTORE_PASSWORD) { throw "Set FISHTANK_KEYSTORE_PASSWORD before building." }
$ROOT = 'D:\android_build'
$DL   = Join-Path $ROOT 'dl'
$JDK  = Join-Path $ROOT 'jdk'
$SDK  = Join-Path $ROOT 'sdk'
$PROJ = Join-Path (Split-Path -Parent $PSScriptRoot) 'android'
$OUT  = Join-Path $ROOT 'out'
$WORK = Join-Path $ROOT 'work'

function Step($m) { Write-Output ("[build] " + $m) }
function Fail($m) { Write-Output ("[build][ERR] " + $m); exit 1 }

# ---------- 1) JDK ----------
if (-not (Test-Path (Join-Path $JDK 'bin\javac.exe'))) {
    Step "解压 JDK ..."
    New-Item -ItemType Directory -Force -Path $JDK | Out-Null
    Expand-Archive -Path (Join-Path $DL 'jdk17.zip') -DestinationPath $JDK -Force
    $inner = Get-ChildItem $JDK -Directory | Where-Object { Test-Path (Join-Path $_.FullName 'bin\javac.exe') } | Select-Object -First 1
    if ($inner -and $inner.FullName -ne $JDK) {
        Get-ChildItem $inner.FullName -Force | Move-Item -Destination $JDK -Force
        Remove-Item $inner.FullName -Recurse -Force -ErrorAction SilentlyContinue
    }
}
$env:JAVA_HOME = $JDK
$env:PATH = "$JDK\bin;" + $env:PATH
Step ("JDK: " + ((& "$JDK\bin\javac.exe" -version 2>&1 | Out-String).Trim()))

$ANDROID_JAR = Join-Path $SDK 'platforms\android-34\android.jar'
$BT = Join-Path $SDK 'build-tools\34.0.0'
if (-not (Test-Path $ANDROID_JAR)) { Fail "缺少 android.jar" }
if (-not (Test-Path (Join-Path $BT 'aapt2.exe'))) { Fail "缺少 aapt2" }
Step "SDK: platform 34 + build-tools 34.0.0 OK"

# ---------- 2) 复制源码到 ASCII 路径 ----------
if (Test-Path $WORK) { Remove-Item $WORK -Recurse -Force }
New-Item -ItemType Directory -Force -Path $WORK, $OUT | Out-Null
$SRC = Join-Path $WORK 'src'
$APPMAIN = Join-Path $PROJ 'app\src\main'
if (-not (Test-Path $APPMAIN)) { Fail "找不到 app\src\main" }
Copy-Item $APPMAIN $SRC -Recurse -Force
Step ("源码复制到 ASCII 路径: " + $SRC)

$RES      = Join-Path $SRC 'res'
$ASSETS   = Join-Path $SRC 'assets'
$MANIFEST = Join-Path $SRC 'AndroidManifest.xml'
$JAVA     = Join-Path $SRC 'java'
foreach ($p in @($RES, $ASSETS, $MANIFEST, $JAVA)) {
    if (-not (Test-Path $p)) { Fail "缺少 $p" }
}
# aapt2 link 要求 manifest 有 package 属性；但 Gradle 8 禁止它（用 namespace）。
# 所以只在构建用的副本里注入，原工程保持 Gradle 兼容。
$mfText = Get-Content $MANIFEST -Raw -Encoding UTF8
if ($mfText -notmatch 'package\s*=') {
    $mfText = $mfText -replace '<manifest ', '<manifest package="com.fishtank.console" '
    Set-Content -Path $MANIFEST -Value $mfText -Encoding UTF8 -NoNewline
    Step "已向构建副本注入 package 属性"
}

Step ("assets/index.html " + [math]::Round((Get-Item (Join-Path $ASSETS 'index.html')).Length / 1KB) + " KB")

$GEN = Join-Path $WORK 'gen'
$CLS = Join-Path $WORK 'classes'
$DEX = Join-Path $WORK 'dex'
$STG = Join-Path $WORK 'stage'
New-Item -ItemType Directory -Force -Path $GEN, $CLS, $DEX, $STG | Out-Null

# ---------- 3) aapt2 compile ----------
Step "aapt2 compile ..."
$flat = Join-Path $WORK 'res.zip'
& (Join-Path $BT 'aapt2.exe') compile --dir $RES -o $flat
if ($LASTEXITCODE -ne 0) { Fail "aapt2 compile 失败" }

# ---------- 4) aapt2 link ----------
Step "aapt2 link ..."
$baseApk = Join-Path $WORK 'base.apk'
& (Join-Path $BT 'aapt2.exe') link `
    -o $baseApk -I $ANDROID_JAR --manifest $MANIFEST -R $flat --auto-add-overlay `
    --java $GEN -A $ASSETS `
    --min-sdk-version 24 --target-sdk-version 34 `
    --version-code 1 --version-name 1.0
if ($LASTEXITCODE -ne 0) { Fail "aapt2 link 失败" }

# ---------- 4.5) 眼镜 SDK 依赖（AAR / JAR）----------
# 手工链没有 Gradle 的依赖解析，这里直接吃 tools\glasses_test\deps：
#   *.jar  -> javac classpath + d8 输入
#   *.aar  -> 解出 classes.jar 进 d8，jni/<abi>/*.so 进 APK 的 lib/<abi>/
# 与 build_glasses_test.ps1 是同一套依赖、同一套打包方式
# （那条链路已跑通 CXR-L 1.1.2，这里不另起炉灶）。
Step "眼镜 SDK 依赖（aar/jar）..."
$DEPS   = Join-Path (Split-Path -Parent $PROJ) 'tools\glasses_test\deps'
& (Join-Path $PSScriptRoot 'build_glasses_test.ps1') -DependenciesOnly
if ($LASTEXITCODE -ne 0) { Fail "眼镜 SDK 依赖下载失败" }
$NATIVE = Join-Path $WORK 'native'
New-Item -ItemType Directory -Force -Path $NATIVE | Out-Null
$LIBJARS = New-Object System.Collections.Generic.List[string]
if (Test-Path $DEPS) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem -ErrorAction SilentlyContinue
    foreach ($dep in (Get-ChildItem $DEPS -File | Sort-Object Name)) {
        $ext = $dep.Extension.ToLower()
        if ($ext -eq '.jar') {
            $LIBJARS.Add($dep.FullName)
            Step ("  jar  " + $dep.Name)
            continue
        }
        if ($ext -ne '.aar') { continue }
        $zip = [System.IO.Compression.ZipFile]::OpenRead($dep.FullName)
        $so = 0
        try {
            foreach ($e in $zip.Entries) {
                if ($e.FullName -eq 'classes.jar') {
                    # 变量名不能叫 $out（PowerShell 大小写不敏感，会踩到上面的 $OUT 产物目录）
                    $jarOut = Join-Path $WORK ($dep.BaseName + '-classes.jar')
                    [System.IO.Compression.ZipFileExtensions]::ExtractToFile($e, $jarOut, $true)
                    $LIBJARS.Add($jarOut)
                } elseif ($e.FullName -like 'jni/*/*.so') {
                    $dst = Join-Path $NATIVE ($e.FullName.Substring(4))   # 去掉 "jni/"
                    New-Item -ItemType Directory -Force -Path (Split-Path $dst) | Out-Null
                    [System.IO.Compression.ZipFileExtensions]::ExtractToFile($e, $dst, $true)
                    $so++
                }
            }
        } finally {
            $zip.Dispose()
        }
        Step ("  aar  " + $dep.Name + " -> classes.jar + " + $so + " 个 .so")
    }
}
if ($LIBJARS.Count -eq 0) {
    Fail ("找不到眼镜 SDK 依赖：" + $DEPS + "（AR 页签会不可用）")
}
$CP = $ANDROID_JAR + ';' + ($LIBJARS -join ';')
$soCount = @(Get-ChildItem $NATIVE -Recurse -Filter *.so -ErrorAction SilentlyContinue).Count
Step ("  依赖 " + $LIBJARS.Count + " 个 jar，native .so " + $soCount + " 个")

# ---------- 5) javac ----------
Step "javac ..."
$srcs = New-Object System.Collections.Generic.List[string]
Get-ChildItem $JAVA -Recurse -Filter *.java | ForEach-Object { $srcs.Add($_.FullName) }
Get-ChildItem $GEN  -Recurse -Filter *.java -ErrorAction SilentlyContinue | ForEach-Object { $srcs.Add($_.FullName) }
Step ("  源文件 " + $srcs.Count + " 个")
& "$JDK\bin\javac.exe" -encoding UTF-8 -source 17 -target 17 -nowarn -classpath $CP -d $CLS @srcs
if ($LASTEXITCODE -ne 0) { Fail "javac 失败" }

# ---------- 6) d8 ----------
Step "d8 ..."
$appJar = Join-Path $WORK 'app-classes.jar'
if (Test-Path $appJar) { Remove-Item $appJar -Force }
& "$JDK\bin\jar.exe" cf $appJar -C $CLS .
if ($LASTEXITCODE -ne 0) { Fail "打包 app-classes.jar 失败" }
$dexInputs = New-Object System.Collections.Generic.List[string]
$dexInputs.Add($appJar)
foreach ($j in $LIBJARS) { $dexInputs.Add($j) }
# d8 对 kotlin 1.9 会刷几千行 "malformed kotlin.Metadata"（build-tools 34 的 R8 比
# kotlin 旧，只是元数据重写失败，不影响产物；glasses_test 已实测两次产物一致）。
# 这里把 d8 输出落到文件里，只在失败时给尾巴，构建日志保持可读。
$d8Log = Join-Path $WORK 'd8.log'
& (Join-Path $BT 'd8.bat') --lib $ANDROID_JAR --min-api 24 --output $DEX @dexInputs 2>&1 |
    Out-File -FilePath $d8Log -Encoding utf8
if ($LASTEXITCODE -ne 0) {
    Get-Content $d8Log -Tail 25 | ForEach-Object { "   " + $_ }
    Fail ("d8 失败（完整日志 " + $d8Log + "）")
}
$d8Warn = @(Select-String -Path $d8Log -Pattern 'Warning:' -ErrorAction SilentlyContinue).Count
Step ("  d8 警告 " + $d8Warn + " 条（kotlin 元数据重写，已知无害）")
$dexFiles = @(Get-ChildItem $DEX -Filter 'classes*.dex' | Sort-Object Name)
if ($dexFiles.Count -eq 0) { Fail "d8 没产出 classes.dex" }
Step ("  dex: " + (($dexFiles | ForEach-Object { $_.Name + '(' + [math]::Round($_.Length/1KB) + 'KB)' }) -join ', '))

# ---------- 7) dex + native 进 APK ----------
Step "加入 dex / native ..."
$libStage = Join-Path $STG 'lib'
if (Test-Path $libStage) { Remove-Item $libStage -Recurse -Force }
foreach ($d in $dexFiles) { Copy-Item $d.FullName (Join-Path $STG $d.Name) -Force }
$adds = New-Object System.Collections.Generic.List[string]
foreach ($d in $dexFiles) { $adds.Add($d.Name) }
foreach ($abiDir in (Get-ChildItem $NATIVE -Directory -ErrorAction SilentlyContinue)) {
    $dst = Join-Path $libStage $abiDir.Name
    New-Item -ItemType Directory -Force -Path $dst | Out-Null
    foreach ($soFile in (Get-ChildItem $abiDir.FullName -Filter *.so)) {
        Copy-Item $soFile.FullName $dst -Force
        $adds.Add('lib/' + $abiDir.Name + '/' + $soFile.Name)
    }
}
Push-Location $STG
try {
    foreach ($rel in $adds) {
        & (Join-Path $BT 'aapt.exe') add $baseApk $rel
        if ($LASTEXITCODE -ne 0) { Fail ("aapt add 失败：" + $rel) }
    }
} finally {
    Pop-Location
}
Step ("  已加入 " + $adds.Count + " 个文件（dex " + $dexFiles.Count + "，so " + $soCount + "）")

# ---------- 8) zipalign ----------
Step "zipalign ..."
$aligned = Join-Path $WORK 'aligned.apk'
& (Join-Path $BT 'zipalign.exe') -f -p 4 $baseApk $aligned
if ($LASTEXITCODE -ne 0) { Fail "zipalign 失败" }

# ---------- 9) 签名 ----------
$ks = Join-Path $ROOT 'fishtank.keystore'
if (-not (Test-Path $ks)) {
    Step "生成签名密钥 ..."
    & "$JDK\bin\keytool.exe" -genkeypair -keystore $ks -storepass:env FISHTANK_KEYSTORE_PASSWORD `
        -keypass:env FISHTANK_KEYSTORE_PASSWORD -alias fishtank -keyalg RSA -keysize 2048 -validity 10000 `
        -dname "CN=FishTank, OU=Home, O=Home, L=SH, ST=SH, C=CN"
    if ($LASTEXITCODE -ne 0) { Fail "keytool 失败" }
}

Step "apksigner 签名 ..."
$final = Join-Path $OUT 'FishTankConsole.apk'
if (Test-Path $final) { Remove-Item $final -Force }
& (Join-Path $BT 'apksigner.bat') sign `
    --ks $ks --ks-pass env:FISHTANK_KEYSTORE_PASSWORD --key-pass env:FISHTANK_KEYSTORE_PASSWORD `
    --min-sdk-version 24 --out $final $aligned
if ($LASTEXITCODE -ne 0) { Fail "apksigner 失败" }

Step "校验签名 ..."
& (Join-Path $BT 'apksigner.bat') verify --print-certs $final 2>&1 | Select-Object -First 4 | ForEach-Object { "   " + $_ }

$sz = [math]::Round((Get-Item $final).Length / 1KB)
Step ("完成: " + $final + "   (" + $sz + " KB)")
