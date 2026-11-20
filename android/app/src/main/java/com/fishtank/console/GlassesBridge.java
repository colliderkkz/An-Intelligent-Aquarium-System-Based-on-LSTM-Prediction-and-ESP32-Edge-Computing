package com.fishtank.console;

import android.Manifest;
import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.webkit.JavascriptInterface;
import android.webkit.WebView;

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

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.util.Arrays;

import kotlin.Unit;
import kotlin.jvm.functions.Function1;

/**
 * 眼镜桥：把 Rokid CXR-L（CUSTOM_VIEW）接到手机页面上。
 *
 * 页面侧只认 window.AndroidGlasses 的六个方法：
 *   env()          -> String  JSON：Rokid AI App 是否安装/版本是否兼容/蓝牙是否已连
 *   authorize()    -> void    拉起 Rokid AI App 授权，结果用 __arEvent 回传
 *   connect()      -> void    建会话并显示卡片，回调全部用 __arEvent 回传
 *   pushCard(json) -> void    customViewUpdate(增量更新数组)
 *   close()        -> void    关闭会话
 *   status()       -> String  JSON：当前状态（页面每秒轮询）
 *
 * 三条硬规则：
 *  1) 事件统一走 webView.evaluateJavascript("window.__arEvent({...})")，页面在
 *     任何时刻重载都不会因为桥抛出异常而崩。
 *  2) 任何异常都吞掉并转成 {type:'error'} 事件 —— 眼镜是外设，坏了不能连累鱼缸。
 *  3) SDK 类只在真正调用时才解析（懒初始化 manager），SDK 缺失/设备没装
 *     Rokid AI App 时，页面只会看到错误提示，App 本身照常工作。
 *
 * 调用顺序（与 tools/glasses_test 里跑通过的测试件一致）：
 *   检查环境 -> 授权 -> create(CUSTOM_VIEW) -> addLifecycleCallback -> connect(token)
 *   -> onSessionStarted 后才可以 customViewUpdate。
 */
public class GlassesBridge {

    private static final String TAG = "GlassesBridge";
    private static final int REQ_BT = 41;
    private static final long TERMINATING_GRACE_MS = 5000L;
    private static final String EMPTY_ICONS = "[]";

    private final Activity act;
    private final WebView web;
    private final Handler ui = new Handler(Looper.getMainLooper());

    /* ---- CXR 状态（全部懒初始化，构造时不碰 SDK 类） ---- */
    private CxrSessionManager manager;
    private CxrSession session;
    private String token;

    /* ---- 给页面看的状态 ---- */
    private String viewData = "{}";
    private String stateText = "Idle";
    private boolean authorized = false;
    private String lastError = "";
    private String lastPushAt = "";
    private int pushOk = 0;
    private int pushFail = 0;
    private int seq = 0;

    public GlassesBridge(Activity act, WebView web) {
        this.act = act;
        this.web = web;
        this.viewData = readAsset("viewdata.json");
    }

    /* ==================== 页面可调用的六个方法 ==================== */

    /** 环境检查：Rokid AI App 是否安装、版本是否兼容、眼镜蓝牙是否已连。 */
    @JavascriptInterface
    public String env() {
        JSONObject o = new JSONObject();
        try {
            o.put("ok", true);
            o.put("sdk", true);
            CxrSessionManager m = manager();
            boolean installed = m.isRokidAppInstalled(act);
            o.put("installed", installed);
            if (installed) {
                RokidAppStatus st = m.checkRokidAppCompatibility(act);
                if (st instanceof RokidAppStatus.Compatible) {
                    o.put("compat", "compatible");
                    o.put("version", ((RokidAppStatus.Compatible) st).getVersion());
                } else if (st instanceof RokidAppStatus.VersionTooLow) {
                    RokidAppStatus.VersionTooLow low = (RokidAppStatus.VersionTooLow) st;
                    o.put("compat", "low");
                    o.put("version", low.getInstalledVersion());
                    o.put("minVersion", low.getMinimumVersion());
                    o.put("url", low.getDownloadUrl());
                } else if (st instanceof RokidAppStatus.NotInstalled) {
                    RokidAppStatus.NotInstalled ni = (RokidAppStatus.NotInstalled) st;
                    o.put("compat", "missing");
                    o.put("minVersion", ni.getMinimumVersion());
                    o.put("url", ni.getDownloadUrl());
                } else {
                    o.put("compat", "unknown");
                    o.put("raw", String.valueOf(st));
                }
            } else {
                o.put("compat", "missing");
            }
            try {
                o.put("bt", m.isGlassesBtConnected());
            } catch (Throwable t2) {
                o.put("bt", false);
                o.put("btErr", String.valueOf(t2));
            }
        } catch (Throwable t) {
            o = failJson("env", t);
        }
        return o.toString();
    }

