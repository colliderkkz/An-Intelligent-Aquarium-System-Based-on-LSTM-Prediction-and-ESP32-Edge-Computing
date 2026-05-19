#include "control/temp_control.h"

#include "actuators/heater.h"
#include "config/system_config.h"
#include "drivers/ds18b20.h"
#include "esp_log.h"

static const char *TAG = "TEMP_CTRL";

static bool s_initialized = false;
static float s_last_temperature_c = 0.0f;

esp_err_t temp_control_init(void) {
    esp_err_t ret = heater_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Heater init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = ds18b20_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "DS18B20 init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_initialized = true;
    s_last_temperature_c = TEMP_TARGET;
    ESP_LOGI(TAG, "Temperature control initialized");
    return ESP_OK;
}

esp_err_t temp_control_update(void) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    float temperature_c = 0.0f;
    esp_err_t ret = ds18b20_read_temperature_c(&temperature_c);
    if (ret != ESP_OK) {
        return ret;
    }

    s_last_temperature_c = temperature_c;

    const bool heater_on = heater_get_state();
    bool should_heat = heater_on;

    if (temperature_c <= (TEMP_TARGET - HEATER_HYSTERESIS)) {
        should_heat = true;
    } else if (temperature_c >= (TEMP_TARGET + HEATER_HYSTERESIS)) {
        should_heat = false;
    }

    if (should_heat != heater_on) {
        ret = heater_set_state(should_heat);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Temperature %.2f C, heater %s",
                     temperature_c, should_heat ? "ON" : "OFF");
        }
        return ret;
    }

    return ESP_OK;
}

float temp_control_get_last_temperature_c(void) {
    return s_last_temperature_c;
}

bool temp_control_is_heating(void) {
    return heater_get_state();
}
