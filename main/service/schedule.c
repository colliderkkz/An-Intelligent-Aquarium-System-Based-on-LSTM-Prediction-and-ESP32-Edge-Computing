#include "schedule.h"

#include "actuators/air_pump.h"
#include "actuators/co2_valve.h"
#include "actuators/light_actuator.h"
#include "actuators/pump.h"
#include "actuators/uv_lamp.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "service/control_mode.h"
#include "service/act_hold.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "SCHEDULE";

#define SCH_NS      "fishpz"
#define SCH_KEY     "sched"
#define SCH_MAGIC   0x53434844u /* 'SCHD' */
#define SCH_VERSION 1u

#define MINUTES_PER_DAY 1440

typedef struct {
    uint32_t magic;
    uint8_t version;
    uint8_t count;
    sched_entry_t items[SCH_COUNT];
    uint32_t crc;
} sched_blob_t;

static sched_entry_t s_items[SCH_COUNT];
/* 上一次实际下发的强度，-1 表示"还没下发过/该路未启用" */
static int s_last_pct[SCH_COUNT];
/* 上一轮是否处于"计划接管"状态，用于检测关闭边沿 */
static bool s_was_enabled[SCH_COUNT];


static const char *const s_names[SCH_COUNT] = {
    "light", "air", "pump", "uv", "co2"
};


/* 大小写无关比较（MSVC 没有 strcasecmp，newlib 有 strcasecmp 但不在 <string.h>） */
static bool str_eq_ci(const char *a, const char *b) {
    if (!a || !b) {
        return false;
    }
    while (*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + ('a' - 'A')) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + ('a' - 'A')) : *b;
        if (ca != cb) {
            return false;
        }
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

