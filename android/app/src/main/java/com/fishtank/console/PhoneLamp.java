package com.fishtank.console;

import android.Manifest;
import android.app.Activity;
import android.bluetooth.*;
import android.bluetooth.le.*;
import android.content.pm.PackageManager;
import android.os.*;
import android.webkit.WebView;
import org.json.JSONObject;
import java.util.LinkedHashMap;

/** BLE transport owned by the console. The original WebView controls render its state. */
final class PhoneLamp {
    static final int PERMISSIONS = 1241;
    private final Activity activity;
    private final WebView web;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final LinkedHashMap<String,String> labels = new LinkedHashMap<>();
    private final BluetoothAdapter bluetooth;
    private BluetoothLeScanner scanner;
    private ScanCallback scan;
    private BluetoothGatt link;
    private BluetoothGattCharacteristic writer;
    private final int[] spectrum = {100,100,100,100};
    private int fan, expected = -1, level = -1, queued = -1;
    private boolean sg130, encoded, voice, haveReport;
    private boolean queriedAfterWrite, tryingMinimum, atMinimum;
    private String message = "", address = "";

    PhoneLamp(Activity activity, WebView web) {
        this.activity = activity;
        this.web = web;
        BluetoothManager manager = activity.getSystemService(BluetoothManager.class);
        bluetooth = manager == null ? null : manager.getAdapter();
    }
    private static java.util.UUID uuid(int value) {
        return java.util.UUID.fromString(String.format(java.util.Locale.ROOT,
                "%08x-0000-1000-8000-00805f9b34fb", value));
    }
    void show(String text) {
        message = text;
        publish();
    }
    void publish() {
        JSONObject data = new JSONObject();
        try {
            data.put("message", message).put("address", address).put("ready", sg130 || haveReport)
                .put("level", level).put("busy", expected >= 0).put("scanning", scan != null)
                .put("atMinimum", atMinimum).put("sg", sg130).put("names", new JSONObject(labels));
        } catch (org.json.JSONException ex) { throw new IllegalStateException(ex); }
        web.evaluateJavascript("window.phoneLampUpdate && window.phoneLampUpdate(" + data + ")", null);
    }
    private final Runnable reportTimeout = () -> {
        closeLink();
        show("灯具操作超时，请重新连接");
    };
    private final Runnable finishScan = () -> {
        stopScan();
        show(labels.isEmpty() ? "未发现设备，请确认灯已通电后重新扫描" : "请选择要连接的灯具");
    };
    private final Runnable timeout = () -> {
        closeLink();
        show("连接超时，请重新选择灯具");
    };
    void begin() {
        String[] permissions = Build.VERSION.SDK_INT >= 31
                ? new String[]{Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT}
                : new String[]{Manifest.permission.ACCESS_FINE_LOCATION};
        for (String permission : permissions) {
            if (activity.checkSelfPermission(permission) != PackageManager.PERMISSION_GRANTED) {
                activity.requestPermissions(permissions, PERMISSIONS);
                return;
            }
        }
        stopScan();
        closeLink();
        labels.clear();
        if (bluetooth == null || !bluetooth.isEnabled()) {
            show("请打开手机蓝牙，再点扫描");
            return;
        }
        scanner = bluetooth.getBluetoothLeScanner();
        if (scanner == null) { show("蓝牙扫描暂不可用，请重试"); return; }
        scan = new ScanCallback() {
            @Override public void onScanResult(int type, ScanResult result) {
                if (scan != this || (Build.VERSION.SDK_INT >= 26 && !result.isConnectable())) return;
                String mac = result.getDevice().getAddress();
                String name = result.getScanRecord() == null ? null : result.getScanRecord().getDeviceName();
                if (name == null) name = labels.get(mac);
                if (name == null) name = "未命名设备";
                if (name.equals(labels.get(mac))) return;
                labels.put(mac, name);
                publish();
            }
            @Override public void onScanFailed(int code) {
                if (scan != this) return;
                stopScan();
                show("扫描失败（" + code + "），请重试");
            }
        };
        show("正在扫描附近 BLE 设备，请按名称或地址选择灯具");
        scanner.startScan(null, new ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build(), scan);
        handler.postDelayed(finishScan, 8000);
    }

    public void onRequestPermissionsResult(int request, String[] permissions, int[] grants) {
        if (request != PERMISSIONS || grants.length == 0) return;
        for (int result : grants) {
            if (result != PackageManager.PERMISSION_GRANTED) {
                show("请在手机设置里允许蓝牙扫描权限，再点扫描");
                return;
            }
        }
        begin();
    }

    private void stopScan() {
        handler.removeCallbacks(finishScan);
        ScanCallback previous = scan;
        scan = null;
        if (previous != null && bluetooth.isEnabled()) scanner.stopScan(previous);
    }

