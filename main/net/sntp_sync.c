#include "net/sntp_sync.h"

#include "esp_log.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net/wifi_manager.h"
#include "service/time_service.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "SNTP_SYNC";

/* 北京时间 UTC+8。板子的 time_service 把"年月日时分秒"当作 epoch 存，
 * 所以这里先把 UTC 加 8 小时，再用 gmtime 拆成字段。 */
#define CST_OFFSET_S       (8 * 3600)
#define RESYNC_PERIOD_MS   (3600 * 1000)   /* 每小时重对齐一次 */
#define RETRY_PERIOD_MS    (60 * 1000)     /* 没对成功时每分钟再试 */

static bool s_started = false;
static bool s_ok = false;
static uint32_t s_last_epoch = 0;
static bool s_inited = false;
static char s_status[96] = "idle";

static void apply_time_t(time_t utc_now) {
    if (utc_now <= 1700000000L) {
        return;   /* 明显无效（大概是 1970），不采纳 */
    }

    const time_t local = utc_now + CST_OFFSET_S;
    struct tm tm_local;
    if (gmtime_r(&local, &tm_local) == NULL) {
        return;
    }

    pcf8563_time_t t = {
        .year = (uint16_t)(tm_local.tm_year + 1900),
        .month = (uint8_t)(tm_local.tm_mon + 1),
        .day = (uint8_t)tm_local.tm_mday,
        .hour = (uint8_t)tm_local.tm_hour,
        .minute = (uint8_t)tm_local.tm_min,
        .second = (uint8_t)tm_local.tm_sec,
        .weekday = (uint8_t)tm_local.tm_wday,
    };

    /* 复用 time_service_set：它会同时更新软件时钟、写 RTC 芯片、存 NVS。
     * 晶振坏了 RTC 也不会走，但 NVS 能让下次上电至少回到这个时刻。 */
    (void)time_service_set(&t);
    time_service_set_net_synced(true);

    s_last_epoch = (uint32_t)local;
    s_ok = true;
    snprintf(s_status, sizeof(s_status), "synced %04u-%02u-%02u %02u:%02u:%02u",
             t.year, t.month, t.day, t.hour, t.minute, t.second);
    ESP_LOGI(TAG, "time synced from network: %04u-%02u-%02u %02u:%02u:%02u (CST)",
             t.year, t.month, t.day, t.hour, t.minute, t.second);
}

/* esp_sntp 的同步通知回调（在 lwip 任务上下文） */
static void sync_cb(struct timeval *tv) {
    if (tv) {
        apply_time_t((time_t)tv->tv_sec);
    }
}

void sntp_sync_now(void) {
    if (!s_inited) {
        return;
    }
    esp_sntp_restart();
}

bool sntp_sync_ok(void) {
    return s_ok;
}

uint32_t sntp_sync_last_epoch(void) {
    return s_last_epoch;
}

const char *sntp_sync_status(void) {
    return s_status;
}

static void ntp_task(void *arg) {
    (void)arg;
    bool started_ntp = false;
    uint32_t last_sync_ms = 0;

    while (1) {
        const bool up = net_wifi_up();

        if (up && !started_ntp) {
            esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
            esp_sntp_setservername(0, "ntp.aliyun.com");
            esp_sntp_setservername(1, "cn.pool.ntp.org");
            esp_sntp_setservername(2, "pool.ntp.org");
            esp_sntp_init();
            started_ntp = true;
            snprintf(s_status, sizeof(s_status), "waiting for first sync");
            ESP_LOGI(TAG, "SNTP started (aliyun / cn.pool / pool)");
        }

        /* 已经对过时：每小时重新对齐一次 */
        if (started_ntp && s_ok) {
            const uint32_t ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
            if ((uint32_t)(ms - last_sync_ms) >= RESYNC_PERIOD_MS) {
                last_sync_ms = ms;
                ESP_LOGI(TAG, "periodic resync");
                esp_sntp_restart();
            }
        }

        /* WiFi 掉了或者还没对成功：一分钟后再试 */
        if (started_ntp && !s_ok) {
            const uint32_t ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
            if ((uint32_t)(ms - last_sync_ms) >= RETRY_PERIOD_MS) {
                last_sync_ms = ms;
                esp_sntp_restart();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

esp_err_t sntp_sync_start(void) {
    if (s_started) {
        return ESP_OK;
    }

    esp_sntp_set_time_sync_notification_cb(sync_cb);
    s_inited = true;

    if (xTaskCreate(ntp_task, "sntp_sync", 4096, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create sntp task");
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    ESP_LOGI(TAG, "sntp sync task started");
    return ESP_OK;
}
