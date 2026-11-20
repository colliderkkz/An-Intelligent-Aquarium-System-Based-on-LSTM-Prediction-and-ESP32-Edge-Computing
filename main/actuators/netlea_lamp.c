/**
 * @file netlea_lamp.c
 * @brief 尼特利灯的语义层：组帧 / 解析回报 / NVS / 与板载灯的二选一
 */
#include "netlea_lamp.h"

#include "actuators/light_actuator.h"
#include "drivers/pcf8563.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "netlea_transport.h"
#include "service/act_hold.h"
#include "service/time_service.h"
#include <string.h>

static const char *TAG = "NETLEA";

#define NVS_NS       "fishpz"
#define NVS_KEY      "netlea"
#define NVS_MAGIC    0x4E4C4131u   /* 'NLA1' */
#define SEND_MIN_MS  60            /* 两条命令之间至少隔这么久（灯要求 40ms） */
#define QUERY_PERIOD_MS  30000     /* 30 秒主动查一次，确保界面不"冻结" */

typedef struct {
    uint32_t magic;
    uint8_t src;      /* lamp_source */
    uint8_t on;
    uint8_t bright;
    uint8_t r, g, b, w;
    uint8_t fan;
    uint8_t mode_id;
    uint8_t grade;
} netlea_blob_t;

static netlea_state_t s_tgt;      /* 我们想要的状态 */
static netlea_state_t s_rep;      /* 灯回报的状态 */
static bool s_have_rep = false;
static int  s_src = LAMP_SRC_BOARD;
static bool s_inited = false;
static bool s_dirty = true;
static int64_t s_last_send_us = 0;
static int64_t s_last_query_us = 0;
/* 回报 / 下发的时间戳：用来判断"界面该显示灯回报的旧值，还是刚下发的目标值"。
 * 灯回报比命令新 → 用回报；否则用目标（刚点完马上看到效果，不用等一个来回）。 */
static int64_t s_rep_us = 0;
static int64_t s_cmd_us = 0;

/* ---------------- 帧工具 ---------------- */

static uint8_t sum8(const uint8_t *p, size_t n) {
    uint8_t s = 0;
    for (size_t i = 0; i < n; i++) {
        s = (uint8_t)(s + p[i]);
    }
    return s;
}

static esp_err_t send_frame(uint8_t cmd, uint8_t b5, const uint8_t *payload, size_t plen) {
    uint8_t buf[6 + 16 + 1];
    if (plen > 16) {
        return ESP_ERR_INVALID_SIZE;
    }
    buf[0] = 0x5A;
    buf[1] = (uint8_t)(6 + plen + 1);
    buf[2] = 0x01;          /* DEV */
    buf[3] = cmd;
    buf[4] = 0x00;          /* 请求 */
    buf[5] = b5;
    if (plen) {
        memcpy(&buf[6], payload, plen);
    }
    buf[6 + plen] = sum8(buf, 6 + plen);
    /* 先记下发时间再写：模拟器会在 write 里同步回声，回声时间戳必须更晚 */
    s_cmd_us = esp_timer_get_time();
    esp_err_t ret = netlea_transport_write(buf, 6 + plen + 1);
    s_last_send_us = esp_timer_get_time();
    return ret;
}

/* CMD16：主灯状态 16 字节
 * 布局（与交接文档 §6.2 逐字节对齐）：
 *   [0]state [1]mode [2-3]rsv [4]R [5]W [6]G [7]B [8]F
 *   [9-10]restoreActTime(LE) [11]modeId [12-13]grade(LE) [14-15]rsv
 */
static esp_err_t send_master(void) {
    uint8_t full[16] = {0};
    const uint8_t r = (uint8_t)((uint16_t)s_tgt.r * s_tgt.bright / 100u);
    const uint8_t g = (uint8_t)((uint16_t)s_tgt.g * s_tgt.bright / 100u);
    const uint8_t b = (uint8_t)((uint16_t)s_tgt.b * s_tgt.bright / 100u);
    const uint8_t w = (uint8_t)((uint16_t)s_tgt.w * s_tgt.bright / 100u);
    /* 真实设备行为：四路光全 0 会判为关灯 */
    full[0] = s_tgt.on ? 0x01 : 0x00;
    full[1] = 0x01;                     /* mode=1 手动 */
    full[4] = r;
    full[5] = w;
    full[6] = g;
    full[7] = b;
    full[8] = s_tgt.fan;
    full[9] = 0xE8;                     /* restoreActTime = 1000 s */
    full[10] = 0x03;
    full[11] = s_tgt.mode_id ? s_tgt.mode_id : 1;
    full[12] = s_tgt.grade;
    full[13] = 0x00;
    return send_frame(16, 0x00, full, sizeof(full));
}