    void connect(String mac) {
        if (!labels.containsKey(mac)) return;
        stopScan();
        closeLink();
        if (!bluetooth.isEnabled()) { show("请先开启蓝牙"); return; }
        show("正在连接 " + mac);
        address = mac;
        link = bluetooth.getRemoteDevice(mac).connectGatt(activity, false, new BluetoothGattCallback() {
            @Override public void onConnectionStateChange(BluetoothGatt gatt, int code, int next) {
                handler.post(() -> {
                    if (gatt != link) return;
                    handler.removeCallbacks(timeout);
                    if (code == BluetoothGatt.GATT_SUCCESS && next == BluetoothProfile.STATE_CONNECTED) {
                        show("已连接 " + labels.get(mac) + "，正在检测灯控服务");
                        if (!gatt.discoverServices()) show("灯控服务发现失败，请重连");
                        else handler.postDelayed(timeout, 15000);
                    } else {
                        closeLink();
                        show("连接已断开（" + code + "），可点选设备重试");
                    }
                });
            }
            @Override public void onServicesDiscovered(BluetoothGatt g, int code) {
                handler.post(() -> {
                    if (g != link) return;
                    BluetoothGattService legacy = g.getService(Sg130Light.uuid(0xfff0));
                    if (code == 0 && legacy != null && "SG-130-XDb12".equalsIgnoreCase(labels.get(mac))) {
                        BluetoothGattCharacteristic candidate = legacy.getCharacteristic(Sg130Light.uuid(0xffe1));
                        encoded = writable(candidate);
                        writer = encoded ? candidate : legacy.getCharacteristic(Sg130Light.uuid(0xfff1));
                        if (writable(writer)) {
                            handler.removeCallbacks(timeout);
                            sg130 = true;
                            voice = legacy.getCharacteristic(Sg130Light.uuid(0xffe5)) != null;
                            show("已连接 " + labels.get(mac) + "，可用水草灯开关或亮度滑条控制");
                            return;
                        }
                    }
                    BluetoothGattService svc = g.getService(uuid(0xfffa));
                    writer = svc == null ? null : svc.getCharacteristic(uuid(0xff02));
                    if (code != 0 || writer == null ||
                            (writer.getProperties() & (BluetoothGattCharacteristic.PROPERTY_WRITE |
                             BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE)) == 0) {
                        handler.removeCallbacks(timeout);
                        show("蓝牙已连接，此设备没有可用的尼特利灯控服务");
                        return;
                    }
                    if (!g.requestMtu(247)) show("无法协商灯控报文长度，请重连");
                });
            }
            @Override public void onMtuChanged(BluetoothGatt g, int mtu, int code) {
                handler.post(() -> {
                    if (g != link || writer == null || sg130) return;
                    BluetoothGattCharacteristic reader = g.getService(uuid(0xfffa)).getCharacteristic(uuid(0xff01));
                    BluetoothGattDescriptor cccd = reader == null ? null : reader.getDescriptor(uuid(0x2902));
                    if (code != 0 || mtu < 41 || cccd == null ||
                            !g.setCharacteristicNotification(reader, true)) {
                        handler.removeCallbacks(timeout);
                        show("设备不支持当前灯控通知或报文长度");
                        return;
                    }
                    cccd.setValue(BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE);
                    if (!g.writeDescriptor(cccd)) show("启用灯具回报失败，请重连");
                });
            }
            @Override public void onDescriptorWrite(BluetoothGatt g, BluetoothGattDescriptor d, int code) {
                handler.post(() -> {
                    if (g != link) return;
                    handler.removeCallbacks(timeout);
                    if (code != 0 || !send(5, new byte[0])) {
                        show("灯控初始化失败，请重连");
                        return;
                    }
                    show("正在读取灯具实际亮度");
                    handler.postDelayed(reportTimeout, 7000);
                });
            }
            @Override public void onCharacteristicWrite(BluetoothGatt g, BluetoothGattCharacteristic c, int code) {
                byte[] value = c.getValue();
                int cmd = value == null || value.length < 4 ? -1 : value[3];
                handler.post(() -> {
                    if (g != link) return;
                    if (sg130) {
                        handler.removeCallbacks(reportTimeout);
                        int sent = expected;
                        expected = -1;
                        if (code == 0) level = sent;
                        show(code != 0 ? "写入失败（" + code + "）" : sent == 0 ? "已发送最低微光档" : "已发送亮度 " + sent + "%");
                        drain();
                        return;
                    }
                    if (code != 0) { reportTimeout.run(); return; }
                    if (cmd == 5 || cmd == 16) handler.postDelayed(() -> {
                        if (g != link) return;
                        queriedAfterWrite = true;
                        if (!send(27, new byte[]{0})) reportTimeout.run();
                    }, 200);
                });
            }
            @Override public void onCharacteristicChanged(BluetoothGatt g, BluetoothGattCharacteristic c) {
                if (!uuid(0xff01).equals(c.getUuid()) || c.getValue() == null) return;
                byte[] frame = c.getValue().clone();
                handler.post(() -> { if (g == link) report(frame); });
            }
        }, BluetoothDevice.TRANSPORT_LE);
        if (link != null) handler.postDelayed(timeout, 15000);
        else show("无法发起连接，请重试");
    }

