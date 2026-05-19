#include "control/feeding_control.h"

#include "actuators/feeder.h"
#include "config/system_config.h"
#include "esp_log.h"
#include <stdint.h>

static const char *TAG = "FEED_CTRL";

static uint32_t s_last_fed_key = 0;
static bool s_initialized = false;

static bool is_feeding_time_now(const pcf8563_time_t *t) {
    if (!t) {
        return false;
    }

    const bool match1 = (t->hour == FEEDING_TIME_1_HOUR) && (t->minute == FEEDING_TIME_1_MINUTE);
    const bool match2 = (t->hour == FEEDING_TIME_2_HOUR) && (t->minute == FEEDING_TIME_2_MINUTE);
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

    if (!is_feeding_time_now(now)) {
        return ESP_OK;
    }

    uint32_t minute_of_year =
        (uint32_t)((((now->month * 32u + now->day) * 24u + now->hour) * 60u) + now->minute);
    uint32_t key = (uint32_t)now->year * 600000u + minute_of_year;
    if (key == s_last_fed_key) {
        return ESP_OK;
    }

    esp_err_t ret = feeder_feed_now();
    if (ret == ESP_OK) {
        s_last_fed_key = key;
        ESP_LOGI(TAG, "Feeding triggered at %04d-%02d-%02d %02d:%02d",
                 now->year, now->month, now->day, now->hour, now->minute);
    }
    return ret;
}
