# 鱼缸控制台 Android App

把手机网页控制台包成 APK。界面就是 `app/src/main/assets/index.html`（自包含，mqtt.js 已内嵌）。

## 构建方式一：Android Studio（最简单）

1. 装 [Android Studio](https://developer.android.com/studio)（自带 JDK + SDK + Gradle）
2. `File → Open`，选这个 `android` 目录
3. 等 Gradle 同步完成（首次会下载依赖，约几分钟）
4. 菜单 `Build → Build Bundle(s) / APK(s) → Build APK(s)`
5. 生成的 APK 在：
   ```
   app/build/outputs/apk/debug/app-debug.apk
   ```
6. 传到手机安装（需允许"未知来源"）

## 构建方式二：命令行

需要 JDK 17 + Android SDK（`ANDROID_HOME` 指向 SDK 目录）：

```bash
cd android
# 首次需要生成 wrapper（Android Studio 会自动做）
gradle wrapper --gradle-version 8.5
./gradlew assembleDebug
# 输出: app/build/outputs/apk/debug/app-debug.apk
```

## 构建方式三：在线打包（不想装 SDK）

把 `app/src/main/assets/index.html` 上传到任意在线 Web→APK 服务
（如 WebIntoApp / Median / AppsGeyser），用 "HTML 本地文件" 模式，
设置：

- 允许 JavaScript
- 允许 DOM Storage  
- **允许明文流量（HTTP/WS）** ← 关键，否则 `ws://` 连不上
- 全屏 / 隐藏地址栏

## 关键配置（已设好）

| 配置 | 值 | 为什么 |
|---|---|---|
| `android:usesCleartextTraffic` | `true` | 页面用 `ws://` 明文 WebSocket，Android 9+ 默认禁止 |
| `javaScriptEnabled` | `true` | 页面全靠 JS |
| `domStorageEnabled` | `true` | 记住 broker 地址和设备 ID |
| `allowFileAccess` | `true` | 从 `file:///android_asset/` 加载 |
| `minSdk` | 24（Android 7.0） | 覆盖绝大多数设备 |

## 改界面怎么办

只需要改 `app/src/main/assets/index.html`，重新 Build 即可。
（这个文件就是电脑上的 `tools/phone_console.html` 复制过来的）

## 功能

五个页签，对应电脑上位机：

| 页签 | 内容 |
|---|---|
| 监控 | 7 路传感器 + 模式 + 板子时间/IP |
| 执行器 | 6 路开关 + 灯光档位/滑条 + 喂食 + 返回自动 |
| 计划 | 定时计划列表 |
| SD | 卡状态（挂载/记录/行数/文件大小）+ 传感器写入流 + 日志写入流 |
| 日志 | 板子日志，按 E/W/I 着色，"只看告警"过滤 |

## 网络

- 默认连公网 `ws://broker.emqx.io:8083/mqtt`，**任何网络都能用**
- 连不上自动降级到 `wss://broker.emqx.io:8084/mqtt`
- 在家可改成 `ws://mqtt.local:8083/mqtt`（NAS 直连，更快）
- broker / 前缀 / 账号都在「监控」页的「高级」里改，存手机本地
