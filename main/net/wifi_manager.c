#include "net/wifi_manager.h"

/* 启用 WPA2-Enterprise（校园网 PEAP）支持 */
#define NET_WIFI_ENTERPRISE 1

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * WPA2-Enterprise(PEAP) 需要 IDF 的 wpa_supplicant + esp_eap_client，
 * 并打开 CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT。
 * 默认关闭：先用普通 PSK（手机热点/家用路由）把整条 MQTT 链路跑通；
 * 要连学校网时再定义 NET_WIFI_ENTERPRISE 并启用对应 Kconfig。
 */
#ifdef NET_WIFI_ENTERPRISE
#include "esp_eap_client.h"
#endif

static const char *TAG = "NET";

#define NET_NS        "fishnet"
#define NET_KEY       "cfg"
#define NET_RETRY_MS  5000
#define NET_SSID_LEN  33
#define NET_USER_LEN  65
#define NET_PASS_LEN  65
#define NET_URI_LEN   96
#define NET_PFX_LEN   48
#define NET_WIFI_MAX  6      /* 最多记 6 组 WiFi 账密 */

/* 一条 WiFi 记录（家里路由 / 手机热点 / 公司 …） */
typedef struct {
    char ssid[NET_SSID_LEN];
    char pass[NET_PASS_LEN];
    char user[NET_USER_LEN];   /* 企业网账号（PEAP 用户名），PSK 时为空 */
    uint8_t ent;              /* 1 = WPA2-Enterprise（账号+密码） */
} wifi_cred_t;

/* WiFi 列表单独一个 NVS key：结构体再变也不会把已存数据读错位 */
#define NET_KEY_WIFI "wifi"
typedef struct {
    uint8_t ver;              /* 固定 1 */
    uint8_t n;
    wifi_cred_t wl[NET_WIFI_MAX];
} wifi_store_t;

typedef struct {
    char ssid[NET_SSID_LEN];
    char user[NET_USER_LEN];      /* WPA2-Enterprise 的账号 */
    char pass[NET_PASS_LEN];      /* WiFi 密码 / EAP 密码 */
    char uri[NET_URI_LEN];
    char prefix[NET_PFX_LEN];
    uint8_t ent;          /* 1 = WPA2-Enterprise(PEAP)，0 = 普通 PSK */
    uint8_t has_cfg;      /* ssid 非空 */

    /* --- 以下为新增字段，必须加在末尾，靠 NVS 的短 blob 兼容旧配置 --- */
    char mqtt_user[48];   /* NAS 自建 broker 的账号（公共 broker 留空） */
    char mqtt_pass[64];
    char uri2[NET_URI_LEN];   /* 第二路 broker：公网直连，NAS 挂了这个还通 */

    /* --- 多 WiFi 记录（同样加在末尾，旧 blob 读进来就是 wl_n=0，走迁移） --- */
    wifi_cred_t wl[NET_WIFI_MAX];
    uint8_t wl_n;
} net_cfg_t;

static net_cfg_t s_cfg;
static wifi_store_t s_wifi;   /* 多 WiFi 列表（独立 NVS key） */
static bool s_inited = false;
static bool s_started = false;
static bool s_connected = false;
static char s_ip[16] = "0.0.0.0";
static char s_summ[384] = "";
static esp_netif_t *s_netif = NULL;
static TaskHandle_t s_retry_task = NULL;

/* 多 WiFi 连接状态机 */
static volatile int s_good_idx = 0;        /* 上次连上的槽位 */
static volatile uint32_t s_disc_seq = 0;   /* 每次断开 +1，用来提前结束等待 */
static volatile int s_last_disc_reason = 0;/* 最近一次断开原因（WIFI_REASON_*） */
/* 用户/上位机显式要求"重新连一下"：由 retry_task 立刻执行，
 * 避免在串口命令里长时间阻塞（NET,CONNECT 以前要卡 9~54 秒）。 */
static volatile bool s_want_reconnect = false;
/* 用户明确要求断开（NET,DISCONNECT）：后台重连任务要闭嘴，
 * 否则 5 秒后又自己连回去，"断开"按钮形同虚设。 */
static volatile bool s_user_stop = false;
static bool s_wifi_scanning = false;
/* 扫描和连接互斥：两者都要求 STA 处于"空闲"，同时来就会互相打断，
 * 表现是 NET,SCAN 扫出 0 个 AP、或者刚连上又被扫描踢下来。 */
static volatile bool s_net_busy = false;