/* CMD5：初始化（B5=groupId） */
static esp_err_t send_init(void) {
    return send_frame(5, 0x00, NULL, 0);
}

/* CMD4：校时（tz=8） */
static esp_err_t send_time(void) {
    pcf8563_time_t t = {0};
    time_service_now(&t);
    uint8_t p[9];
    p[0] = 0x00;                       /* groupId */
    p[1] = 0x00;                       /* devIdx */
    p[2] = 8;                          /* tz */
    p[3] = (uint8_t)(t.year > 2000 ? (t.year - 2000) : 0);
    p[4] = t.month;
    p[5] = t.day;
    p[6] = t.hour;
    p[7] = t.minute;
    p[8] = t.second;
    return send_frame(4, 0x00, p, sizeof(p));
}

/* CMD27：查询主灯 */
static esp_err_t send_query(void) {
    return send_frame(27, 0x00, (const uint8_t *)"\x00", 1);
}

/* ---------------- 回报解析 ---------------- */

static void on_lamp_frame(const uint8_t *f, size_t len) {
    if (!f || len < 7 || f[0] != 0x5A || f[1] != (uint8_t)len) {
        return;
    }
    uint8_t s = 0;
    for (size_t i = 0; i + 1 < len; i++) {
        s = (uint8_t)(s + f[i]);
    }
    if (s != f[len - 1]) {
        ESP_LOGW(TAG, "report SUM mismatch");
        return;
    }
    if (f[3] != 20) {                 /* 只关心 CMD20 状态帧 */
        return;
    }
    const uint8_t *p = &f[6];
    const size_t plen = len - 7;
    if (plen < 13) {
        return;
    }
    s_rep.on = (p[3] != 0);
    s_rep.r = p[6];
    s_rep.g = p[7];
    s_rep.w = p[8];
    s_rep.b = p[9];
    s_rep.fan = p[10];
    s_rep.mode_id = 1;
    s_rep.bright = 100;              /* 回报是四路实际值，亮度用四路最大值近似 */
    uint8_t mx = 0;
    if (s_rep.r > mx) mx = s_rep.r;
    if (s_rep.g > mx) mx = s_rep.g;
    if (s_rep.w > mx) mx = s_rep.w;
    if (s_rep.b > mx) mx = s_rep.b;
    s_rep.bright = mx;
    s_have_rep = true;
    s_rep_us = esp_timer_get_time();
}

/* ---------------- NVS ---------------- */

static void load_nvs(void) {
    netlea_blob_t b;
    memset(&b, 0, sizeof(b));
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t len = sizeof(b);
    esp_err_t r = nvs_get_blob(h, NVS_KEY, &b, &len);
    nvs_close(h);
    if (r != ESP_OK || len != sizeof(b) || b.magic != NVS_MAGIC) {
        return;
    }
    s_src = (b.src == LAMP_SRC_NETLEA) ? LAMP_SRC_NETLEA : LAMP_SRC_BOARD;
    s_tgt.on = b.on != 0;
    s_tgt.bright = b.bright <= 100 ? b.bright : 100;
    s_tgt.r = b.r; s_tgt.g = b.g; s_tgt.b = b.b; s_tgt.w = b.w;
    s_tgt.fan = b.fan;
    s_tgt.mode_id = b.mode_id ? b.mode_id : 1;
    s_tgt.grade = b.grade;
}

esp_err_t netlea_lamp_save(void) {
    netlea_blob_t b = {
        .magic = NVS_MAGIC,
        .src = (uint8_t)s_src,
        .on = s_tgt.on ? 1 : 0,
        .bright = s_tgt.bright,
        .r = s_tgt.r, .g = s_tgt.g, .b = s_tgt.b, .w = s_tgt.w,
        .fan = s_tgt.fan,
        .mode_id = s_tgt.mode_id,
        .grade = s_tgt.grade,
    };
    nvs_handle_t h;
    esp_err_t r = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (r != ESP_OK) {
        return r;
    }
    r = nvs_set_blob(h, NVS_KEY, &b, sizeof(b));
    if (r == ESP_OK) {
        r = nvs_commit(h);
    }
    nvs_close(h);
    return r;
}

/* ---------------- 任务 ---------------- */

