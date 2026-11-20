package com.fishtank.console;

import android.annotation.SuppressLint;
import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.os.Bundle;
import android.view.KeyEvent;
import android.webkit.JavascriptInterface;
import android.webkit.WebResourceRequest;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.Toast;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;

/**
 * 整个 App 就是一个 WebView。
 *
 * 页面来源优先级：
 *   1) App 私有目录里的热更新版本  files/www/index.html
 *   2) 内置的 assets/index.html
 *
 * 热更新流程：
 *   启动 -> 请求 NAS 上的 app/version.json -> 比本地版本号大就下载 app.html
 *        -> 校验内容里含有标记 -> 覆盖 files/www/index.html -> reload
 *
 * 没有网络（或不在家里局域网）时静默失败，继续用已有版本。
 */
public class MainActivity extends Activity {

    /* 热更新源：家里 NAS 的 nginx */
    /**
     * HTTP 更新源默认关闭。
     *
     * 原因：NAS 的 webconsole 上那份 /app/app.html 是某次手工部署留下的旧页面
     * （BOOT_VER=0、broker 还写着公网 emqx），APK 启动时无条件去拉它，
     * 就会把刚装的新版本页面覆盖成旧的 —— 用户看到的就是
     * 局域网搜不到设备、显示公网、卡在发现新界面下载中。
     * 页面自己已经能通过 MQTT 热更新（内网/外网都通），这条通道不需要了。
     */
    private static final boolean HTTP_UPDATE = false;
    private static final String BASE      = "http://mqtt.local:8090/app/";
    private static final String VER_URL   = BASE + "version.json";
    private static final String HTML_URL  = BASE + "app.html";

    /* 下载内容必须包含这个标记，防止下到错误页面把 App 弄坏 */
    private static final String MARKER    = "Ciallo";

    private static final String PREFS     = "updater";
    private static final String KEY_VER   = "www_ver";

    private WebView web;
    private PhoneLamp phoneLamp;
    private SharedPreferences prefs;

    /* 眼镜（Rokid CXR-L）桥：页面上的 AR 页签通过它调 SDK。
     * 单独一个类，且内部懒加载 SDK —— 没有眼镜/没装 Rokid AI App 时
     * 页面只会看到错误提示，监控/计划/高级等原有功能完全不受影响。 */
    private GlassesBridge glasses;

    @SuppressLint({"SetJavaScriptEnabled", "AddJavascriptInterface"})
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        prefs = getSharedPreferences(PREFS, MODE_PRIVATE);

        /* 允许通过 USB(adb) 远程调试页面：chrome://inspect 或 CDP。
         * 只在本地 adb 可用时才有意义，不联网、不上传任何数据；
         * 现场排查"某个按钮点了没反应"这类问题时必须靠它。 */
        WebView.setWebContentsDebuggingEnabled(true);

        web = new WebView(this);
        setContentView(web);

        WebSettings s = web.getSettings();
        s.setJavaScriptEnabled(true);
        s.setDomStorageEnabled(true);
        s.setAllowFileAccess(true);
        s.setAllowContentAccess(true);
        s.setDatabaseEnabled(true);
        s.setCacheMode(WebSettings.LOAD_DEFAULT);
        s.setMediaPlaybackRequiresUserGesture(false);
        s.setMixedContentMode(WebSettings.MIXED_CONTENT_ALWAYS_ALLOW);

        phoneLamp = new PhoneLamp(this, web);
        web.setWebViewClient(new WebViewClient() {
            @Override
            public boolean shouldOverrideUrlLoading(WebView view, WebResourceRequest request) {
                android.net.Uri uri = request.getUrl();
                if ("fishtank".equals(uri.getScheme()) && "lamp".equals(uri.getHost())) {
                    if (!request.isForMainFrame() || !request.hasGesture() ||
                            !"file:///android_asset/index.html".equals(view.getUrl())) return true;
                    String action = uri.getPath();
                    if ("/scan".equals(action) || "/nearby".equals(action)) phoneLamp.begin();
                    else if ("/connect".equals(action)) phoneLamp.connect(uri.getQueryParameter("mac"));
                    else if ("/disconnect".equals(action)) { phoneLamp.close(); phoneLamp.publish(); }
                    else {
                        try {
                            int value = Integer.parseInt(uri.getQueryParameter("v"));
                            if ("/level".equals(action)) phoneLamp.applyLevel(value);
                            else if ("/color".equals(action) && uri.getQueryParameter("c") != null)
                                phoneLamp.color(uri.getQueryParameter("c"), value);
                        } catch (NumberFormatException ignored) {}
                    }
                    return true;
                }
                return false;
            }
        });

