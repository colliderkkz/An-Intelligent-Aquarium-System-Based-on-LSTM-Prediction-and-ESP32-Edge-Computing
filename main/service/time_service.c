#include "service/time_service.h"

#include "config/pin_config.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "TIME";

/* 每 10s 巡检一次 RTC：既做漂移校正，也用来识别"晶振不起振"这种硬故障。
 * 之前是 60s，一旦 RTC 冻住，每 60s 就把软件时钟往回拨一次，时间会周期性倒退。 */
#define RTC_RESYNC_PERIOD_MS  10000u
#define RTC_DRIFT_TOLERANCE_S 2
/* 连续这么久 RTC 的 epoch 一点没变，就判定走时停止 */
#define RTC_FROZEN_MS         5000u

static i2c_bus_handle_t s_bus = NULL;
static bool s_rtc_ok = false;
static bool s_from_nvs = false;
static bool s_initialized = false;
/* RTC 冻结检测：芯片能读写但秒不走（晶振/负载电容问题） */
static bool s_rtc_frozen = false;
/* 时间是否来自网络（SNTP）。晶振坏掉时它就是唯一可靠来源 */
static bool s_net_synced = false;
static uint32_t s_last_rtc_epoch = 0;
static uint32_t s_last_rtc_change_ms = 0;

/* 软件时钟：s_epoch 是在 s_epoch_tick 时刻的“民用时间秒数”（不含时区换算） */
static uint32_t s_epoch = 0;
static uint32_t s_epoch_tick = 0;
static uint32_t s_last_poll_ms = 0;

/* ---------- 公历 <-> 天数（Howard Hinnant 算法，无需 libc/TZ） ---------- */
static int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= (m <= 2) ? 1 : 0;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const int mp = (int)m + ((m > 2) ? -3 : 9);
    const unsigned doy = (unsigned)((153 * mp + 2) / 5) + d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

static void civil_from_days(int64_t z, int *y, unsigned *m, unsigned *d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    const int yy = (int)yoe + (int)(era * 400);
    const unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    const unsigned mp = (5u * doy + 2u) / 153u;
    const unsigned dd = doy - (153u * mp + 2u) / 5u + 1u;
    const unsigned mm = (mp < 10u) ? (mp + 3u) : (mp - 9u);
    *y = yy + ((mm <= 2u) ? 1 : 0);
    *m = mm;
    *d = dd;
}

static uint32_t time_to_epoch(const pcf8563_time_t *t) {
    int64_t days = days_from_civil((int)t->year, t->month, t->day);
    int64_t secs = days * 86400 + (int64_t)t->hour * 3600 + (int64_t)t->minute * 60 + t->second;
    if (secs < 0) {
        secs = 0;
    }
    return (uint32_t)secs;
}

static void epoch_to_time(uint32_t epoch, pcf8563_time_t *t) {
    int64_t days = (int64_t)epoch / 86400;
    uint32_t rem = epoch % 86400u;

    int y = 0;
    unsigned m = 0;
    unsigned d = 0;
    civil_from_days(days, &y, &m, &d);

    t->year = (uint16_t)y;
    t->month = (uint8_t)m;
    t->day = (uint8_t)d;
    t->hour = (uint8_t)(rem / 3600u);
    t->minute = (uint8_t)((rem % 3600u) / 60u);
    t->second = (uint8_t)(rem % 60u);
    /* 0=周日 ... 6=周六，与 struct tm 的 tm_wday 一致 */
    t->weekday = (uint8_t)(((days + 4) % 7 + 7) % 7);
}