    /** 发起授权（会跳到 Rokid AI App）。 */
    @JavascriptInterface
    public void authorize() {
        ui.post(new Runnable() {
            @Override
            public void run() {
                try {
                    CxrSessionManager m = manager();
                    emitLog("正在拉起 Rokid AI App 授权…");
                    m.requestAuthorization(act, Arrays.asList(GlassPermission.MEDIA),
                            new Function1<AuthResult, Unit>() {
                                @Override
                                public Unit invoke(AuthResult result) {
                                    onAuthResult(result);
                                    return Unit.INSTANCE;
                                }
                            });
                } catch (Throwable t) {
                    emitError("requestAuthorization", t);
                }
            }
        });
    }

    /** 建会话并让眼镜显示鱼缸卡片（viewData 由 SessionConfig 带下去）。 */
    @JavascriptInterface
    public void connect() {
        ui.post(new Runnable() {
            @Override
            public void run() {
                try {
                    if (token == null || token.length() == 0) {
                        lastError = "NOT_AUTHORIZED";
                        emitError("connect", new IllegalStateException("还没有授权，先点「授权」"));
                        return;
                    }
                    if (needBtPermission()) {
                        askBtPermission();
                        emitLog("已申请蓝牙权限，允许后再点一次「连接并显示」");
                        return;
                    }
                    if (session != null) {
                        emitError("connect", new IllegalStateException(
                                "已有会话（" + stateOf(session) + "），先点「关闭会话」"));
                        return;
                    }
                    CxrSessionManager m = manager();
                    SessionConfig cfg = new SessionConfig(
                            SessionType.CUSTOM_VIEW,
                            null,                       /* glassesPackageName：CUSTOM_VIEW 不需要 */
                            AiInterceptMode.ALLOW_WITH_PAUSE,
                            TERMINATING_GRACE_MS,
                            new SessionTimeouts(),
                            viewData,
                            EMPTY_ICONS,                /* 本轮不用 ImageView */
                            null,
                            null);
                    session = m.create(cfg);
                    session.addLifecycleCallback(life);
                    stateText = "Starting";
                    emitState("已建会话，正在连眼镜…");
                    session.connect(token);
                } catch (Throwable t) {
                    session = null;
                    emitError("connect", t);
                }
            }
        });
    }

    /**
     * 增量更新：json 必须是 [{"action":"update","id":"line_temp","props":{"text":"..."}}]。
     * 只在 Started 状态下发；失败只回事件，不抛。
     */
    @JavascriptInterface
    public void pushCard(final String json) {
        final String payload = (json == null || json.trim().length() < 2) ? "[]" : json.trim();
        ui.post(new Runnable() {
            @Override
            public void run() {
                try {
                    JSONArray arr = new JSONArray(payload);
                    if (arr.length() == 0) {
                        emitPush(false, "EMPTY", "没有变化，跳过", 0);
                        return;
                    }
                } catch (Throwable t) {
                    emitError("pushCard(不是合法 JSON 数组)", t);
                    return;
                }
                try {
                    CxrSession s = session;
                    if (s == null) {
                        emitPush(false, "NO_SESSION", "还没有会话", payload.length());
                        return;
                    }
                    SessionState st = s.getState();
                    if (st != SessionState.Started) {
                        emitPush(false, "SESSION_NOT_STARTED", "当前状态 " + st + "（要等 Started 才能更新）",
                                payload.length());
                        return;
                    }
                    SessionResult<Unit> r = s.customViewUpdate(payload);
                    boolean ok = r != null && r.isSuccess();
                    String code = describeCode(r == null ? null : r.getCode());
                    String msg = r == null ? "null result" : (r.getMessage() == null ? "" : r.getMessage());
                    if (ok) {
                        pushOk++;
                        seq++;
                        lastPushAt = clock();
                    } else {
                        pushFail++;
                    }
                    emitPush(ok, code, msg, payload.length());
                } catch (Throwable t) {
                    pushFail++;
                    emitPush(false, "EXCEPTION", String.valueOf(t), payload.length());
                    emitError("customViewUpdate", t);
                }
            }
        });
    }

    /** 关闭会话（再次显示要重新 create）。 */
    @JavascriptInterface
    public void close() {
        ui.post(new Runnable() {
            @Override
            public void run() {
                closeQuietly("用户关闭");
            }
        });
    }