static void set_defaults(void) {
    memset(&s_cfg, 0, sizeof(s_cfg));
    /* 只有一路 broker：家里 NAS 上的 Mosquitto。
     * 外网访问靠 NAS 上的 PassNAT 隧道把 8083 映射出去，
     * 不再连任何境外公共 broker（原来那路 broker-cn.emqx.io 已移除）。 */
    snprintf(s_cfg.uri, sizeof(s_cfg.uri), "mqtt://mqtt.local:1883");
    snprintf(s_cfg.prefix, sizeof(s_cfg.prefix), "fishtank-eed4");
    s_cfg.uri2[0] = '\0';
}

static void load_cfg(void) {
    set_defaults();
    nvs_handle_t h;
    if (nvs_open(NET_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    /* 允许旧版本更短的 blob：新字段都加在结构体末尾，短 blob 读进来后
     * 剩余部分保持 memset 的 0，等于"新字段默认空"，不会把 WiFi 配置搞丢。 */
    size_t len = sizeof(s_cfg);
    net_cfg_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    if (nvs_get_blob(h, NET_KEY, &tmp, &len) == ESP_OK && len > 8) {
        s_cfg = tmp;
        /* 注意：这里不能再把 uri2 清空 —— 之前为了甩掉旧的 emqx 默认值，
         * 在这里写过清空语句，结果用户设的"公网隧道地址"每次开机都被抹掉，
         * 第二路永远起不来。默认值本来就是空，存进去什么就用什么。 */
    }
    memset(&s_wifi, 0, sizeof(s_wifi));
    {
        size_t wl_len = sizeof(s_wifi);
        if (nvs_get_blob(h, NET_KEY_WIFI, &s_wifi, &wl_len) != ESP_OK ||
            s_wifi.ver != 1 || s_wifi.n > NET_WIFI_MAX) {
            memset(&s_wifi, 0, sizeof(s_wifi));
        }
    }
    nvs_close(h);

    /* 迁移：老固件只存了一条 ssid/pass（可能还带企业网账号），搬进列表第 0 项 */
    if (s_wifi.n == 0 && s_cfg.ssid[0] != '\0') {
        snprintf(s_wifi.wl[0].ssid, sizeof(s_wifi.wl[0].ssid), "%s", s_cfg.ssid);
        snprintf(s_wifi.wl[0].pass, sizeof(s_wifi.wl[0].pass), "%s", s_cfg.pass);
        snprintf(s_wifi.wl[0].user, sizeof(s_wifi.wl[0].user), "%s", s_cfg.user);
        s_wifi.wl[0].ent = s_cfg.ent;
        s_wifi.n = 1;
        s_wifi.ver = 1;
        ESP_LOGI(TAG, "wifi list seeded from legacy cfg: %s (ent=%d)",
                 s_wifi.wl[0].ssid, s_wifi.wl[0].ent);
    }
    if (s_wifi.n > NET_WIFI_MAX) {
        s_wifi.n = NET_WIFI_MAX;
    }
    s_cfg.has_cfg = (s_wifi.n > 0) ? 1 : 0;
}

/* ---------------- 多 WiFi：列表管理 ---------------- */

int net_wifi_count(void) {
    return s_wifi.n;
}

const char *net_wifi_ssid_at(int i) {
    if (i < 0 || i >= s_wifi.n) {
        return "";
    }
    return s_wifi.wl[i].ssid;
}

int net_wifi_ent_at(int i) {
    if (i < 0 || i >= s_wifi.n) {
        return 0;
    }
    return s_wifi.wl[i].ent ? 1 : 0;
}

const char *net_wifi_user_at(int i) {
    if (i < 0 || i >= s_wifi.n) {
        return "";
    }
    return s_wifi.wl[i].user;
}

int net_wifi_active_index(void) {
    /* 当前"正在用"的槽位：ssid 与列表匹配的那个 */
    for (int i = 0; i < s_wifi.n; i++) {
        if (strcmp(s_wifi.wl[i].ssid, s_cfg.ssid) == 0) {
            return i;
        }
    }
    return -1;
}

esp_err_t net_wifi_add_ex(const char *ssid, const char *pass,
                           const char *user, int ent) {
    if (!ssid || !ssid[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *pw = pass ? pass : "";
    const char *acct = user ? user : "";
    /* 已存在就更新密码/账号，不重复占槽位 */
    for (int i = 0; i < s_wifi.n; i++) {
        if (strcmp(s_wifi.wl[i].ssid, ssid) == 0) {
            snprintf(s_wifi.wl[i].pass, sizeof(s_wifi.wl[i].pass), "%s", pw);
            snprintf(s_wifi.wl[i].user, sizeof(s_wifi.wl[i].user), "%s", acct);
            s_wifi.wl[i].ent = (ent != 0) ? 1 : 0;
            ESP_LOGI(TAG, "wifi updated: %s (ent=%d user=%s)", ssid,
                     s_wifi.wl[i].ent, s_wifi.wl[i].user[0] ? s_wifi.wl[i].user : "-");
            return ESP_OK;
        }
    }
    if (s_wifi.n >= NET_WIFI_MAX) {
        return ESP_ERR_NO_MEM;          /* 列表满 */
    }
    wifi_cred_t *c = &s_wifi.wl[s_wifi.n];
    snprintf(c->ssid, sizeof(c->ssid), "%s", ssid);
    snprintf(c->pass, sizeof(c->pass), "%s", pw);
    snprintf(c->user, sizeof(c->user), "%s", acct);
    c->ent = (ent != 0) ? 1 : 0;
    s_wifi.n++;
    s_cfg.has_cfg = 1;
    ESP_LOGI(TAG, "wifi added: %s (ent=%d, total %d)", ssid, c->ent, s_wifi.n);
    return ESP_OK;
}

esp_err_t net_wifi_add(const char *ssid, const char *pass) {
    return net_wifi_add_ex(ssid, pass, "", 0);
}

esp_err_t net_wifi_set_ent(int idx, int ent) {
    if (idx < 0 || idx >= s_wifi.n) {
        return ESP_ERR_INVALID_ARG;
    }
    s_wifi.wl[idx].ent = (ent != 0) ? 1 : 0;
    ESP_LOGI(TAG, "wifi[%d] %s -> ent=%d (密码保持不变)", idx,
             s_wifi.wl[idx].ssid, s_wifi.wl[idx].ent);
    return ESP_OK;
}

esp_err_t net_wifi_swap(int a, int b) {
    if (a < 0 || b < 0 || a >= s_wifi.n || b >= s_wifi.n || a == b) {
        return ESP_ERR_INVALID_ARG;
    }
    wifi_cred_t tmp = s_wifi.wl[a];
    s_wifi.wl[a] = s_wifi.wl[b];
    s_wifi.wl[b] = tmp;
    ESP_LOGI(TAG, "wifi swapped %d <-> %d", a, b);
    return ESP_OK;
}

esp_err_t net_wifi_del(int idx) {
    if (idx < 0 || idx >= s_wifi.n) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = idx; i + 1 < s_wifi.n; i++) {
        s_wifi.wl[i] = s_wifi.wl[i + 1];
    }
    s_wifi.n--;
    memset(&s_wifi.wl[s_wifi.n], 0, sizeof(s_wifi.wl[0]));
    s_cfg.has_cfg = (s_wifi.n > 0) ? 1 : 0;
    ESP_LOGI(TAG, "wifi removed idx=%d (left %d)", idx, s_wifi.n);
    return ESP_OK;
}

void net_wifi_clear(void) {
    memset(s_wifi.wl, 0, sizeof(s_wifi.wl));
    s_wifi.n = 0;
    s_cfg.has_cfg = 0;
}

/* 按给定槽位下发配置并发起连接 */
static esp_err_t apply_and_connect(int idx) {
    if (idx < 0 || idx >= s_wifi.n) {
        return ESP_ERR_INVALID_ARG;
    }
    const wifi_cred_t *c = &s_wifi.wl[idx];

    /* 目标 SSID 记到 s_cfg.ssid，NET,STATUS / 状态上报都读它 */
    snprintf(s_cfg.ssid, sizeof(s_cfg.ssid), "%s", c->ssid);
    snprintf(s_cfg.pass, sizeof(s_cfg.pass), "%s", c->pass);
    s_cfg.ent = c->ent;

    wifi_config_t wcfg;
    memset(&wcfg, 0, sizeof(wcfg));
    memcpy(wcfg.sta.ssid, c->ssid, strnlen(c->ssid, sizeof(wcfg.sta.ssid)));

    if (c->ent) {
        /* 校园网/企业网：PEAP-MSCHAPv2，账号密码走 EAP，不是"WiFi 密码"。
         * 之前的版本把这段删掉了，导致录了企业网也连不上。 */
        (void)esp_eap_client_clear_identity();
        (void)esp_eap_client_clear_username();
        (void)esp_eap_client_clear_password();
        (void)esp_eap_client_set_identity((const unsigned char *)c->user,
                                          (int)strlen(c->user));
        (void)esp_eap_client_set_username((const unsigned char *)c->user,
                                          (int)strlen(c->user));
        (void)esp_eap_client_set_password((const unsigned char *)c->pass,
                                          (int)strlen(c->pass));
        esp_err_t er = esp_wifi_sta_enterprise_enable();
        if (er != ESP_OK) {
            ESP_LOGW(TAG, "enterprise enable failed: %s", esp_err_to_name(er));
        }
        wcfg.sta.threshold.authmode = WIFI_AUTH_WPA2_ENTERPRISE;
        wcfg.sta.pmf_cfg.capable = false;
        wcfg.sta.pmf_cfg.required = false;
    } else {
        (void)esp_wifi_sta_enterprise_disable();
        memcpy(wcfg.sta.password, c->pass, strnlen(c->pass, sizeof(wcfg.sta.password)));
        wcfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        wcfg.sta.pmf_cfg.capable = true;
        wcfg.sta.pmf_cfg.required = false;
    }

    (void)esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(150));
    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &wcfg);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "set_config(%s) failed: %s", c->ssid, esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "connecting to \"%s\" (slot %d/%d)", c->ssid, idx + 1, s_wifi.n);
    return esp_wifi_connect();
}

