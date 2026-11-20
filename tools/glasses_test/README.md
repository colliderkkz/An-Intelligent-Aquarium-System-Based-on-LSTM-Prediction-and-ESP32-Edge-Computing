# 眼镜显示最小测试件（tools/glasses_test）

在 **不改造现有 FishTankConsole、不接 MQTT、不装眼镜端 APK** 的前提下，
用 Rokid CXR-L SDK 的 `CUSTOM_VIEW` 能力，把一张鱼缸数据卡片推到 Rokid 眼镜上显示。

配套文档：[真机 5 分钟测试](../../docs/ar_glasses/真机5分钟测试.md)
交接文档：[AR 眼镜显示-交接文档](../../docs/ar_glasses/AR眼镜显示-交接文档.md)（Word 版同目录 `.docx`）

## 目录结构

```
tools/glasses_test/
├─ AndroidManifest.xml          包名 com.fishtank.glassestest，minSdk 28 / targetSdk 34，蓝牙权限 + queries
├─ java/com/fishtank/glassestest/MainActivity.java
│                               五个按钮：检查环境 / 授权 / 连接并显示 / 手动刷新 / 关闭
├─ res/values/themes.xml        系统 Material 主题（不依赖 AppCompat）
├─ assets/viewdata.json         CUSTOM_VIEW 布局模板（纯 TextView，深色底 + 绿字）
├─ deps/                        依赖缓存（首次构建自动下载，可删）
└─ README.md
```

构建脚本：`tools/build_glasses_test.ps1`，产物：`dist/FishTankGlassesTest.apk`。

## 为什么是「纯 Java + 手工打包」

现有 APK 是 `aapt2 + javac + d8` 手工链路，没有 Gradle / Kotlin / AndroidX。
CXR-L SDK 是 Kotlin 写的 AAR，但公开 API 全部可以从 Java 调用：

| 需要的东西 | Java 写法 |
|---|---|
| 入口单例 | `CxrSessionManager.Companion.getInstance(context)` |
| 授权 | `requestAuthorization(activity, List<GlassPermission>, Function1<AuthResult, Unit>)` |
| 会话配置 | `new SessionConfig(SessionType.CUSTOM_VIEW, null, AiInterceptMode.ALLOW_WITH_PAUSE, 5000L, new SessionTimeouts(), viewData, "[]", null, null)` |
| 建链 / 更新 | `session.connect(token)` / `session.customViewUpdate(json)` |

依赖（已验证可从公网仓库取到，缓存在 `deps/`）：

| 依赖 | 版本 / 来源 |
|---|---|
| `com.rokid.cxr:client-l` | 1.1.2，`maven.rokid.com` |
| `com.rokid.cxr:cxr-service-bridge` | `1.0-20260715.121510-107`（client-l 的 POM 声明版本） |
| `kotlin-stdlib` / `kotlinx-coroutines` / `gson` | Maven Central |

## 构建

```powershell
cd code\fish_panzer_code1.0
powershell -NoProfile -ExecutionPolicy Bypass -File tools\build_glasses_test.ps1
```

首次构建会下载依赖到 `deps/`（用本机 Python 下载；PowerShell/curl 在本机走 schannel 会 TLS 失败）。
脚本会：

1. 用 `tools/viewdata_check.py` 校验 `assets/viewdata.json`；
2. 把工程复制到 ASCII 临时目录（`aapt2` 处理不了含中文的仓库路径）；
3. `aapt2 compile/link` → `javac` → `d8` → `aapt add`（dex + 两种 ABI 的 5 个 `.so`）→ `zipalign` → `apksigner`；
4. 自检：`aapt dump badging`（包名/minSdk/权限）、`apksigner verify`、APK 内容（`classes.dex`、`assets/viewdata.json`、`lib/**/*.so`）。

> `d8` 会打印若干 `malformed kotlin.Metadata` 警告：`build-tools 34` 里的 R8/D8 比 kotlin 1.9 旧，
> 只是重写元数据失败，不影响运行，可忽略。

## 离线校验模板

```powershell
python tools\viewdata_check.py --self-test                 # 内置正反例
python tools\viewdata_check.py                             # 校验 assets/viewdata.json
python tools\viewdata_check.py --state 某个 state.json      # 遥测 -> 增量更新（演示映射）
```

## 本测试件不做什么

- 不连 NAS / MOSQUITTO，不订阅、不发布任何 MQTT 消息；
- 不改固件、不改 `android/` 里的现有控制台工程；
- 不做眼镜上控制（`CUSTOM_VIEW` 没有点击事件，控制仍在手机页面）；
- 不验证真机效果——授权、渲染、锁屏表现必须按真机测试文档执行。

## 已知限制（下一阶段再处理）

- 卡片数据是内置样例，每 2 秒微调；
- 后台保活未做（本轮先看前台表现，G/H 项记录后决定是否加前台服务）；
- 图标能力未用（避免 `viewIconData` 与建链数据量的额外变量）。
