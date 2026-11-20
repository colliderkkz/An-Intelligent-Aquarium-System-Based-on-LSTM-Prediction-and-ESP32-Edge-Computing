#include "sensors/water_level.h"

#include "drivers/adc/water_level.h"
#include "service/sensor_hub.h"
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

    sensor_sample_t sample = {0};
    esp_err_t ret = sensor_hub_sample(SH_LEVEL, &sample);
    if (ret == ESP_OK) {
        s_last_level_percent = sample.value;
        *level_percent = sample.value;
    }
    return ret;
}

esp_err_t sensor_water_level_read_voltage(float *voltage_v) {
    if (!voltage_v) {
        return ESP_ERR_INVALID_ARG;
    }
    int mv = 0;
    esp_err_t ret = water_level_read_raw(NULL, NULL, &mv);
    if (ret == ESP_OK) {
        *voltage_v = (float)mv / 1000.0f;
    }
    return ret;
}

float sensor_water_level_get_last_percent(void) {
    const sensor_sample_t *s = sensor_hub_get(SH_LEVEL);
    if (s && s->valid) {
        return s->value;
    }
    return s_last_level_percent;
}