/* 扫一遍，把"可见的已知 SSID"按信号强弱排序返回槽位下标 */
static int rank_visible(int *order, int max) {
    if (!s_started) {
        return 0;
    }
    wifi_scan_config_t sc;
    memset(&sc, 0, sizeof(sc));
    sc.show_hidden = 0;
    s_wifi_scanning = true;
    esp_err_t r = esp_wifi_scan_start(&sc, true);
    s_wifi_scanning = false;
    if (r != ESP_OK) {
        return 0;
    }
    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);
    if (num == 0) {
        return 0;
    }
    if (num > 32) {
        num = 32;
    }
    wifi_ap_record_t *aps = (wifi_ap_record_t *)calloc(num, sizeof(wifi_ap_record_t));
    if (!aps) {
        return 0;
    }
    uint16_t got = num;
    if (esp_wifi_scan_get_ap_records(&got, aps) != ESP_OK) {
        free(aps);
        return 0;
    }

    /* 按"记录顺序"优先，而不是信号强弱：
     * 信号强不代表能上网（例如校园网信号强但连不到家里的 broker）。
     * 用户在列表里的顺序就是他想要的优先级。只在"看得见的"里面按顺序取。 */
    int n = 0;
    for (int i = 0; i < s_wifi.n && n < max; i++) {
        for (int k = 0; k < got; k++) {
            if (strcmp((const char *)aps[k].ssid, s_wifi.wl[i].ssid) == 0) {
                order[n++] = i;
                break;
            }
        }
    }
    free(aps);
    return n;
}

