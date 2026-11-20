package com.fishtank.glassestest;

import android.Manifest;
import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import com.rokid.cxr.session.AiInterceptMode;
import com.rokid.cxr.session.AuthResult;
import com.rokid.cxr.session.CloseReason;
import com.rokid.cxr.session.CxrSession;
import com.rokid.cxr.session.CxrSessionManager;
import com.rokid.cxr.session.GlassPermission;
import com.rokid.cxr.session.ISessionLifecycleCbk;
import com.rokid.cxr.session.PausedReason;
import com.rokid.cxr.session.RokidAppStatus;
import com.rokid.cxr.session.SessionConfig;
import com.rokid.cxr.session.SessionErrorCode;
import com.rokid.cxr.session.SessionResult;
import com.rokid.cxr.session.SessionState;
import com.rokid.cxr.session.SessionTimeouts;
import com.rokid.cxr.session.SessionType;
import com.rokid.cxr.session.TerminatingReason;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.Arrays;
import java.util.Date;
import java.util.Locale;

import kotlin.Unit;
import kotlin.jvm.functions.Function1;

/**
 * 眼镜显示最小测试件（CUSTOM_VIEW）
 *
 * 目的：在不用眼镜端 APK 的前提下，验证「授权 → 建会话 → 眼镜渲染 → 增量更新」这条链路。
 * 数据为内置样例（鱼缸温度/pH/TDS/浊度/水位/CO2 + 执行器），本轮不接真实 MQTT。
 *
 * 依据 Rokid CXR 文档：
 *  - CUSTOM_VIEW 只支持 LinearLayout / RelativeLayout / TextView / ImageView；
 *  - 能力 API 必须在 onSessionStarted() 之后调用；
 *  - 会话只能 connect 一次，close() 后要重新 create；
 *  - AI 唤醒 / 蓝牙断开会让会话进入 Paused，恢复后可继续推送。
 *
 * 操作顺序：1 检查环境 → 2 授权 → 3 连接并显示 → 4 手动刷新 → 5 关闭。
 */
public class MainActivity extends Activity {

    private static final String TAG = "GlassesTest";
    private static final int REQ_BT = 41;
    private static final long TICK_MS = 2000L;

    private CxrSessionManager manager;
    private CxrSession session;
    private String token;
    private String viewData = "{}";

    private TextView statusView;
    private TextView logView;
    private ScrollView logScroll;

    private final Handler handler = new Handler(Looper.getMainLooper());
    private int beat = 0;
    private boolean ticking = false;

    private final Runnable ticker = new Runnable() {
        @Override
        public void run() {
            if (!ticking) {
                return;
            }
            beat++;
            CxrSession s = session;
            if (s != null && s.getState() == SessionState.Started) {
                pushUpdates(s, beat, false);
            }
            handler.postDelayed(this, TICK_MS);
        }
    };

    private final ISessionLifecycleCbk lifecycle = new ISessionLifecycleCbk() {
        @Override
        public void onSessionStarted() {
            log("[ok] onSessionStarted：会话 Started，能力 API 可用");
            setStatus("状态：Started（眼镜应已显示卡片）");
            pushUpdates(session, 1, true);
            startTicker();
        }

        @Override
        public void onSessionPaused(PausedReason reason) {
            log("[!] onSessionPaused: " + reason
                    + (reason == PausedReason.AI_ASSIST ? "（AI 占用，结束后自动恢复）"
                    : "（蓝牙断开，回 Rokid AI App 重连）"));
            setStatus("状态：Paused（" + reason + "）");
        }

        @Override
        public void onSessionResumed() {
            log("[ok] onSessionResumed：继续推送");
            setStatus("状态：Started（已恢复）");
        }

        @Override
        public void onSessionTerminating(TerminatingReason reason, long gracePeriodMs) {
            log("[!] onSessionTerminating: " + reason + "，宽限期 " + gracePeriodMs + "ms");
            setStatus("状态：Terminating");
        }

        @Override
        public void onSessionClosed(CloseReason reason) {
            log("[!] onSessionClosed: " + reason);
            setStatus("状态：已关闭（" + reason + "）");
            stopTicker();
        }

        @Override
        public void onConnectResult(boolean success, SessionErrorCode errorCode) {
            log("onConnectResult: success=" + success + "，error=" + describeCode(errorCode));
            if (!success) {
                setStatus("连接失败：" + describeCode(errorCode));
            }
        }
    };

    // ------------------------------------------------------------------ 生命周期

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        buildUi();
        requestBtPermissions();

        try {
            manager = CxrSessionManager.Companion.getInstance(getApplicationContext());
            log("CXR-L 初始化完成（CxrSessionManager）");
        } catch (Throwable t) {
            log("[x] CxrSessionManager 初始化失败: " + t);
        }

