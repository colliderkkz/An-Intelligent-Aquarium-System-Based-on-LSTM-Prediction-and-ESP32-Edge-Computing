#include "control/water_control.h"

#include "actuators/water_pump.h"
#include "config/system_config.h"
#include "drivers/adc/adc_manager.h"
#include "drivers/adc/water_level.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdint.h>

static const char *TAG = "WATER_CTRL";

static bool s_initialized = false;
static bool s_is_refilling = false;
static float s_last_level_percent = 0.0f;
static TickType_t s_refill_start_tick = 0;
static TickType_t s_last_refill_end_tick = 0;

static uint32_t elapsed_seconds_since(TickType_t start_tick) {
    TickType_t now = xTaskGetTickCount();
    return (uint32_t)((now - start_tick) / configTICK_RATE_HZ);
}

esp_err_t water_control_init(void) {
    esp_err_t ret = adc_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC manager init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = water_level_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Water level sensor init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = water_pump_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Water refill pump init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_initialized = true;
    s_is_refilling = false;
    s_last_level_percent = 0.0f;
    s_refill_start_tick = 0;
    s_last_refill_end_tick = 0;

    ESP_LOGI(TAG, "Water control initialized");
    return ESP_OK;
}

esp_err_t water_control_update(void) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    float level_percent = 0.0f;
    esp_err_t ret = water_level_read(&level_percent);
    if (ret != ESP_OK) {
        if (s_is_refilling) {
            (void)water_pump_set_state(false);
            s_is_refilling = false;
            s_last_refill_end_tick = xTaskGetTickCount();
        }
        return ret;
    }

    s_last_level_percent = level_percent;

    if (s_is_refilling) {
        bool reached_target = level_percent >= WATER_LEVEL_MAX;
        bool timeout = elapsed_seconds_since(s_refill_start_tick) >= REFILL_MAX_DURATION;
        if (reached_target || timeout) {
            ret = water_pump_set_state(false);
            if (ret == ESP_OK) {
                s_is_refilling = false;
                s_last_refill_end_tick = xTaskGetTickCount();
                ESP_LOGI(TAG, "Refill stopped, level=%.1f%%, reason=%s",
                         level_percent, reached_target ? "target reached" : "timeout");
            }
            return ret;
        }
        return ESP_OK;
    }

    bool low_level = level_percent <= WATER_LEVEL_AUTO_REFILL;
    bool cooldown_done = (s_last_refill_end_tick == 0) ||
                         (elapsed_seconds_since(s_last_refill_end_tick) >= REFILL_COOLDOWN);

    if (low_level && cooldown_done) {
        ret = water_pump_set_state(true);
        if (ret == ESP_OK) {
            s_is_refilling = true;
            s_refill_start_tick = xTaskGetTickCount();
            ESP_LOGW(TAG, "Low water level %.1f%%, start refill pump", level_percent);
        }
        return ret;
    }

    return ESP_OK;
}

bool water_control_is_refilling(void) {
    return s_is_refilling;
}

float water_control_get_last_level_percent(void) {
    return s_last_level_percent;
}
