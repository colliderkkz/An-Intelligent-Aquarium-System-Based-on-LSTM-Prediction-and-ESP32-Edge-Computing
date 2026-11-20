#include "control/feeding_control.h"

#include "actuators/feeder.h"
#include "config/error_codes.h"
#include "config/param_store.h"
#include "config/system_config.h"
#include "esp_log.h"
#include "service/control_mode.h"
#include <stdint.h>

static const char *TAG = "FEED_CTRL";

static uint32_t s_last_fed_key = 0;
static bool s_initialized = false;

static bool is_feeding_time_now(const pcf8563_time_t *t) {
    if (!t) {
        return false;
    }
    const int h1 = (int)P(PARAM_IDX_FEED1_H);
    const int m1 = (int)P(PARAM_IDX_FEED1_M);
    const int h2 = (int)P(PARAM_IDX_FEED2_H);
    const int m2 = (int)P(PARAM_IDX_FEED2_M);

    const bool match1 = (t->hour == h1) && (t->minute == m1);
    const bool match2 = (t->hour == h2) && (t->minute == m2);
    return match1 || match2;
}

esp_err_t feeding_control_init(void) {
    esp_err_t ret = feeder_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Feeder init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_last_fed_key = 0;
    s_initialized = true;
    ESP_LOGI(TAG, "Feeding control initialized");
    return ESP_OK;
}

esp_err_t feeding_control_update(const pcf8563_time_t *now) {
    if (!s_initialized || !now) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!control_mode_auto_enabled() ||
        !param_get_bool(PARAM_IDX_AUTO_FEEDER)) {
        return ESP_OK;
    }

    if (!is_feeding_time_now(now)) {
        return ESP_OK;
    }

    /* 用“年 + 年内分钟”做去重键，避免同一分钟内重复投喂 */
    const uint32_t minute_of_year =
        (uint32_t)((((uint32_t)now->month * 32u + now->day) * 24u + now->hour) * 60u + now->minute);
    const uint32_t key = (uint32_t)now->year * 600000u + minute_of_year;
    if (key == s_last_fed_key) {
        return ESP_OK;
    }

    esp_err_t ret = feeder_feed_now();
    if (ret == ESP_OK) {
        s_last_fed_key = key;
        ESP_LOGI(TAG, "Feeding triggered at %04u-%02u-%02u %02u:%02u",
                 now->year, now->month, now->day, now->hour, now->minute);
    }
    return ret;
}

void feeding_control_reset_schedule(void) {
    s_last_fed_key = 0;
}
