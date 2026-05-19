#include "sensors/temp_sensor.h"

#include "drivers/ds18b20.h"
#include <stdbool.h>

static bool s_initialized = false;
static float s_last_temperature_c = 25.0f;

esp_err_t sensor_temp_init(void) {
    if (s_initialized) {
        return ESP_OK;
    }

    esp_err_t ret = ds18b20_init();
    if (ret == ESP_OK) {
        s_initialized = true;
    }
    return ret;
}

esp_err_t sensor_temp_read_c(float *temperature_c) {
    if (!temperature_c) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = sensor_temp_init();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = ds18b20_read_temperature_c(temperature_c);
    if (ret == ESP_OK) {
        s_last_temperature_c = *temperature_c;
    }
    return ret;
}

float sensor_temp_get_last_c(void) {
    return s_last_temperature_c;
}