/* "Oct 28 2026" + "21:47:31" -> epoch，作为无 RTC 时的初始时间 */
static bool parse_build_time(uint32_t *epoch_out) {
    static const char *const months[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                           "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char mon[4] = {0};
    int day = 0;
    int year = 0;
    int hh = 0;
    int mm = 0;
    int ss = 0;

    if (sscanf(__DATE__, "%3s %d %d", mon, &day, &year) != 3) {
        return false;
    }
    if (sscanf(__TIME__, "%d:%d:%d", &hh, &mm, &ss) != 3) {
        return false;
    }

    int month = 0;
    for (int i = 0; i < 12; i++) {
        if (strcmp(mon, months[i]) == 0) {
            month = i + 1;
            break;
        }
    }
    if (month == 0) {
        return false;
    }

    pcf8563_time_t t = {
        .year = (uint16_t)year,
        .month = (uint8_t)month,
        .day = (uint8_t)day,
        .hour = (uint8_t)hh,
        .minute = (uint8_t)mm,
        .second = (uint8_t)ss,
    };
    *epoch_out = time_to_epoch(&t);
    return true;
}

static uint32_t tick_to_ms(uint32_t tick) {
    return (uint32_t)((uint64_t)tick * 1000u / configTICK_RATE_HZ);
}

static uint32_t now_ms(void) {
    return tick_to_ms((uint32_t)xTaskGetTickCount());
}

static void set_local_epoch(uint32_t epoch) {
    s_epoch = epoch;
    s_epoch_tick = (uint32_t)xTaskGetTickCount();
}

/* 时间落 NVS：这块板子的 RTC 备份电源没接通（实测电池正极到 VDD 断路），
 * 每次断电 PCF8563 都会置 VL=1、时间清零。把最后一次校准时间存进 NVS，
 * 上电时至少能恢复到"上次校时的那一刻"，而不是退回编译时间。 */
#define TIME_NVS_NS   "fishpz"
#define TIME_NVS_KEY  "epoch_save"

static void save_epoch_to_nvs(uint32_t epoch) {
    nvs_handle_t h;
    if (nvs_open(TIME_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (nvs_set_u32(h, TIME_NVS_KEY, epoch) == ESP_OK) {
        (void)nvs_commit(h);
    }
    nvs_close(h);
}

static bool load_epoch_from_nvs(uint32_t *out) {
    if (!out) {
        return false;
    }
    nvs_handle_t h;
    if (nvs_open(TIME_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    esp_err_t ret = nvs_get_u32(h, TIME_NVS_KEY, out);
    nvs_close(h);
    return ret == ESP_OK && *out > 1700000000u;   /* 2023 年以后才算有效 */
}

static esp_err_t sync_from_rtc(bool verbose) {
    if (!s_bus) {
        return ESP_ERR_INVALID_STATE;
    }

    pcf8563_time_t t = {0};
    esp_err_t ret = pcf8563_get_time(s_bus, PCF8563_I2C_ADDR, &t);
    if (ret != ESP_OK) {
        if (s_rtc_ok && verbose) {
            ESP_LOGW(TAG, "RTC lost (%s), switch to software clock", esp_err_to_name(ret));
        }
        s_rtc_ok = false;
        return ret;
    }

    s_rtc_ok = true;
    const uint32_t ep = time_to_epoch(&t);
    if (ep != s_last_rtc_epoch) {
        s_last_rtc_epoch = ep;
        s_last_rtc_change_ms = now_ms();
        s_rtc_frozen = false;
    }
    set_local_epoch(ep);
    if (verbose) {
        ESP_LOGI(TAG, "RTC synced: %04u-%02u-%02u %02u:%02u:%02u",
                 t.year, t.month, t.day, t.hour, t.minute, t.second);
    }
    return ESP_OK;
}

esp_err_t time_service_init(i2c_bus_handle_t bus) {
    s_bus = bus;
    s_rtc_ok = false;

    uint32_t epoch = 0;
    if (!parse_build_time(&epoch)) {
        /* 理论上不会失败；兜底 2026-01-01 00:00:00 */
        epoch = 1767225600u;
    }
    set_local_epoch(epoch);
    s_last_poll_ms = now_ms();

    if (s_bus) {
        (void)sync_from_rtc(true);
    }

    if (!s_rtc_ok) {
        uint32_t saved = 0;
        if (load_epoch_from_nvs(&saved)) {
            set_local_epoch(saved);
            s_from_nvs = true;
            ESP_LOGW(TAG, "RTC unusable (VL=1), restored last known time from NVS");
        }
    }

    s_initialized = true;
    ESP_LOGI(TAG, "time source: %s",
             s_rtc_ok ? "RTC" : (s_from_nvs ? "NVS(last calibrated)" : "SOFT(build time)"));
    return ESP_OK;
}

void time_service_poll(void) {
    if (!s_initialized) {
        return;
    }

    uint32_t ms = now_ms();
    if ((uint32_t)(ms - s_last_poll_ms) < RTC_RESYNC_PERIOD_MS) {
        return;
    }
    s_last_poll_ms = ms;

    if (!s_bus) {
        return;
    }

    pcf8563_time_t rtc_time = {0};
    if (pcf8563_get_time(s_bus, PCF8563_I2C_ADDR, &rtc_time) != ESP_OK) {
        if (s_rtc_ok) {
            ESP_LOGW(TAG, "RTC read failed, keep software clock");
        }
        s_rtc_ok = false;
        return;
    }

    uint32_t rtc_epoch = time_to_epoch(&rtc_time);

    /* ---- 关键：先看 RTC 这 10 秒里到底有没有前进 ---- */
    const bool advanced = (rtc_epoch != s_last_rtc_epoch);

    if (advanced) {
        s_last_rtc_epoch = rtc_epoch;
        s_last_rtc_change_ms = ms;
        if (s_rtc_frozen) {
            ESP_LOGI(TAG, "RTC ticking again");
        }
        s_rtc_frozen = false;
    } else if (!s_rtc_frozen && (uint32_t)(ms - s_last_rtc_change_ms) >= RTC_FROZEN_MS) {
        s_rtc_frozen = true;
        ESP_LOGW(TAG, "RTC is FROZEN (registers readable but seconds never advance) "
                      "- check 32.768kHz crystal / load caps");
    }

    /* 一轮轮询周期内 RTC 一分钟都没走，就绝不能拿它的值去校正软件时钟——
     * 否则每次 SNTP 刚对完时，下一秒就被这个冻结值拉回去（实测偏差 3~5 秒）。 */
    if (!advanced) {
        if (s_rtc_frozen) {
            s_rtc_ok = false;
        }
        return;
    }

    uint32_t local_epoch = time_service_epoch();
    int32_t diff = (int32_t)(rtc_epoch - local_epoch);

    if (!s_rtc_ok || diff > RTC_DRIFT_TOLERANCE_S || diff < -RTC_DRIFT_TOLERANCE_S) {
        set_local_epoch(rtc_epoch);
        if (!s_rtc_ok) {
            ESP_LOGI(TAG, "RTC recovered, resync");
        } else {
            ESP_LOGW(TAG, "clock drift %lds, corrected from RTC", (long)diff);
        }
        s_rtc_ok = true;
    }
}

uint32_t time_service_epoch(void) {
    if (!s_initialized) {
        return s_epoch;
    }
    uint32_t ticks_now = (uint32_t)xTaskGetTickCount();
    uint32_t elapsed_s = (ticks_now - s_epoch_tick) / configTICK_RATE_HZ;
    return s_epoch + elapsed_s;
}

esp_err_t time_service_now(pcf8563_time_t *out) {
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    epoch_to_time(time_service_epoch(), out);
    return ESP_OK;
}

esp_err_t time_service_set(const pcf8563_time_t *t) {
    if (!t) {
        return ESP_ERR_INVALID_ARG;
    }
    if (t->month < 1 || t->month > 12 || t->day < 1 || t->day > 31 ||
        t->hour > 23 || t->minute > 59 || t->second > 59) {
        return ESP_ERR_INVALID_ARG;
    }

    set_local_epoch(time_to_epoch(t));

    if (s_bus) {
        esp_err_t ret = pcf8563_set_time(s_bus, PCF8563_I2C_ADDR, t);
        if (ret == ESP_OK) {
            s_rtc_ok = true;
            s_from_nvs = false;
            s_rtc_frozen = false;
            s_last_rtc_epoch = time_to_epoch(t);
            s_last_rtc_change_ms = now_ms();
            ESP_LOGI(TAG, "RTC set to %04u-%02u-%02u %02u:%02u:%02u",
                     t->year, t->month, t->day, t->hour, t->minute, t->second);
        } else {
            /* RTC 芯片写不进（这块板子备份电源断路），但软件时钟已经更新，
             * 照样存 NVS，重启后还能恢复到这次校准的时刻。 */
            s_rtc_ok = false;
            s_from_nvs = true;
            ESP_LOGW(TAG, "RTC write failed (%s), keep time in NVS only",
                     esp_err_to_name(ret));
        }
        save_epoch_to_nvs(time_to_epoch(t));
        return ret;
    }

    return ESP_OK;
}

bool time_service_rtc_ok(void) {
    return s_rtc_ok;
}

const char *time_service_source(void) {
    if (s_rtc_ok && !s_rtc_frozen) {
        return "RTC";
    }
    if (s_net_synced) {
        return "SNTP";        /* 网络对时，最可靠 */
    }
    if (s_rtc_frozen) {
        return "RTC-FROZEN";  /* 芯片不走时，只能用软件时钟 */
    }
    return s_from_nvs ? "NVS" : "SOFT";
}

void time_service_set_net_synced(bool on) {
    s_net_synced = on;
}

bool time_service_net_synced(void) {
    return s_net_synced;
}

bool time_service_rtc_frozen(void) {
    return s_rtc_frozen;
}

void time_service_format(const pcf8563_time_t *t, char *buf, size_t len) {
    if (!t || !buf || len == 0) {
        return;
    }
    snprintf(buf, len, "%04u-%02u-%02u %02u:%02u:%02u",
             t->year, t->month, t->day, t->hour, t->minute, t->second);
}