/* 断开原因里哪些属于"这台 AP 肯定连不上"，可以立刻换下一个。
 * 关键：我们自己调用 esp_wifi_disconnect() 产生的是 ASSOC_LEAVE(8)，
 * 以及 DHCP 慢导致的 BEACON_TIMEOUT 之类要给它时间，不能一看到断开就放弃。 */
static bool disc_is_fatal(int reason) {
    switch (reason) {
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:      /* 15：密码错 */
        case WIFI_REASON_NO_AP_FOUND:                 /* 201 */
        case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY: /* 210 */
        case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD: /* 211 */
        case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:     /* 212 */
        case WIFI_REASON_AUTH_FAIL:                   /* 202：认证失败 */
        case WIFI_REASON_802_1X_AUTH_FAILED:          /* 23：企业网账号密码错 */
        case WIFI_REASON_HANDSHAKE_TIMEOUT:           /* 204 */
        case WIFI_REASON_AUTH_EXPIRE:                 /* 2  */
        case WIFI_REASON_ASSOC_FAIL:                  /* 203 */
            return true;
        default:
            return false;
    }
}

/* 等结果：连上返回 true；明确失败（密码错/搜不到）就提前返回 false */
static bool wait_connected(uint32_t timeout_ms, uint32_t disc_before) {
    const uint32_t t0 = (uint32_t)xTaskGetTickCount();
    while ((uint32_t)(xTaskGetTickCount() - t0) < pdMS_TO_TICKS(timeout_ms)) {
        if (s_connected) {
            return true;
        }
        const uint32_t elapsed = (uint32_t)(xTaskGetTickCount() - t0);
        if (s_disc_seq != disc_before && elapsed > pdMS_TO_TICKS(1200) &&
            disc_is_fatal(s_last_disc_reason)) {
            return false;               /* 已经明确失败（多半是密码错/不在范围） */
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return s_connected;
}

/**
 * @brief 从已记录的 WiFi 里挑一个连上
 *
 * 策略：先扫一遍，只在"当前能看到的"里按信号强弱挑；一个都看不到就用
 * 上次成功的那个直接试（省掉一次扫描）。这样家里/热点/公司多处跑，断电重启后
 * 只要能搜到任意一个已知网络就能自动上线。
 */
bool net_wifi_pick_and_connect(void) {
    if (!s_started || s_wifi.n == 0 || s_connected) {
        return s_connected;
    }
    if (s_net_busy) {
        return s_connected;         /* 正在扫描/别的连接流程，别插队 */
    }
    s_net_busy = true;

    int order[NET_WIFI_MAX];
    int n = rank_visible(order, NET_WIFI_MAX);
    if (n == 0) {
        /* 没扫到已知网络：先试上次成功的那个，再按顺序逐个试一遍 */
        if (s_good_idx >= 0 && s_good_idx < s_wifi.n) {
            order[n++] = s_good_idx;
        }
        for (int i = 0; i < s_wifi.n && n < NET_WIFI_MAX; i++) {
            bool dup = false;
            for (int k = 0; k < n; k++) {
                if (order[k] == i) {
                    dup = true;
                    break;
                }
            }
            if (!dup) {
                order[n++] = i;
            }
        }
    }

    /* 总预算：最多磨 20 秒。
     * 以前 6 个 AP × 9 秒 = 54 秒全阻塞在调用者身上（NET,CONNECT 或开机流程），
     * 串口这段时间完全没有响应，看起来就是"板子死了"。 */
    const uint32_t t_start = (uint32_t)xTaskGetTickCount();

    for (int i = 0; i < n; i++) {
        if (i > 0 && (uint32_t)(xTaskGetTickCount() - t_start) > pdMS_TO_TICKS(20000)) {
            ESP_LOGW(TAG, "wifi pick: 20s budget used up, giving up (%d/%d tried)", i, n);
            break;
        }
        const int idx = order[i];
        const uint32_t before = s_disc_seq;
        if (apply_and_connect(idx) != ESP_OK) {
            continue;
        }
        if (wait_connected(8000, before)) {
            s_good_idx = idx;
            ESP_LOGI(TAG, "wifi up: %s ip=%s", s_wifi.wl[idx].ssid, s_ip);
            s_net_busy = false;
            return true;
        }
        ESP_LOGW(TAG, "wifi \"%s\" not usable, try next", s_wifi.wl[idx].ssid);
    }
    s_net_busy = false;
    return false;
}

/* 断开后延时重连（独立任务，避免阻塞事件循环） */
static void retry_task(void *arg) {
    (void)arg;
    while (1) {
        /* 用户刚点了"连接"：0.5 秒内就动手，别让他等满 5 秒 */
        vTaskDelay(pdMS_TO_TICKS(s_want_reconnect ? 500 : NET_RETRY_MS));
        if (s_user_stop) {
            s_want_reconnect = false;
            continue;
        }
        if (!s_started || s_wifi.n == 0 || s_net_busy) {
            continue;
        }
        if (!s_connected || s_want_reconnect) {
            s_want_reconnect = false;
            /* 不再写死重连某一个 SSID：从记录列表里挑一个能用的 */
            if (!s_connected) {
                ESP_LOGI(TAG, "no link, picking from %d saved wifi ...", s_wifi.n);
            }
            (void)net_wifi_pick_and_connect();
        }
    }
    s_retry_task = NULL;
    vTaskDelete(NULL);
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_cfg.has_cfg) {
            (void)esp_wifi_connect();
        }
        /* 关掉 WiFi 省电：默认 MIN_MODEM 会让发包等 DTIM 唤醒窗口，
         * 实测让局域网 MQTT 往返多出 150~250ms。本设备是市电供电，
         * 完全没必要省这点电，直接用 WIFI_PS_NONE 换取低延迟。 */
        (void)esp_wifi_set_ps(WIFI_PS_NONE);
        ESP_LOGI(TAG, "wifi power save disabled (WIFI_PS_NONE)");
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        s_disc_seq++;
        const int reason = ((wifi_event_sta_disconnected_t *)data)->reason;
        s_last_disc_reason = reason;
        snprintf(s_ip, sizeof(s_ip), "0.0.0.0");
        if (!s_wifi_scanning) {
            ESP_LOGW(TAG, "disconnected (reason=%d, ssid=%s)", reason, s_cfg.ssid);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = (const ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_connected = true;
        /* 重连后省电模式可能被重置，这里再设一次，确保一直是低延迟 */
        (void)esp_wifi_set_ps(WIFI_PS_NONE);
        ESP_LOGI(TAG, "got ip %s", s_ip);
    }
}

esp_err_t net_init(void) {
    if (s_inited) {
        return ESP_OK;
    }
    load_cfg();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    s_inited = true;
    ESP_LOGI(TAG, "wifi manager ready (ssid=%s, mode=%s)",
             s_cfg.has_cfg ? s_cfg.ssid : "<unset>", s_cfg.ent ? "enterprise" : "psk");
    return ESP_OK;
}

esp_err_t net_connect(void) {
    if (!s_inited) {
        esp_err_t ret = net_init();
        if (ret != ESP_OK) {
            return ret;
        }
    }
    if (s_wifi.n == 0) {
        ESP_LOGW(TAG, "no wifi credential saved");
        return ESP_ERR_INVALID_STATE;
    }

    /* 需要"重新连一下"就先把意图记下来：即使下面因为正忙没连成，
     * retry_task 也会立刻接手，不会出现"点了连接却回失败"的假告警。 */
    s_user_stop = false;
    s_want_reconnect = true;
    if (s_net_busy) {
        ESP_LOGI(TAG, "connect requested while picker busy, queued");
        return ESP_OK;                  /* 已受理 */
    }

    /* WiFi 已经在连接/已连接时不能改配置（ESP_ERR_WIFI_STATE），
     * 先断开等一下再设。重连任务可能正好在尝试，这里要抢在它前面。 */
    if (s_started) {
        (void)esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(300));
    }

    if (!s_started) {
        esp_err_t ret = esp_wifi_start();
        if (ret != ESP_OK && ret != ESP_ERR_WIFI_CONN) {
            ESP_LOGE(TAG, "wifi_start failed: %s", esp_err_to_name(ret));
            return ret;
        }
        s_started = true;
        if (!s_retry_task) {
            xTaskCreate(retry_task, "net_retry", 4096, NULL, 3, &s_retry_task);
        }
    }

    s_want_reconnect = false;
    /* 从记录的多条 WiFi 里扫一遍挑一个能用的连上。
     * 返回 ESP_OK 也可能还没拿到 IP（DHCP 慢），调用方应以 NET,STATUS 为准。 */
    return net_wifi_pick_and_connect() ? ESP_OK : ESP_FAIL;
}

/* 后台连接：开机时不能因为 WiFi 慢就把整个系统卡在 app_main 里
 * （以前最坏 54 秒，自动控制任务要等这么久才开始跑）。 */
static void net_connect_task(void *arg) {
    (void)arg;
    esp_err_t ret = net_connect();
    ESP_LOGI(TAG, "async connect done: %s (ip=%s)", esp_err_to_name(ret), net_ip());
    vTaskDelete(NULL);
}

esp_err_t net_connect_async(void) {
    if (!s_inited) {
        esp_err_t ret = net_init();
        if (ret != ESP_OK) {
            return ret;
        }
    }
    if (s_wifi.n == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xTaskCreate(net_connect_task, "net_conn", 4096, NULL, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void net_disconnect(void) {
    /* 标记"用户要求断开"，让后台重连任务不要偷偷连回来 */
    s_user_stop = true;
    s_want_reconnect = false;
    if (s_started) {
        (void)esp_wifi_disconnect();
    }
    s_connected = false;
    snprintf(s_ip, sizeof(s_ip), "0.0.0.0");
}

bool net_wifi_up(void) {
    return s_connected;
}

const char *net_ip(void) {
    return s_ip;
}

bool net_has_config(void) {
    return s_cfg.has_cfg != 0;
}

const char *net_prefix(void) {
    return s_cfg.prefix;
}

const char *net_mqtt_uri(void) {
    return s_cfg.uri;
}

const char *net_mqtt2_uri(void) {
    return s_cfg.uri2;
}

const char *net_mqtt_user(void) {
    return s_cfg.mqtt_user;
}

const char *net_mqtt_pass(void) {
    return s_cfg.mqtt_pass;
}

const char *net_get(const char *key) {
    static char buf[NET_URI_LEN];
    if (!key) {
        return "";
    }
    if (strcmp(key, "ssid") == 0) {
        return s_cfg.ssid;
    }
    if (strcmp(key, "user") == 0) {
        return s_cfg.user;
    }
    if (strcmp(key, "pass") == 0) {
        return s_cfg.pass;
    }
    if (strcmp(key, "uri") == 0) {
        return s_cfg.uri;
    }
    if (strcmp(key, "uri2") == 0) {
        return s_cfg.uri2;
    }
    if (strcmp(key, "prefix") == 0) {
        return s_cfg.prefix;
    }
    if (strcmp(key, "mqtt_user") == 0) {
        return s_cfg.mqtt_user;
    }
    if (strcmp(key, "mqtt_pass") == 0) {
        return s_cfg.mqtt_pass;
    }
    if (strcmp(key, "ent") == 0) {
        snprintf(buf, sizeof(buf), "%d", s_cfg.ent);
        return buf;
    }
    return "";
}

esp_err_t net_set(const char *key, const char *val) {
    if (!key || !val) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(key, "ssid") == 0) {
        strncpy(s_cfg.ssid, val, sizeof(s_cfg.ssid) - 1);
    } else if (strcmp(key, "user") == 0) {
        strncpy(s_cfg.user, val, sizeof(s_cfg.user) - 1);
    } else if (strcmp(key, "pass") == 0) {
        strncpy(s_cfg.pass, val, sizeof(s_cfg.pass) - 1);
    } else if (strcmp(key, "uri") == 0) {
        strncpy(s_cfg.uri, val, sizeof(s_cfg.uri) - 1);
    } else if (strcmp(key, "uri2") == 0) {
        strncpy(s_cfg.uri2, val, sizeof(s_cfg.uri2) - 1);
    } else if (strcmp(key, "prefix") == 0) {
        strncpy(s_cfg.prefix, val, sizeof(s_cfg.prefix) - 1);
    } else if (strcmp(key, "mqtt_user") == 0 || strcmp(key, "mqttuser") == 0) {
        strncpy(s_cfg.mqtt_user, val, sizeof(s_cfg.mqtt_user) - 1);
    } else if (strcmp(key, "mqtt_pass") == 0 || strcmp(key, "mqttpass") == 0) {
        strncpy(s_cfg.mqtt_pass, val, sizeof(s_cfg.mqtt_pass) - 1);
    } else if (strcmp(key, "ent") == 0) {
        s_cfg.ent = (val[0] == '1') ? 1 : 0;
    } else {
        return ESP_ERR_INVALID_ARG;
    }
    /* strncpy 在源串长度 >= n-1 时不会写 \0，这里统一补一次，
     * 否则 ssid 会一路读进 user[] 字段（后续 snprintf("%s") 越界读） */
    s_cfg.ssid[sizeof(s_cfg.ssid) - 1] = '\0';
    s_cfg.user[sizeof(s_cfg.user) - 1] = '\0';
    s_cfg.pass[sizeof(s_cfg.pass) - 1] = '\0';
    s_cfg.uri[sizeof(s_cfg.uri) - 1] = '\0';
    s_cfg.uri2[sizeof(s_cfg.uri2) - 1] = '\0';
    s_cfg.prefix[sizeof(s_cfg.prefix) - 1] = '\0';
    s_cfg.mqtt_user[sizeof(s_cfg.mqtt_user) - 1] = '\0';
    s_cfg.mqtt_pass[sizeof(s_cfg.mqtt_pass) - 1] = '\0';
    /* 改 ssid/pass/ent 时同步到多 WiFi 列表，保证"列表里一定包含当前这条" */
    if (s_cfg.ssid[0] != '\0' &&
        (strcmp(key, "ssid") == 0 || strcmp(key, "pass") == 0 || strcmp(key, "ent") == 0)) {
        (void)net_wifi_add_ex(s_cfg.ssid, s_cfg.pass, s_cfg.user, s_cfg.ent);
        int ai = net_wifi_active_index();
        if (ai >= 0) {
            s_wifi.wl[ai].ent = s_cfg.ent;
        }
    }
    s_cfg.has_cfg = (s_wifi.n > 0 || s_cfg.ssid[0] != '\0') ? 1 : 0;
    return ESP_OK;
}

/**
 * @brief 扫描附近 WiFi，回调输出
 * @param cb  每个 AP 调一次：cb(ssid, rssi, channel, authmode)
 * @param max 最多输出几个
 * @return 扫到的总数
 */
static int net_scan_impl(void (*cb)(const char *ssid, int rssi, int ch, int auth), int max) {
    if (!s_inited) {
        return -1;
    }
    if (!s_started) {
        /* 扫描需要 STA 已经启动 */
        if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) {
            return -1;
        }
        if (esp_wifi_start() != ESP_OK && !s_started) {
            return -1;
        }
        s_started = true;
    }

    /* 关键：不要为了扫描把正在用的 WiFi 断掉。
     * 旧实现无条件 esp_wifi_disconnect()，于是上位机每点一次"扫描 WiFi"，
     * 板子就会掉线重连 —— MQTT 断、手机 App 报设备离线、正在跑的计划看不到状态。
     * IDF 只在"正在连接"时才拒绝扫描（ESP_ERR_WIFI_STATE），已经连上是可以
     * 带连接扫描的（默认 120ms/信道，不会把链路扫断）。 */
    const bool was_connected = s_connected;
    if (!was_connected) {
        /* 可能正处于 connecting：必须先松开，否则 scan_start 直接返回 WIFI_STATE */
        (void)esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(300));
    }

    wifi_scan_config_t sc = {0};
    sc.show_hidden = true;
    esp_err_t sret = esp_wifi_scan_start(&sc, true);
    if (sret != ESP_OK && was_connected) {
        ESP_LOGW(TAG, "scan while connected failed (%s), falling back to disconnect+scan",
                 esp_err_to_name(sret));
        (void)esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(500));
        sret = esp_wifi_scan_start(&sc, true);
    }
    if (sret != ESP_OK) {
        ESP_LOGW(TAG, "scan_start failed: %s", esp_err_to_name(sret));
        if (was_connected && !s_connected) {
            s_want_reconnect = true;
        }
        return -1;
    }

    uint16_t count = 0;
    if (esp_wifi_scan_get_ap_num(&count) != ESP_OK) {
        return -1;
    }
    if (count == 0) {
        return 0;
    }
    if (count > 32) {
        count = 32;
    }

    wifi_ap_record_t *recs = calloc(count, sizeof(wifi_ap_record_t));
    if (!recs) {
        return -1;
    }
    uint16_t got = count;
    if (esp_wifi_scan_get_ap_records(&got, recs) != ESP_OK) {
        free(recs);
        return -1;
    }

    int shown = 0;
    for (uint16_t i = 0; i < got; ++i) {
        if (recs[i].ssid[0] == '\0') {
            continue;
        }
        if (cb && shown < max) {
            cb((const char *)recs[i].ssid, recs[i].rssi, recs[i].primary, (int)recs[i].authmode);
            shown++;
        }
    }
    ESP_LOGI(TAG, "scan done: %u APs", (unsigned)got);
    free(recs);

    /* 如果这一轮扫描把连接弄断了（或者本来就是断开态），立刻排队重连，
     * 不要让"扫描"变成一次断网。 */
    if (!s_connected) {
        s_want_reconnect = true;
    }
    return (int)got;
}

