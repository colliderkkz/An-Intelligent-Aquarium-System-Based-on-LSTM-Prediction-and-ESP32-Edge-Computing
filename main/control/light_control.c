#include "control/light_control.h"

#include "actuators/co2_valve.h"
#include "actuators/light_actuator.h"
#include "config/system_config.h"
#include "esp_log.h"

static const char *TAG = "LIGHT_CTRL";

static bool s_initialized = false;
static bool s_last_should_on = false;
static bool s_has_state = false;

static bool is_light_in_on_window(uint8_t hour) {
    if (LIGHT_ON_HOUR == LIGHT_OFF_HOUR) {
        return false;
    }
    if (LIGHT_ON_HOUR < LIGHT_OFF_HOUR) {
        return (hour >= LIGHT_ON_HOUR) && (hour < LIGHT_OFF_HOUR);
    }
    return (hour >= LIGHT_ON_HOUR) || (hour < LIGHT_OFF_HOUR);
}

esp_err_t light_control_init(void) {
    esp_err_t ret = light_actuator_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Light actuator init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = co2_valve_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "CO2 valve init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_initialized = true;
    s_last_should_on = false;
    s_has_state = false;
    ESP_LOGI(TAG, "Light control initialized");
    return ESP_OK;
}

esp_err_t light_control_update(const pcf8563_time_t *now) {
    if (!s_initialized || !now) {
        return ESP_ERR_INVALID_ARG;
    }

    bool should_on = is_light_in_on_window(now->hour);
    if (s_has_state && (should_on == s_last_should_on)) {
        return ESP_OK;
    }

    esp_err_t ret = light_actuator_set_brightness(should_on ? (float)LIGHT_DAY_BRIGHTNESS : 0.0f);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = co2_valve_set_state(should_on ? CO2_DAY_MODE : CO2_NIGHT_MODE);
    if (ret != ESP_OK) {
        return ret;
    }

    s_last_should_on = should_on;
    s_has_state = true;
    ESP_LOGI(TAG, "Light %s and CO2 %s by schedule at %02d:%02d",
             should_on ? "ON" : "OFF",
             should_on ? "ON" : "OFF",
             now->hour, now->minute);
    return ESP_OK;
}

bool light_control_is_light_on(void) {
    return s_last_should_on;
}