    private boolean writable(BluetoothGattCharacteristic c) {
        return c != null && (c.getProperties() & (BluetoothGattCharacteristic.PROPERTY_WRITE |
                BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE)) != 0;
    }

    private boolean send(int cmd, byte[] data) {
        if (link == null || writer == null) return false;
        byte[] frame = new byte[data.length + 7];
        frame[0] = 0x5a; frame[1] = (byte) frame.length; frame[2] = 1; frame[3] = (byte) cmd;
        System.arraycopy(data, 0, frame, 6, data.length);
        for (int i=0; i<frame.length-1; i++) frame[frame.length-1] += frame[i];
        return write(frame);
    }

    private boolean write(byte[] frame) {
        writer.setWriteType((writer.getProperties() & BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE) != 0 ?
                BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE : BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT);
        writer.setValue(frame);
        return link.writeCharacteristic(writer);
    }

    private void report(byte[] f) {
        if (f.length < 20 || f[0] != 0x5a || (f[1]&255) != f.length || f[2]!=1 || f[3]!=20) return;
        byte sum=0;
        for (int i=0; i<f.length-1; i++) sum+=f[i];
        if (sum != f[f.length-1]) return;
        int[] channels = {f[12]&255, f[14]&255, f[13]&255, f[15]&255}; // R,W,G,B
        int peak=0;
        for (int channel : channels) { if (channel > 100) return; peak = Math.max(peak, channel); }
        int level = f[9] == 0 ? 0 : peak;
        // Wait for the post-write query before deciding whether an off request worked.
        if (expected >= 0 && !queriedAfterWrite) return;
        boolean offRejected = expected == 0 && level > 1 && queued < 0;
        boolean minimumReported = level == 1 && (expected == 0 || tryingMinimum ||
                (atMinimum && expected < 0));
        if (expected != 0 && !tryingMinimum && !minimumReported && peak > 0) for (int i=0;i<4;i++) spectrum[i]=(channels[i]*100 + peak/2)/peak;
        if (level > 0 && expected != 0 && !tryingMinimum) fan=f[16]&255;
        haveReport = true;
        handler.removeCallbacks(reportTimeout);
        this.level = level;
        expected = -1;
        atMinimum = minimumReported;
        tryingMinimum = false;
        if (offRejected) {
            tryingMinimum = true;
            applyLevel(1);
            return;
        }
        show(atMinimum ? "关灯后灯具仍亮，已回报最低亮度 1%" : "灯具回报亮度 " + level + "%");
        drain();
    }

    void color(String channel, int value) {
        int index = "rwgb".indexOf(channel);
        if ("fan".equals(channel) && !sg130) fan = Math.max(0, Math.min(255,value));
        else if (channel.length()==1 && index>=0) spectrum[index] = Math.max(0,Math.min(100,value));
        else return;
        if (level>0 || expected>0) applyLevel(queued>=0 ? queued : expected>=0 ? expected : level);
    }

    private void drain() {
        int next = queued;
        queued = -1;
        if (next >= 0) applyLevel(next);
    }

    void applyLevel(int level) {
        if (level < 0 || level > 100 || !(sg130 || haveReport)) {
            show("请先连接灯具");
            return;
        }
        if (expected >= 0) { queued=level; return; }
        if (sg130) {
            int step = level == 0 ? 0 : Math.max(1, Math.round(level/10f));
            if (!write(Sg130Light.manual(step, encoded, voice, spectrum))) {
                show("发送失败，请重新连接");
                return;
            }
            expected = step * 10;
            show(level == 0 ? "正在切换最低微光档" : "正在发送亮度 " + expected + "%");
            handler.postDelayed(reportTimeout, 5000);
            return;
        }
        byte[] data=new byte[16];
        // The recorded CMD16 off frame has mode 1 and every other payload byte zero.
        data[1]=1;
        if (level > 0) {
            data[0]=1;
            for (int i=0;i<4;i++) data[4+i]=(byte)((spectrum[i]*level+50)/100);
            data[8]=(byte)fan; data[9]=(byte)0xe8; data[10]=3; data[11]=1;
        }
        queriedAfterWrite=false;
        if (!send(16, data)) { tryingMinimum = false; show("发送失败，请重试"); return; }
        expected=level;
        show(tryingMinimum ? "关灯后灯具仍亮，正在调至最低亮度" : "已发送，等待灯具回报 " + level + "%");
        handler.removeCallbacks(reportTimeout);
        handler.postDelayed(reportTimeout, 5000);
    }

    private void closeLink() {
        handler.removeCallbacks(timeout);
        handler.removeCallbacks(reportTimeout);
        sg130 = false;
        atMinimum = tryingMinimum = queriedAfterWrite = false;
        level = queued = -1; address = "";
        writer = null;
        expected = -1; haveReport = false;
        java.util.Arrays.fill(spectrum, 100);
        BluetoothGatt previous = link;
        link = null;
        if (previous != null) { previous.disconnect(); previous.close(); }
    }
    void close() {
        stopScan();
        closeLink();
        labels.clear(); message = "";
    }
}