        viewData = loadAsset("viewdata.json");
        log("viewData 已载入：" + viewData.length() + " 字符");

        log("提示：先在 Rokid AI App 里连好眼镜（不是系统蓝牙配对），再点 1。");
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (manager == null) {
            return;
        }
        try {
            final AuthResult result = manager.parseAuthorizationResult(resultCode, data);
            handler.post(new Runnable() {
                @Override
                public void run() {
                    onAuthResult(result);
                }
            });
        } catch (Throwable t) {
            log("[x] parseAuthorizationResult 抛异常: " + t);
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode != REQ_BT) {
            return;
        }
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < permissions.length && i < grantResults.length; i++) {
            sb.append(permissions[i]).append('=')
              .append(grantResults[i] == PackageManager.PERMISSION_GRANTED ? "granted" : "denied")
              .append(' ');
        }
        log("蓝牙权限: " + sb);
    }

    @Override
    protected void onDestroy() {
        stopTicker();
        if (session != null) {
            try {
                session.removeLifecycleCallback(lifecycle);
                session.close();
            } catch (Throwable t) {
                Log.w(TAG, "close failed: " + t);
            }
            session = null;
        }
        super.onDestroy();
    }

    // ------------------------------------------------------------------ 四个动作

    private void envCheck() {
        if (manager == null) {
            log("[x] SDK 未初始化");
            return;
        }
        log("— 环境检查 —");
        boolean installed = manager.isRokidAppInstalled(this);
        log("Rokid AI App 已安装: " + installed);
        if (!installed) {
            log("[!] 请先安装 Rokid AI App（大陆）/ Hi Rokid（海外）");
            setStatus("环境：未安装 Rokid AI App");
            return;
        }
        RokidAppStatus status = manager.checkRokidAppCompatibility(this);
        if (status instanceof RokidAppStatus.Compatible) {
            log("[ok] 版本兼容: " + ((RokidAppStatus.Compatible) status).getVersion());
            setStatus("环境：App 兼容，可以点 2 授权");
        } else if (status instanceof RokidAppStatus.VersionTooLow) {
            RokidAppStatus.VersionTooLow low = (RokidAppStatus.VersionTooLow) status;
            log("[!] 版本过低：已装 " + low.getInstalledVersion()
                    + "，最低 " + low.getMinimumVersion());
            log("    下载: " + low.getDownloadUrl());
            setStatus("环境：Rokid AI App 版本过低");
        } else if (status instanceof RokidAppStatus.NotInstalled) {
            RokidAppStatus.NotInstalled ni = (RokidAppStatus.NotInstalled) status;
            log("[!] 未安装：最低 " + ni.getMinimumVersion() + "，下载 " + ni.getDownloadUrl());
            setStatus("环境：未安装 Rokid AI App");
        } else {
            log("[!] 兼容性返回未知分支: " + status);
        }
        log("提示：眼镜是否已连，以 Rokid AI App 内显示为准（SDK 的蓝牙检查在建会话后才有效）");
    }

    private void doAuth() {
        if (manager == null) {
            log("[x] SDK 未初始化");
            return;
        }
        log("— 发起授权（会跳转 Rokid AI App）—");
        try {
            manager.requestAuthorization(this, Arrays.asList(GlassPermission.MEDIA),
                    new Function1<AuthResult, Unit>() {
                        @Override
                        public Unit invoke(final AuthResult result) {
                            handler.post(new Runnable() {
                                @Override
                                public void run() {
                                    onAuthResult(result);
                                }
                            });
                            return Unit.INSTANCE;
                        }
                    });
        } catch (Throwable t) {
            log("[x] requestAuthorization 抛异常: " + t);
        }
    }

    private void onAuthResult(AuthResult result) {
        if (result == null) {
            log("[x] 授权回调为空");
            return;
        }
        if (result.isSuccess()) {
            token = result.getToken();
            log("[ok] 授权成功，token " + (token == null ? "为空(异常)" : "已获取（" + token.length() + " 字符）"));
            setStatus("已授权：点 3 连接并显示");
        } else {
            log("[x] 授权失败：" + describeCode(result.getErrorCode()) + " " + result.getMessage());
            setStatus("授权失败：" + describeCode(result.getErrorCode()));
        }
    }

    private void doConnect() {
        if (manager == null) {
            log("[x] SDK 未初始化");
            return;
        }
        if (token == null) {
            log("[!] 还没有 token，先点 2 授权");
            return;
        }
        if (session != null) {
            log("[!] 已有会话（" + session.getState() + "），先点 5 关闭再重连");
            return;
        }
        SessionConfig config = new SessionConfig(
                SessionType.CUSTOM_VIEW,
                null,                       // glassesPackageName：CUSTOM_VIEW 不需要
                AiInterceptMode.ALLOW_WITH_PAUSE,
                5000L,
                new SessionTimeouts(),
                viewData,
                "[]",                       // 本轮不用 ImageView，图标给空数组
                null,
                null);
        try {
            session = manager.create(config);
            log("[ok] create(CUSTOM_VIEW)，state=" + session.getState());
        } catch (Throwable t) {
            log("[x] create 失败: " + t);
            session = null;
            return;
        }
        session.addLifecycleCallback(lifecycle);
        log("回调已注册，开始 connect(token) …");
        setStatus("状态：Starting（建链中）");
        try {
            session.connect(token);
        } catch (Throwable t) {
            log("[x] connect 抛异常: " + t);
        }
    }

    private void manualUpdate() {
        if (session == null) {
            log("[!] 还没有会话，先点 3");
            return;
        }
        SessionState state = session.getState();
        log("当前 state=" + state);
        if (state != SessionState.Started) {
            log("[!] 只有 Started 才能 customViewUpdate（Started 前调用会拿到 SESSION_NOT_STARTED）");
            return;
        }
        beat++;
        pushUpdates(session, beat, true);
    }

    private void doClose() {
        stopTicker();
        if (session == null) {
            log("[!] 当前没有会话");
            return;
        }
        try {
            session.removeLifecycleCallback(lifecycle);
            session.close();
            log("[ok] close() 已调用（会话实例不可再用，重连要重新 create）");
        } catch (Throwable t) {
            log("[x] close 抛异常: " + t);
        }
        session = null;
        setStatus("状态：已关闭");
    }

    // ------------------------------------------------------------------ 数据推送

    private void pushUpdates(CxrSession s, int seq, boolean manual) {
        if (s == null) {
            return;
        }
        String json = buildUpdates(seq);
        try {
            SessionResult<Unit> result = s.customViewUpdate(json);
            if (result != null && result.isSuccess()) {
                log((manual ? "[手动] " : "[定时] ") + "更新 #" + seq + " 成功（" + json.length() + " 字符）");
            } else {
                log("[x] 更新 #" + seq + " 失败: " + describeResult(result));
            }
        } catch (Throwable t) {
            log("[x] customViewUpdate 抛异常: " + t);
        }
    }

    /** 样例数据：数值每 2 秒微调，用来肉眼确认增量更新真的到了眼镜上 */
    private String buildUpdates(int seq) {
        double temp = 26.0 + (seq % 10) * 0.1;
        double ph = 7.10 + (seq % 5) * 0.05;
        int tds = 315 + (seq % 7);
        double turb = 3.0 + (seq % 4) * 0.2;
        int level = 62 - (seq % 3);
        int co2 = 610 + (seq % 6) * 5;
        String clock = new SimpleDateFormat("HH:mm:ss", Locale.US).format(new Date());

        StringBuilder sb = new StringBuilder(1024);
        sb.append('[');
        appendUpdate(sb, "line_time", "同步 " + clock);
        appendUpdate(sb, "line_temp", "温度 " + fmt(temp, 1) + " ℃   目标 26.0");
        appendUpdate(sb, "line_ph", "PH " + fmt(ph, 2) + "    浊度 " + fmt(turb, 1) + " NTU");
        appendUpdate(sb, "line_tds", "TDS " + tds + " ppm    水位 " + level + " %");
        appendUpdate(sb, "line_co2", "CO2 " + co2 + " ppm    光照 240");
        appendUpdate(sb, "line_act", "灯 55%   泵 100%   气 60%");
        appendUpdate(sb, "line_mode", "模式 自动    保持 0 s");
        appendUpdate(sb, "line_beat", "链路 正常    刷新 " + seq);
        sb.append(']');
        return sb.toString();
    }

    private void appendUpdate(StringBuilder sb, String id, String text) {
        if (sb.charAt(sb.length() - 1) != '[') {
            sb.append(',');
        }
        sb.append("{\"action\":\"update\",\"id\":\"")
          .append(id)
          .append("\",\"props\":{\"text\":\"")
          .append(escape(text))
          .append("\"}}");
    }

    private static String fmt(double v, int digits) {
        return String.format(Locale.US, "%." + digits + "f", v);
    }

    private static String escape(String s) {
        StringBuilder sb = new StringBuilder(s.length() + 8);
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            switch (c) {
                case '"': sb.append("\\\""); break;
                case '\\': sb.append("\\\\"); break;
                case '\n': sb.append("\\n"); break;
                case '\r': sb.append("\\r"); break;
                case '\t': sb.append("\\t"); break;
                default: sb.append(c);
            }
        }
        return sb.toString();
    }

    private void startTicker() {
        if (ticking) {
            return;
        }
        ticking = true;
        handler.postDelayed(ticker, TICK_MS);
        log("定时更新已启动（每 " + TICK_MS + "ms 一次）");
    }

    private void stopTicker() {
        if (!ticking) {
            return;
        }
        ticking = false;
        handler.removeCallbacks(ticker);
        log("定时更新已停止");
    }

    // ------------------------------------------------------------------ UI 与工具

    private void buildUi() {
        ScrollView root = new ScrollView(this);
        root.setBackgroundColor(Color.parseColor("#0B0F14"));

        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        int pad = dp(16);
        box.setPadding(pad, pad, pad, pad);
        root.addView(box, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT));

        TextView title = new TextView(this);
        title.setText("眼镜显示测试 · CUSTOM_VIEW");
        title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 20);
        title.setTextColor(Color.parseColor("#4CC2FF"));
        box.addView(title);

        TextView hint = new TextView(this);
        hint.setText("先确认 Rokid AI App 里眼镜已连接，再按 1 → 2 → 3 顺序点；屏幕下方是日志。");
        hint.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        hint.setTextColor(Color.parseColor("#9AA4B2"));
        box.addView(hint);

        statusView = new TextView(this);
        statusView.setText("状态：Idle");
        statusView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        statusView.setTextColor(Color.parseColor("#E6E6E6"));
        LinearLayout.LayoutParams slp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        slp.topMargin = dp(8);
        box.addView(statusView, slp);

        addButton(box, "1 检查环境（Rokid AI App / 版本）", new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                envCheck();
            }
        });
        addButton(box, "2 授权（拉起 Rokid AI App）", new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                doAuth();
            }
        });
        addButton(box, "3 连接并显示鱼缸卡片", new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                doConnect();
            }
        });
        addButton(box, "4 手动刷新一次", new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                manualUpdate();
            }
        });
        addButton(box, "5 关闭会话", new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                doClose();
            }
        });

        logScroll = new ScrollView(this);
        logView = new TextView(this);
        logView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        logView.setTextColor(Color.parseColor("#B9F6CA"));
        logView.setGravity(Gravity.START);
        logView.setTextIsSelectable(true);
        logScroll.addView(logView);
        LinearLayout.LayoutParams llp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f);
        llp.topMargin = dp(10);
        box.addView(logScroll, llp);

        setContentView(root);
    }

    private void addButton(LinearLayout box, String text, View.OnClickListener listener) {
        Button b = new Button(this);
        b.setText(text);
        b.setAllCaps(false);
        b.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        b.setOnClickListener(listener);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(6);
        box.addView(b, lp);
    }

    private void log(String line) {
        Log.i(TAG, line);
        if (logView == null) {
            return;
        }
        String clock = new SimpleDateFormat("HH:mm:ss", Locale.US).format(new Date());
        logView.append(clock + "  " + line + "\n");
        if (logScroll != null) {
            logScroll.post(new Runnable() {
                @Override
                public void run() {
                    logScroll.fullScroll(View.FOCUS_DOWN);
                }
            });
        }
    }

    private void setStatus(String text) {
        if (statusView != null) {
            statusView.setText(text);
        }
    }

    private String loadAsset(String name) {
        try {
            InputStream in = getAssets().open(name);
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buf = new byte[8192];
            int n;
            while ((n = in.read(buf)) > 0) {
                out.write(buf, 0, n);
            }
            in.close();
            return new String(out.toByteArray(), StandardCharsets.UTF_8);
        } catch (Throwable t) {
            log("[x] 读取 assets/" + name + " 失败: " + t);
            return "{}";
        }
    }

    private void requestBtPermissions() {
        if (Build.VERSION.SDK_INT < 31) {
            return;
        }
        if (checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED) {
            return;
        }
        requestPermissions(new String[]{
                Manifest.permission.BLUETOOTH_CONNECT,
                Manifest.permission.BLUETOOTH_SCAN
        }, REQ_BT);
    }

    private int dp(int v) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v,
                getResources().getDisplayMetrics());
    }

    private static String describeCode(SessionErrorCode code) {
        if (code == null) {
            return "-";
        }
        try {
            return code.name() + "(" + code.getCode() + ")";
        } catch (Throwable t) {
            return String.valueOf(code);
        }
    }

    private static String describeResult(SessionResult<Unit> result) {
        if (result == null) {
            return "null result";
        }
        return describeCode(result.getCode()) + " " + result.getMessage();
    }
}