/* FNV-1a，够用且不依赖 esp_crc */
static uint32_t checksum(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

static void set_defaults(void) {
    memset(s_items, 0, sizeof(s_items));
    for (int i = 0; i < SCH_COUNT; ++i) {
        s_items[i].enabled = 0;
        s_items[i].percent = 100;   /* 默认满档 */
        s_items[i].start_min = 0;
        s_items[i].dur_min = 60;
        s_last_pct[i] = -1;
    }
    /* 给水草灯一个可直接用的默认值：08:00 开 10 小时 100% */
    s_items[SCH_LIGHT].start_min = 8 * 60;
    s_items[SCH_LIGHT].dur_min = 10 * 60;
}

int schedule_count(void) {
    return SCH_COUNT;
}

const char *schedule_name(int idx) {
    if (idx < 0 || idx >= SCH_COUNT) {
        return "?";
    }
    return s_names[idx];
}

bool schedule_find(const char *name, int *out_idx) {
    if (!name || !out_idx) {
        return false;
    }
    for (int i = 0; i < SCH_COUNT; ++i) {
        if (str_eq_ci(name, s_names[i])) {
            *out_idx = i;
            return true;
        }
    }
    return false;
}

const sched_entry_t *schedule_get(int idx) {
    if (idx < 0 || idx >= SCH_COUNT) {
        return NULL;
    }
    return &s_items[idx];
}

esp_err_t schedule_set(int idx, const sched_entry_t *e) {
    if (idx < 0 || idx >= SCH_COUNT || !e) {
        return ESP_ERR_INVALID_ARG;
    }
    s_items[idx].enabled = e->enabled ? 1 : 0;
    s_items[idx].percent = e->percent > 100 ? 100 : e->percent;
    s_items[idx].start_min = e->start_min >= MINUTES_PER_DAY ? 0 : e->start_min;
    s_items[idx].dur_min = (e->dur_min == 0) ? 1 : (e->dur_min > MINUTES_PER_DAY ? MINUTES_PER_DAY : e->dur_min);
    /* 紫外灯如果被写成 00:00->00:00 就是连开一整天（1440 分钟），
     * 对鱼和水质都不合适；单条计划硬性限到 180 分钟。 */
    if (schedule_name(idx) && strcmp(schedule_name(idx), "uv") == 0 &&
        s_items[idx].dur_min > 180) {
        s_items[idx].dur_min = 180;
    }
    s_last_pct[idx] = -1;   /* 强制下次 update 重新下发 */
    return ESP_OK;
}

void schedule_reset_all(void) {
    set_defaults();
}

bool schedule_is_active(int idx) {
    if (idx < 0 || idx >= SCH_COUNT) {
        return false;
    }
    return s_items[idx].enabled != 0;
}

/** 当前分钟是否落在 [start, start+dur) 窗口内（支持跨午夜） */
static bool in_window(uint16_t now_min, uint16_t start, uint16_t dur) {
    const uint32_t end = (uint32_t)start + dur;
    if (end <= MINUTES_PER_DAY) {
        return now_min >= start && now_min < end;
    }
    const uint16_t wrapped = (uint16_t)(end - MINUTES_PER_DAY);
    return now_min >= start || now_min < wrapped;
}

int schedule_desired_percent(int idx, const pcf8563_time_t *now) {
    if (idx < 0 || idx >= SCH_COUNT || !now) {
        return -1;
    }
    const sched_entry_t *e = &s_items[idx];
    if (!e->enabled) {
        return -1;
    }
    const uint16_t now_min = (uint16_t)((uint16_t)now->hour * 60u + now->minute);
    return in_window(now_min, e->start_min, e->dur_min) ? (int)e->percent : 0;
}

void schedule_window_info(int idx, const pcf8563_time_t *now,
                          bool *out_in_window, uint16_t *remain_min,
                          uint16_t *until_start_min) {
    bool win = false;
    uint16_t remain = 0;
    uint16_t until = 0;

    if (idx >= 0 && idx < SCH_COUNT && now && s_items[idx].enabled) {
        const uint16_t now_min = (uint16_t)((uint16_t)now->hour * 60u + now->minute);
        const uint16_t start = s_items[idx].start_min;
        const uint16_t dur = s_items[idx].dur_min;

        if (in_window(now_min, start, dur)) {
            win = true;
            const uint32_t end = (uint32_t)start + dur;   /* 可能超过一天 */
            if (end <= MINUTES_PER_DAY) {
                remain = (uint16_t)(end - now_min);
            } else {
                /* 跨午夜：先算到 24:00，再加第二天剩余 */
                remain = (uint16_t)(MINUTES_PER_DAY - now_min + (end - MINUTES_PER_DAY));
            }
        } else {
            until = (uint16_t)((start + MINUTES_PER_DAY - now_min) % MINUTES_PER_DAY);
        }
    }

    if (out_in_window) {
        *out_in_window = win;
    }
    if (remain_min) {
        *remain_min = remain;
    }
    if (until_start_min) {
        *until_start_min = until;
    }
}

void schedule_format_minutes(uint16_t minutes, char *buf, size_t len) {
    if (!buf || len == 0) {
        return;
    }
    const uint16_t m = minutes % MINUTES_PER_DAY;
    snprintf(buf, len, "%02u:%02u", (unsigned)(m / 60u), (unsigned)(m % 60u));
}

static esp_err_t apply_entry(int idx, uint8_t pct) {
    switch (idx) {
        case SCH_LIGHT:
            return light_actuator_set_brightness((float)pct);
        case SCH_AIR:
            return air_pump_set_state(pct > 0);
        case SCH_PUMP:
            return pump_set_state(pct > 0);
        case SCH_UV:
            return uv_lamp_set_state(pct > 0);
        case SCH_CO2:
            return co2_valve_set_state(pct > 0);
        default:
            return ESP_ERR_INVALID_ARG;
    }
}

/* ---------------- 按执行器独立的手动保持 ---------------- */

void schedule_hold_manual(const char *name, uint32_t seconds) {
    act_hold_set(name, seconds);
}

bool schedule_is_held(int idx) {
    if (idx < 0 || idx >= SCH_COUNT) {
        return false;
    }
    return act_hold_active(s_names[idx]);
}

uint32_t schedule_hold_left_s(int idx) {
    if (idx < 0 || idx >= SCH_COUNT) {
        return 0;
    }
    return act_hold_left_s(s_names[idx]);
}

uint32_t schedule_hold_max_left_s(void) {
    return act_hold_max_left_s();
}

void schedule_held_names(char *buf, size_t len) {
    act_hold_names(buf, (uint32_t)len);
}

void schedule_update(const pcf8563_time_t *now) {
    if (!now) {
        return;
    }
    /* 两种暂停：
     *   1) 显式进入"手动模式"（MODE 命令）-> 全部计划暂停
     *   2) 用户单独点了某个执行器 -> 只暂停它自己的计划
     * 温控/补水/喂食不在这里，它们永远不受影响。 */
    const bool global_manual = control_mode_is_manual();

    for (int i = 0; i < SCH_COUNT; ++i) {
        if (!s_items[i].enabled) {
            /* BUGFIX：计划从"启用"变"禁用"时，原来只清状态不下发"关"，
             * 于是执行器会永远停在最后那个强度上（比如灯常亮）。
             * 这里检测到关闭边沿就补发一次 0%。 */
            if (s_was_enabled[i]) {
                (void)apply_entry(i, 0);
                s_was_enabled[i] = false;
                ESP_LOGI(TAG, "%s schedule disabled -> output 0%%", s_names[i]);
            }
            s_last_pct[i] = -1;
            continue;
        }
        s_was_enabled[i] = true;
        if (global_manual || schedule_is_held(i)) {
            s_last_pct[i] = -1;   /* 恢复后强制重新下发 */
            continue;
        }

        const int want = schedule_desired_percent(i, now);
        if (want < 0 || s_last_pct[i] == want) {
            continue;
        }
        if (apply_entry(i, (uint8_t)want) == ESP_OK) {
            s_last_pct[i] = want;
            char tbuf[8];
            schedule_format_minutes(s_items[i].start_min, tbuf, sizeof(tbuf));
            ESP_LOGI(TAG, "%s schedule: %d%% (window %s +%umin, now %02u:%02u)",
                     s_names[i], want, tbuf, (unsigned)s_items[i].dur_min,
                     (unsigned)now->hour, (unsigned)now->minute);
        }
    }
}

esp_err_t schedule_load(void) {
    nvs_handle_t h;
    esp_err_t ret = nvs_open(SCH_NS, NVS_READONLY, &h);
    if (ret != ESP_OK) {
        set_defaults();
        return ret;
    }

    sched_blob_t blob;
    size_t len = sizeof(blob);
    ret = nvs_get_blob(h, SCH_KEY, &blob, &len);
    nvs_close(h);

    if (ret != ESP_OK || len != sizeof(blob) ||
        blob.magic != SCH_MAGIC || blob.version != SCH_VERSION ||
        blob.count != (uint8_t)SCH_COUNT ||
        blob.crc != checksum(&blob, offsetof(sched_blob_t, crc))) {
        set_defaults();
        if (ret == ESP_OK) {
            ESP_LOGW(TAG, "schedule blob invalid, using defaults");
            ret = ESP_ERR_INVALID_CRC;
        }
        return ret;
    }

    memcpy(s_items, blob.items, sizeof(s_items));
    for (int i = 0; i < SCH_COUNT; ++i) {
        s_last_pct[i] = -1;
    }
    ESP_LOGI(TAG, "schedule loaded (%d entries)", SCH_COUNT);
    return ESP_OK;
}

esp_err_t schedule_save(void) {
    sched_blob_t blob;
    memset(&blob, 0, sizeof(blob));
    blob.magic = SCH_MAGIC;
    blob.version = SCH_VERSION;
    blob.count = (uint8_t)SCH_COUNT;
    memcpy(blob.items, s_items, sizeof(s_items));
    blob.crc = checksum(&blob, offsetof(sched_blob_t, crc));

    nvs_handle_t h;
    esp_err_t ret = nvs_open(SCH_NS, NVS_READWRITE, &h);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = nvs_set_blob(h, SCH_KEY, &blob, sizeof(blob));
    if (ret == ESP_OK) {
        ret = nvs_commit(h);
    }
    nvs_close(h);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "schedule saved to NVS");
    }
    return ret;
}

esp_err_t schedule_init(void) {
    if (sizeof(sched_blob_t) > 512) {
        return ESP_ERR_INVALID_SIZE;
    }
    set_defaults();
    esp_err_t ret = schedule_load();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "schedule load: %s (defaults applied)", esp_err_to_name(ret));
    }
    return ESP_OK;
}