    /** 页面每秒轮询的状态；永远返回合法 JSON。 */
    @JavascriptInterface
    public String status() {
        JSONObject o = new JSONObject();
        try {
            o.put("ok", true);
            o.put("sdk", true);
            o.put("state", stateText);
            o.put("auth", authorized);
            o.put("err", lastError == null ? "" : lastError);
            o.put("pushOk", pushOk);
            o.put("pushFail", pushFail);
            o.put("lastPush", lastPushAt == null ? "" : lastPushAt);
            o.put("seq", seq);
            o.put("hasViewData", viewData != null && viewData.length() > 2);
            CxrSession s = session;
            if (s != null) {
                SessionState st = s.getState();
                o.put("real", String.valueOf(st));
                o.put("conn", st == SessionState.Started);
            } else {
                o.put("real", "");
                o.put("conn", false);
            }
            o.put("hasToken", token != null && token.length() > 0);
        } catch (Throwable t) {
            o = failJson("status", t);
        }
        return o.toString();
    }

    /* ==================== 给 MainActivity 用的入口 ==================== */

    /** 转发 onActivityResult（授权结果可能从 Activity 回调里回来）。 */
    public void onActivityResult(int requestCode, int resultCode, Intent data) {
        try {
            if (data == null && resultCode == 0 && requestCode == 0) {
                return;
            }
            CxrSessionManager m = manager();
            final AuthResult r = m.parseAuthorizationResult(resultCode, data);
            if (r != null) {
                onAuthResult(r);
            }
        } catch (Throwable t) {
            emitError("parseAuthorizationResult", t);
        }
    }

    /** Activity 销毁：尽量安静地收掉会话。 */
    public void release() {
        try {
            if (session != null) {
                ui.post(new Runnable() {
                    @Override
                    public void run() {
                        closeQuietly("页面退出");
                    }
                });
            }
        } catch (Throwable t) {
            Log.w(TAG, "release: " + t);
        }
    }

    /* ==================== 生命周期回调 ==================== */

    private final ISessionLifecycleCbk life = new ISessionLifecycleCbk() {
        @Override
        public void onSessionStarted() {
            stateText = "Started";
            emitState("会话已启动，卡片已下发");
        }

        @Override
        public void onSessionPaused(PausedReason reason) {
            stateText = "Paused";
            emitState("会话暂停：" + reason
                    + (reason == PausedReason.AI_ASSIST ? "（AI 占用，结束后自动恢复）"
                    : "（蓝牙断开，回 Rokid AI App 重连）"));
        }

        @Override
        public void onSessionResumed() {
            stateText = "Started";
            emitState("会话已恢复，继续推送");
        }

        @Override
        public void onSessionTerminating(TerminatingReason reason, long gracePeriodMs) {
            stateText = "Terminating";
            emitState("会话正在结束：" + reason + "（宽限 " + gracePeriodMs + "ms）");
        }

        @Override
        public void onSessionClosed(CloseReason reason) {
            stateText = "Closed";
            session = null;
            emitState("会话已关闭：" + reason);
        }

        @Override
        public void onConnectResult(boolean success, SessionErrorCode errorCode) {
            if (!success) {
                lastError = describeCode(errorCode);
                stateText = "Closed";
            }
            JSONObject o = new JSONObject();
            try {
                o.put("type", "connect");
                o.put("ok", success);
                o.put("code", describeCode(errorCode));
                o.put("msg", success ? "连接成功" : ("连接失败：" + describeCode(errorCode)));
                o.put("state", stateText);
            } catch (Throwable ignored) {
            }
            emitJson(o);
        }
    };

    /* ==================== 内部实现 ==================== */

    /** SDK 懒初始化：只有真正用到时才解析 CXR 类，缺 SDK 也不会拖垮 App。 */
    private CxrSessionManager manager() {
        if (manager == null) {
            manager = CxrSessionManager.Companion.getInstance(act.getApplicationContext());
        }
        return manager;
    }

    private void onAuthResult(final AuthResult r) {
        ui.post(new Runnable() {
            @Override
            public void run() {
                try {
                    JSONObject o = new JSONObject();
                    o.put("type", "auth");
                    o.put("state", stateText);
                    if (r == null) {
                        o.put("ok", false);
                        o.put("code", "-");
                        o.put("msg", "授权回调为空");
                        o.put("token", 0);
                        emitJson(o);
                        return;
                    }
                    boolean ok = r.isSuccess();
                    if (ok) {
                        token = r.getToken();
                        authorized = token != null && token.length() > 0;
                    }
                    String msg = r.getMessage() == null ? "" : r.getMessage();
                    if (!ok) {
                        lastError = describeCode(r.getErrorCode());
                    }
                    o.put("ok", ok);
                    o.put("token", token == null ? 0 : token.length());
                    o.put("code", describeCode(r.getErrorCode()));
                    o.put("msg", msg);
                    emitJson(o);
                } catch (Throwable t) {
                    emitError("onAuthResult", t);
                }
            }
        });
    }