int net_scan(void (*cb)(const char *ssid, int rssi, int ch, int auth), int max) {
    if (s_net_busy) {
        /* 后台正在换 AP / 连另一个 SSID：等它一小会儿再扫，
         * 直接回"忙"会让上位机以为 WiFi 坏了。 */
        for (int i = 0; i < 20 && s_net_busy; i++) {
            vTaskDelay(pdMS_TO_TICKS(250));
        }
        if (s_net_busy) {
            return -2;
        }
    }
    s_net_busy = true;
    const int r = net_scan_impl(cb, max);
    s_net_busy = false;
    return r;
}

esp_err_t net_save(void) {
    nvs_handle_t h;
    esp_err_t ret = nvs_open(NET_NS, NVS_READWRITE, &h);
    if (ret != ESP_OK) {
        return ret;
    }
    s_wifi.ver = 1;
    ret = nvs_set_blob(h, NET_KEY, &s_cfg, sizeof(s_cfg));
    if (ret == ESP_OK) {
        ret = nvs_set_blob(h, NET_KEY_WIFI, &s_wifi, sizeof(s_wifi));
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(h);
    }
    nvs_close(h);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "wifi config saved");
    }
    return ret;
}

const char *net_summary(void) {
    snprintf(s_summ, sizeof(s_summ),
             "ssid=%s,ent=%d,connected=%d,ip=%s,uri=%s,prefix=%s,mqtt_user=%s",
             s_cfg.ssid[0] ? s_cfg.ssid : "<unset>", s_cfg.ent,
             s_connected ? 1 : 0, s_ip,
             s_cfg.uri, s_cfg.prefix, s_cfg.mqtt_user[0] ? s_cfg.mqtt_user : "-");
    return s_summ;
}