static void netlea_task(void *arg) {
    (void)arg;
    bool was_ready = false;
    int64_t connected_at = 0;

    while (1) {
        netlea_transport_tick();
        const bool ready = netlea_transport_ready();
        const int64_t now = esp_timer_get_time();

        if (ready && !was_ready) {
            /* 刚连上：走一遍 初始化 → 校时 → 同步一次颜色 */
            ESP_LOGI(TAG, "lamp link up (%s), init+time+sync",
                     netlea_transport_name());
            (void)send_init();
            vTaskDelay(pdMS_TO_TICKS(120));
            (void)send_time();
            vTaskDelay(pdMS_TO_TICKS(120));
            s_dirty = true;
            connected_at = now;
        }
        was_ready = ready;

        if (ready) {
            /* 连接后 800ms 内不要连发，避免和初始化撞车 */
            const bool settle = (now - connected_at) < 800000LL;
            if (s_dirty && !settle && (now - s_last_send_us) > (SEND_MIN_MS * 1000LL)) {
                s_dirty = false;
                if (s_src == LAMP_SRC_NETLEA) {
                    esp_err_t r = send_master();
                    if (r != ESP_OK) {
                        ESP_LOGW(TAG, "cmd16 failed: %s", esp_err_to_name(r));
                        s_dirty = true;
                    }
                }
            }
            if ((now - s_last_query_us) > (QUERY_PERIOD_MS * 1000LL)) {
                s_last_query_us = now;
                (void)send_query();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* ---------------- 对外 API ---------------- */


/* ---------------- 灯具搜索 / 绑定 ---------------- */
#define NVS_KEY_MAC "netlea_mac"
static char s_mac_str[18] = "";

static void mac_to_str(const uint8_t *m, char *out, size_t n) {
    /* 输入可能是小端 val[]，统一按人读顺序输出 */
    snprintf(out, n, "%02X:%02X:%02X:%02X:%02X:%02X",
             m[5], m[4], m[3], m[2], m[1], m[0]);
}

static bool parse_mac(const char *s, uint8_t out[6]) {
    int v[6], n = 0;
    const char *p = s;
    while (*p && n < 6) {
        if (*p == ':' || *p == '-' || *p == ' ') { p++; continue; }
        int hi = -1, lo = -1;
        char a = *p, b = *(p + 1);
        if (a >= '0' && a <= '9') hi = a - '0';
        else if (a >= 'a' && a <= 'f') hi = a - 'a' + 10;
        else if (a >= 'A' && a <= 'F') hi = a - 'A' + 10;
        if (b >= '0' && b <= '9') lo = b - '0';
        else if (b >= 'a' && b <= 'f') lo = b - 'a' + 10;
        else if (b >= 'A' && b <= 'F') lo = b - 'A' + 10;
        if (hi < 0 || lo < 0) return false;
        v[n++] = hi * 16 + lo;
        p += 2;
    }
    if (n != 6 || *p) return false;
    for (int i = 0; i < 6; i++) out[i] = (uint8_t)v[i];
    return true;
}

esp_err_t netlea_lamp_scan_start(void) {
    netlea_transport_scan_start();
    return ESP_OK;
}
int netlea_lamp_scan_items(netlea_scan_item_t *out, int max) {
    return netlea_transport_scan_items(out, max);
}
bool netlea_lamp_scan_done(void) {
    return netlea_transport_scan_done();
}
const char *netlea_lamp_mac_str(void) {
    return s_mac_str;
}

esp_err_t netlea_lamp_bind(const char *mac) {
    uint8_t m[6];
    if (!mac || !parse_mac(mac, m)) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t rev[6];
    for (int i = 0; i < 6; i++) {
        rev[i] = m[5 - i];       /* 人读顺序 -> ble_addr_t 小端 */
    }
    nvs_handle_t h;
    esp_err_t r = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (r != ESP_OK) return r;
    r = nvs_set_blob(h, NVS_KEY_MAC, rev, sizeof(rev));
    if (r == ESP_OK) r = nvs_commit(h);
    nvs_close(h);
    if (r != ESP_OK) return r;
    netlea_transport_set_target(rev);
    mac_to_str(rev, s_mac_str, sizeof(s_mac_str));   /* rev 是小端 val，打印时会反转回人读序 */
    s_dirty = true;
    ESP_LOGW(TAG, "lamp bound to %s", s_mac_str);
    return ESP_OK;
}

static void load_mac_nvs(void) {
    uint8_t m[6];
    size_t len = sizeof(m);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    esp_err_t r = nvs_get_blob(h, NVS_KEY_MAC, m, &len);
    nvs_close(h);
    if (r == ESP_OK && len == 6) {
        netlea_transport_set_target(m);
        mac_to_str(m, s_mac_str, sizeof(s_mac_str));
    }
}
esp_err_t netlea_lamp_init(void) {
    if (s_inited) {
        return ESP_OK;
    }
    memset(&s_tgt, 0, sizeof(s_tgt));
    memset(&s_rep, 0, sizeof(s_rep));
    s_tgt.bright = 100;
    s_tgt.mode_id = 1;
    s_tgt.grade = 1;
    load_nvs();
    load_mac_nvs();

    netlea_transport_set_rx(on_lamp_frame);
    esp_err_t r = netlea_transport_init();
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "transport init failed: %s (fallback to sim)", esp_err_to_name(r));
    }
    if (xTaskCreate(netlea_task, "netlea", 4096, NULL, 2, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_inited = true;
    ESP_LOGI(TAG, "ready: src=%s, transport=%s, bright=%u",
             s_src == LAMP_SRC_NETLEA ? "netlea" : "board",
             netlea_transport_name(), (unsigned)s_tgt.bright);
    return ESP_OK;
}

int lamp_source_get(void) {
    return s_src;
}

esp_err_t lamp_source_set(int src) {
    s_src = (src == LAMP_SRC_NETLEA) ? LAMP_SRC_NETLEA : LAMP_SRC_BOARD;
    if (s_src == LAMP_SRC_NETLEA) {
        /* 切到尼特利：板载 PWM 先关掉（逻辑亮度由 light_actuator 保留） */
        (void)light_actuator_apply_board_duty(0.0f);
        s_dirty = true;
    } else {
        /* 切回板载：把尼特利关掉，再按当前亮度点板载灯 */
        s_tgt.on = false;
        s_dirty = true;
        (void)light_actuator_apply_board_duty(light_actuator_get_brightness());
    }
    (void)netlea_lamp_save();
    ESP_LOGW(TAG, "lamp source -> %s", s_src == LAMP_SRC_NETLEA ? "netlea" : "board");
    return ESP_OK;
}

bool netlea_lamp_ready(void) {
    return netlea_transport_ready();
}

const char *netlea_lamp_transport(void) {
    return netlea_transport_name();
}

esp_err_t netlea_lamp_set_sim(int on) {
    netlea_transport_force_sim(on != 0);
    s_dirty = true;
    return ESP_OK;
}

esp_err_t netlea_lamp_set_level(uint8_t pct) {
    if (pct > 100) {
        pct = 100;
    }
    s_tgt.bright = pct;
    s_tgt.on = pct > 0;
    if (s_tgt.r == 0 && s_tgt.g == 0 && s_tgt.b == 0 && s_tgt.w == 0) {
        s_tgt.w = 100;              /* 从没设过颜色：默认白光 */
    }
    s_dirty = true;
    return ESP_OK;
}

esp_err_t netlea_lamp_set_rgbw(uint8_t r, uint8_t g, uint8_t b, uint8_t w) {
    #define CLAMP100(x) do { if ((x) > 100) { (x) = 100; } } while (0)
    CLAMP100(r); CLAMP100(g); CLAMP100(b); CLAMP100(w);
    #undef CLAMP100
    s_tgt.r = r; s_tgt.g = g; s_tgt.b = b; s_tgt.w = w;
    const bool any = (r | g | b | w) != 0;
    s_tgt.on = any;
    if (any && s_tgt.bright == 0) {
        s_tgt.bright = 100;
    }
    s_dirty = true;
    return ESP_OK;
}

esp_err_t netlea_lamp_set_fan(uint8_t f) {
    s_tgt.fan = f;
    s_dirty = true;
    return ESP_OK;
}

esp_err_t netlea_lamp_power(bool on) {
    s_tgt.on = on;
    if (on && s_tgt.bright == 0) {
        s_tgt.bright = 100;
    }
    if (on && s_tgt.r == 0 && s_tgt.g == 0 && s_tgt.b == 0 && s_tgt.w == 0) {
        s_tgt.w = 100;
    }
    s_dirty = true;
    return ESP_OK;
}

void netlea_lamp_reported(netlea_state_t *out) {
    if (!out) {
        return;
    }
    /* 只有"灯回报比最近一次下发更新"时才用回报值；
     * 否则用目标值，保证 App 点完立刻能看到效果（不用等一个来回）。 */
    if (s_have_rep && s_rep_us >= s_cmd_us) {
        *out = s_rep;
        out->bright = s_tgt.bright;
    } else {
        *out = s_tgt;
    }
}

void netlea_lamp_target(netlea_state_t *out) {
    if (out) {
        *out = s_tgt;
    }
}

esp_err_t netlea_lamp_query(void) {
    return send_query();
}

esp_err_t netlea_lamp_reconnect(void) {
    netlea_transport_force_sim(false);
    return ESP_OK;
}