    private void closeQuietly(String why) {
        try {
            CxrSession s = session;
            if (s == null) {
                stateText = "Closed";
                emitState("当前没有会话（" + why + "）");
                return;
            }
            try {
                s.removeLifecycleCallback(life);
            } catch (Throwable ignored) {
            }
            s.close();
            session = null;
            stateText = "Closed";
            emitState("会话已关闭（" + why + "）");
        } catch (Throwable t) {
            session = null;
            stateText = "Closed";
            emitError("close", t);
        }
    }

    private boolean needBtPermission() {
        if (Build.VERSION.SDK_INT < 31) {
            return false;
        }
        try {
            return act.checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT)
                    != PackageManager.PERMISSION_GRANTED;
        } catch (Throwable t) {
            return false;
        }
    }

    private void askBtPermission() {
        try {
            act.requestPermissions(new String[]{
                    Manifest.permission.BLUETOOTH_CONNECT,
                    Manifest.permission.BLUETOOTH_SCAN
            }, REQ_BT);
        } catch (Throwable t) {
            emitError("requestPermissions", t);
        }
    }

    private String readAsset(String name) {
        InputStream in = null;
        try {
            in = act.getAssets().open(name);
            ByteArrayOutputStream bo = new ByteArrayOutputStream();
            byte[] buf = new byte[8192];
            int n;
            while ((n = in.read(buf)) > 0) {
                bo.write(buf, 0, n);
            }
            String s = new String(bo.toByteArray(), "UTF-8");
            return s.length() > 2 ? s : "{}";
        } catch (Throwable t) {
            Log.w(TAG, "读取 assets/" + name + " 失败: " + t);
            return "{}";
        } finally {
            try {
                if (in != null) {
                    in.close();
                }
            } catch (Throwable ignored) {
            }
        }
    }

    /* ---- 事件回传（全部吞异常） ---- */

    private void emitJson(JSONObject o) {
        final String js = "(function(){try{if(window.__arEvent){window.__arEvent("
                + o.toString() + ");}}catch(e){}})();";
        ui.post(new Runnable() {
            @Override
            public void run() {
                try {
                    if (web != null) {
                        web.evaluateJavascript(js, null);
                    }
                } catch (Throwable t) {
                    Log.w(TAG, "evaluateJavascript 失败: " + t);
                }
            }
        });
    }

    private void emitState(String msg) {
        JSONObject o = new JSONObject();
        try {
            o.put("type", "state");
            o.put("state", stateText);
            o.put("msg", msg);
            o.put("ok", true);
        } catch (Throwable ignored) {
        }
        emitJson(o);
    }

    private void emitLog(String msg) {
        JSONObject o = new JSONObject();
        try {
            o.put("type", "log");
            o.put("msg", msg);
            o.put("state", stateText);
        } catch (Throwable ignored) {
        }
        emitJson(o);
    }

    private void emitPush(boolean ok, String code, String msg, int size) {
        JSONObject o = new JSONObject();
        try {
            o.put("type", "push");
            o.put("ok", ok);
            o.put("code", code == null ? "" : code);
            o.put("msg", msg == null ? "" : msg);
            o.put("size", size);
            o.put("seq", seq);
            o.put("state", stateText);
        } catch (Throwable ignored) {
        }
        emitJson(o);
    }

    private void emitError(String where, Throwable t) {
        lastError = where + ": " + (t == null ? "未知错误" : t.toString());
        Log.w(TAG, lastError);
        JSONObject o = new JSONObject();
        try {
            o.put("type", "error");
            o.put("msg", lastError);
            o.put("state", stateText);
            o.put("ok", false);
        } catch (Throwable ignored) {
        }
        emitJson(o);
    }

    private JSONObject failJson(String where, Throwable t) {
        lastError = where + ": " + (t == null ? "未知错误" : t.toString());
        Log.w(TAG, lastError);
        JSONObject o = new JSONObject();
        try {
            o.put("ok", false);
            o.put("sdk", false);
            o.put("state", stateText);
            o.put("err", lastError);
        } catch (Throwable ignored) {
        }
        return o;
    }

    private static String stateOf(CxrSession s) {
        try {
            return String.valueOf(s.getState());
        } catch (Throwable t) {
            return "?";
        }
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

    private static String clock() {
        try {
            return new java.text.SimpleDateFormat("HH:mm:ss", java.util.Locale.US)
                    .format(new java.util.Date());
        } catch (Throwable t) {
            return "";
        }
    }
}
