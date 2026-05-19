#include "sensors/water_level.h"

#include "drivers/adc/water_level.h"
#include <stdbool.h>

static bool s_initialized = false;
static float s_last_level_percent = 0.0f;

esp_err_t sensor_water_level_init(void) {
    if (s_initialized) {
        return ESP_OK;
    }

    esp_err_t ret = water_level_init();
    if (ret == ESP_OK) {
        s_initialized = true;
    }
    return ret;
}

esp_err_t sensor_water_level_read_percent(float *level_percent) {
    if (!level_percent) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = sensor_water_level_init();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = water_level_read(level_percent);
    if (ret == ESP_OK) {
        s_last_level_percent = *level_percent;
    }
    return ret;
}

esp_err_t sensor_water_level_read_voltage(float *voltage_v) {
    esp_err_t ret = sensor_water_level_init();
    if (ret != ESP_OK) {
        return ret;
    }
    return water_level_read_voltage(voltage_v);
}

float sensor_water_level_get_last_percent(void) {
    return s_last_level_percent;
}
