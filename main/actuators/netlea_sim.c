/**
 * @file netlea_sim.c
 * @brief 尼特利灯的"内置模拟灯"：不需要实体灯也能验证整条控制链路
 *
 * 行为对齐真实设备（见交接文档 §6）：
 *   - 收 CMD16（主灯状态）→ 更新 RGBWF 与开关，并回一帧 CMD20 状态；
 *   - 收 CMD27/29/33（查询）→ 回 CMD20；
 *   - 收 CMD5（初始化）→ 记下 groupId，回 CMD20；
 *   - 其它命令 → 只记录日志，不回报。
 *
 * 有了它，"点 App 滑条 → 固件组帧 → 传输层 → 解析回报 → 状态帧 → App 显示"
 * 这条链路在本机就能闭环，真机只需要把传输层从 sim 换成 ble。
 */
#include "netlea_transport.h"

#include "esp_log.h"
#include "netlea_lamp.h"

static const char *TAG = "NETLEA_SIM";

static bool s_enabled = true;
static netlea_rx_cb_t s_rx = NULL;

/* 模拟灯的内部状态（0-100，风扇 0-255） */
static struct {
    bool on;
    uint8_t r, w, g, b;
    uint8_t fan;
    uint8_t pattern;   /* 1 = 手动 */
    uint8_t mode_id;
    uint8_t grade;
    uint8_t group;
} s_lamp = { .on = false, .r = 0, .w = 0, .g = 0, .b = 0, .fan = 0,
             .pattern = 1, .mode_id = 1, .grade = 1, .group = 0 };

static uint8_t sum8(const uint8_t *p, size_t n) {
    uint8_t s = 0;
    for (size_t i = 0; i < n; i++) {
        s = (uint8_t)(s + p[i]);
    }
    return s;
}

/* 组一帧 CMD20（31 字节状态），格式见交接文档 §6.3 */
static void emit_status(void) {
    if (!s_rx) {
        return;
    }
    uint8_t f[6 + 31 + 1];
    size_t n = 0;
    f[n++] = 0x5A;
    f[n++] = (uint8_t)(6 + 31 + 1);   /* LEN */
    f[n++] = 0x01;                    /* DEV */
    f[n++] = 20;                      /* CMD20 */
    f[n++] = 0x02;                    /* 上报 */
    f[n++] = 0x00;
    /* 主灯 13 字节：stateType/startH/startM/lampState/lampMode/pattern/ R G W B F / r1 r2 */
    f[n++] = 0x00;
    f[n++] = 0x00;
    f[n++] = 0x00;
    f[n++] = s_lamp.on ? 0x01 : 0x00;
    f[n++] = 0x00;
    f[n++] = s_lamp.pattern;
    f[n++] = s_lamp.r;
    f[n++] = s_lamp.g;
    f[n++] = s_lamp.w;
    f[n++] = s_lamp.b;
    f[n++] = s_lamp.fan;
    f[n++] = 0x00;
    f[n++] = 0x00;
    /* 射灯 9 字节：全 0（未接） */
    for (int i = 0; i < 9; i++) {
        f[n++] = 0x00;
    }
    /* 氛围灯 9 字节：全 0（未接） */
    for (int i = 0; i < 9; i++) {
        f[n++] = 0x00;
    }
    f[n] = sum8(f, n);
    n++;
    s_rx(f, n);
}

esp_err_t netlea_transport_init(void) {
    esp_err_t ret = ESP_OK;
    ESP_LOGI(TAG, "模拟灯已启用（没有实体灯时用它验证链路）");
    return ret;
}

bool netlea_transport_ready(void) {
    return s_enabled;
}

const char *netlea_transport_name(void) {
    return s_enabled ? "sim" : "off";
}

void netlea_transport_force_sim(bool on) {
    s_enabled = on;
    if (!on) {
        s_enabled = true;   /* 模拟器是最后兜底，不允许彻底关掉 */
    }
}

bool netlea_transport_is_sim(void) {
    return true;
}

void netlea_transport_set_rx(netlea_rx_cb_t cb) {
    s_rx = cb;
}

void netlea_transport_tick(void) {
}

esp_err_t netlea_transport_write(const uint8_t *buf, size_t len) {
    if (!s_enabled || !buf || len < 7) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 校验长度与累加和，模拟真实设备的行为 */
    if (buf[1] != (uint8_t)len) {
        ESP_LOGW(TAG, "drop: LEN %u != %u", (unsigned)buf[1], (unsigned)len);
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t s = 0;
    for (size_t i = 0; i + 1 < len; i++) {
        s = (uint8_t)(s + buf[i]);
    }
    if (s != buf[len - 1]) {
        ESP_LOGW(TAG, "drop: SUM %02X != %02X", s, buf[len - 1]);
        return ESP_ERR_INVALID_CRC;
    }

    const uint8_t cmd = buf[3];
    const uint8_t *pl = &buf[6];
    const size_t plen = len - 7;

    switch (cmd) {
        case 5:   /* 初始化 */
            if (plen >= 0) {
                s_lamp.group = buf[5];
            }
            emit_status();
            break;
        case 16:  /* 主灯状态 */
            if (plen >= 9) {
                s_lamp.on = (pl[0] != 0);
                uint8_t r = pl[4], w = pl[5], g = pl[6], b = pl[7], fan = pl[8];
                /* 真实设备的行为：无光通道即视为关灯 */
                if (r == 0 && w == 0 && g == 0 && b == 0) {
                    s_lamp.on = false;
                }
                s_lamp.r = r;
                s_lamp.w = w;
                s_lamp.g = g;
                s_lamp.b = b;
                s_lamp.fan = fan;
                if (plen >= 10) {
                    s_lamp.mode_id = pl[9];
                }
            }
            emit_status();
            break;
        case 27:  /* 查询主灯 */
        case 29:
        case 33:
            emit_status();
            break;
        case 4:   /* 校时，不需要回报 */
        case 6:   /* token */
        default:
            break;
    }
    return ESP_OK;
}

/* ---------------- 扫描 / 绑定（模拟器） ---------------- */
static uint8_t s_target[6] = {0xAC, 0xEB, 0xE6, 0x4B, 0x9C, 0xEA};
static bool s_scan_done = true;

void netlea_transport_set_target(const uint8_t mac[6]) {
    if (mac) memcpy(s_target, mac, 6);
}
void netlea_transport_get_target(uint8_t mac[6]) {
    if (mac) memcpy(mac, s_target, 6);
}
void netlea_transport_scan_start(void) {
    s_scan_done = true;      /* 模拟器里"立刻扫完" */
}
int netlea_transport_scan_items(netlea_scan_item_t *out, int max) {
    if (!out || max < 1) return 0;
    memcpy(out[0].mac, s_target, 6);
    out[0].rssi = -50;
    snprintf(out[0].name, sizeof(out[0].name), "NL-SIM-660");
    return 1;
}
bool netlea_transport_scan_done(void) {
    return s_scan_done;
}