        web.addJavascriptInterface(new Updater(), "AndroidUpdater");

        /* AR 眼镜桥。注册失败（例如 SDK 类缺失）也只影响 AR 页签，
         * 绝不能连累页面加载 —— 所以这里必须 catch 住。 */
        try {
            glasses = new GlassesBridge(this, web);
            web.addJavascriptInterface(glasses, "AndroidGlasses");
        } catch (Throwable t) {
            android.util.Log.w("MainActivity", "AndroidGlasses 桥注册失败: " + t);
            glasses = null;
        }

        loadPage();
        if (HTTP_UPDATE) {
            new Thread(new Runnable() {
                @Override
                public void run() {
                    checkUpdate(false);
                }
            }).start();
        }
    }

    /* ==================== 页面加载 ==================== */

    private File wwwDir() {
        return new File(getFilesDir(), "www");
    }

    private File localIndex() {
        return new File(wwwDir(), "index.html");
    }

    /**
     * 热更新文件是否"完整"。
     *
     * 为什么要查：页面自己写到一半 / 写入被系统打断时，files/www/index.html
     * 可能是个残缺文件。以前 loadPage() 只看长度就加载它，结果是**白屏 + 永远
     * 进不去**（页面脚本根本没被解析）。这里只认"末尾有 &lt;/html&gt;"的完整文件。
     */
    private boolean hotFileLooksComplete(File f) {
        java.io.RandomAccessFile raf = null;
        try {
            if (f == null || !f.exists() || f.length() < 2000) {
                return false;
            }
            raf = new java.io.RandomAccessFile(f, "r");
            long len = raf.length();
            int tail = (int) Math.min(64, len);
            byte[] buf = new byte[tail];
            raf.seek(len - tail);
            raf.readFully(buf);
            String s = new String(buf, "UTF-8");
            return s.contains("</html>");
        } catch (Exception e) {
            return false;
        } finally {
            try {
                if (raf != null) {
                    raf.close();
                }
            } catch (Exception ignored) {
            }
        }
    }

    /**
     * 读内置页面里的 BOOT_VER。
     *
     * 这一步是必须的：APK 升级后，手机私有目录里可能还留着上一版热更新的
     * files/www/index.html。原来 loadPage() 只要看到这个文件就优先用它，
     * 于是一个"旧页面"会把刚装的新 APK 的页面整份盖掉 ——
     * 表现就是：新 APK 里明明是局域网地址+新密码，手机却连到老的公网 broker、
     * 搜不到设备、还卡在"发现新界面下载中"。
     */
    private long bundledVer() {
        InputStream in = null;
        try {
            in = getAssets().open("index.html");
            ByteArrayOutputStream bo = new ByteArrayOutputStream();
            byte[] buf = new byte[16384];
            int n;
            while ((n = in.read(buf)) > 0) {
                bo.write(buf, 0, n);
            }
            String html = new String(bo.toByteArray(), "UTF-8");
            java.util.regex.Matcher m =
                    java.util.regex.Pattern.compile("var BOOT_VER = (\\d+);").matcher(html);
            if (m.find()) {
                return Long.parseLong(m.group(1));
            }
        } catch (Exception e) {
            /* 读不到就当作 0（退回老行为） */
        } finally {
            try {
                if (in != null) {
                    in.close();
                }
            } catch (Exception ignored) {
            }
        }
        return 0;
    }

    private void loadPage() {
        File f = localIndex();
        long bundled = bundledVer();
        long hot = prefs.getLong(KEY_VER, 0);

        /* 只有"热更新版本不低于内置版本"才用热更新那份；
         * 否则说明这个 APK 比残留的页面新，直接删掉残留、用内置页面。 */
        boolean useHot = f.exists() && f.length() > 2000 && (bundled <= 0 || hot >= bundled);
        if (useHot && !hotFileLooksComplete(f)) {
            /* 文件残缺（白屏的元凶）：删掉，回内置页面 */
            useHot = false;
        }
        if (useHot) {
            web.loadUrl("file://" + f.getAbsolutePath());
            return;
        }
        if (f.exists() && !f.delete()) {
            /* 删不掉也不能用旧的，至少别让它盖住新页面 */
            Toast.makeText(this, "清理旧页面失败", Toast.LENGTH_SHORT).show();
        }
        prefs.edit().putLong(KEY_VER, 0).apply();
        web.loadUrl("file:///android_asset/index.html");
    }

    /* ==================== 热更新 ==================== */

    private String httpGet(String urlStr) throws Exception {
        HttpURLConnection c = (HttpURLConnection) new URL(urlStr).openConnection();
        c.setConnectTimeout(4000);
        c.setReadTimeout(8000);
        c.setRequestProperty("Cache-Control", "no-cache");
        InputStream in = c.getInputStream();
        ByteArrayOutputStream bo = new ByteArrayOutputStream();
        byte[] buf = new byte[8192];
        int n;
        while ((n = in.read(buf)) > 0) {
            bo.write(buf, 0, n);
        }
        in.close();
        c.disconnect();
        return new String(bo.toByteArray(), "UTF-8");
    }

    /**
     * @param manual true = 用户在页面上点"检查更新"，需要给回执
     * @return 1 = 已更新，0 = 已是最新，-1 = 检查失败
     */
    private int checkUpdate(boolean manual) {
        try {
            String verStr = httpGet(VER_URL);
            long remote = 0;
            try {
                org.json.JSONObject o = new org.json.JSONObject(verStr);
                remote = o.optLong("v", 0);
            } catch (Exception e) {
                return -1;
            }
            long local = prefs.getLong(KEY_VER, 0);
            long bundled = bundledVer();
            /* 远程版本必须同时高于"已装的热更新"和"APK 内置"，否则一律不降级 */
            if (remote <= local || (bundled > 0 && remote <= bundled)) {
                if (manual) {
                    toastOnUi("已是最新版本");
                }
                return 0;
            }

            String html = httpGet(HTML_URL);
            if (html == null || html.length() < 5000 || !html.contains(MARKER)) {
                if (manual) {
                    toastOnUi("更新包校验失败");
                }
                return -1;
            }

            File dir = wwwDir();
            if (!dir.exists() && !dir.mkdirs()) {
                return -1;
            }
            File tmp = new File(dir, "index.tmp");
            FileOutputStream fo = new FileOutputStream(tmp);
            fo.write(html.getBytes("UTF-8"));
            fo.close();

            File dst = localIndex();
            if (dst.exists() && !dst.delete()) {
                return -1;
            }
            if (!tmp.renameTo(dst)) {
                return -1;
            }

            prefs.edit().putLong(KEY_VER, remote).apply();

            if (manual) {
                toastOnUi("已更新到最新界面，正在重载…");
            }
            runOnUiThread(new Runnable() {
                @Override
                public void run() {
                    loadPage();
                }
            });
            return 1;
        } catch (Exception e) {
            if (manual) {
                toastOnUi("检查更新失败（不在家里网络？）");
            }
            return -1;
        }
    }

    private void toastOnUi(final String msg) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                Toast.makeText(MainActivity.this, msg, Toast.LENGTH_SHORT).show();
            }
        });
    }

    /* ==================== JS 桥 ==================== */

    public class Updater {

        /** 当前使用的版本号（0 = 内置版） */
        @JavascriptInterface
        public String currentVersion() {
            return String.valueOf(prefs.getLong(KEY_VER, 0));
        }

        /** 当前页面来源：hot = 热更新版，builtin = 内置版 */
        @JavascriptInterface
        public String source() {
            File f = localIndex();
            return (f.exists() && f.length() > 2000) ? "hot" : "builtin";
        }

        /** 手动检查更新（后台线程，不卡界面） */
        @JavascriptInterface
        public void checkNow() {
            if (!HTTP_UPDATE) {
                toastOnUi("界面更新走页面内通道，稍等几秒会自动完成");
                return;
            }
            new Thread(new Runnable() {
                @Override
                public void run() {
                    checkUpdate(true);
                }
            }).start();
        }

        /**
         * 把页面自己下载好的热更新整页存成文件，下次启动直接以 file:// 加载。
         *
         * 这是替代旧方案（localStorage + document.write）的关键：
         * 400KB 的页面用 document.write 重写整个文档时，真机上出现过
         * "只画出开屏、脚本不执行、永远进不去"的问题；直接落盘再加载文件
         * 完全绕开这条脆弱路径，而且启动更快。
         *
         * @return true = 已落盘（调用方随后 reload 即可）
         */
        @JavascriptInterface
        public boolean savePage(final String html) {
            FileOutputStream fo = null;
            try {
                if (html == null || html.length() < 5000 || !html.contains("</html>")) {
                    return false;
                }
                File dir = wwwDir();
                if (!dir.exists() && !dir.mkdirs()) {
                    return false;
                }
                File tmp = new File(dir, "index.tmp");
                fo = new FileOutputStream(tmp);
                fo.write(html.getBytes("UTF-8"));
                fo.close();
                fo = null;

                File dst = localIndex();
                if (dst.exists() && !dst.delete()) {
                    return false;
                }
                if (!tmp.renameTo(dst)) {
                    return false;
                }

                long v = 0;
                java.util.regex.Matcher m =
                        java.util.regex.Pattern.compile("var BOOT_VER = (\\d+);").matcher(html);
                if (m.find()) {
                    v = Long.parseLong(m.group(1));
                }
                prefs.edit().putLong(KEY_VER, v).apply();
                return true;
            } catch (Exception e) {
                return false;
            } finally {
                try {
                    if (fo != null) {
                        fo.close();
                    }
                } catch (Exception ignored) {
                }
            }
        }

        /**
         * 重新走一遍页面选择逻辑（内置页 / 热更新文件），然后加载。
         *
         * 为什么不直接用 location.reload()：当前文档是
         * file:///android_asset/index.html，reload 只会把**内置页**再读一遍，
         * 刚存下来的热更新文件根本不会被打开 —— 结果就是"每次都提示要更新、
         * 更新完还是旧页面"的死循环。必须回到原生这里重新选一次 URL。
         */
        @JavascriptInterface
        public void reloadPage() {
            runOnUiThread(new Runnable() {
                @Override
                public void run() {
                    loadPage();
                }
            });
        }

        /** 清掉热更新，回到内置版 */
        @JavascriptInterface
        public void reset() {
            File f = localIndex();
            if (f.exists()) {
                f.delete();
            }
            prefs.edit().putLong(KEY_VER, 0).apply();
            runOnUiThread(new Runnable() {
                @Override
                public void run() {
                    loadPage();
                }
            });
        }
    }

    /* ==================== 生命周期 ==================== */

    /**
     * 眼镜授权是「跳到 Rokid AI App 再回来」的流程，结果从 onActivityResult 回来。
     * 桥自己内部还要兜一层 parseAuthorizationResult，所以这里只做转发 + 吞异常。
     */
    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (glasses == null) {
            return;
        }
        try {
            glasses.onActivityResult(requestCode, resultCode, data);
        } catch (Throwable t) {
            android.util.Log.w("MainActivity", "眼镜授权回调处理失败: " + t);
        }
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        if (keyCode == KeyEvent.KEYCODE_BACK && web != null && web.canGoBack()) {
            web.goBack();
            return true;
        }
        return super.onKeyDown(keyCode, event);
    }

    @Override public void onRequestPermissionsResult(int request, String[] permissions, int[] grants) {
        super.onRequestPermissionsResult(request, permissions, grants);
        if (request == PhoneLamp.PERMISSIONS) phoneLamp.onRequestPermissionsResult(request, permissions, grants);
    }

    @Override
    protected void onDestroy() {
        if (phoneLamp != null) phoneLamp.close();
        if (glasses != null) {
            try {
                glasses.release();
            } catch (Throwable t) {
                android.util.Log.w("MainActivity", "关闭眼镜会话失败: " + t);
            }
            glasses = null;
        }
        if (web != null) {
            web.destroy();
            web = null;
        }
        super.onDestroy();
    }
}
