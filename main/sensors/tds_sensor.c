#include "sensors/tds_sensor.h"

#include "drivers/adc/tds_sensor.h"
#include "sensors/temp_sensor.h"
#include <stdbool.h>

static bool s_initialized = false;
static float s_last_tds_ppm = 0.0f;

esp_err_t sensor_tds_init(void) {
    if (s_initialized) {
        return ESP_OK;
    }

    esp_err_t ret = sensor_temp_init();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = tds_sensor_init();
    if (ret == ESP_OK) {
        s_initialized = true;
    }
    return ret;
}

esp_err_t sensor_tds_read_ppm(float *tds_ppm) {
    if (!tds_ppm) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = sensor_tds_init();
    if (ret != ESP_OK) {
        return ret;
    }

    float temperature_c = sensor_temp_get_last_c();
    ret = sensor_temp_read_c(&temperature_c);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = tds_sensor_read(temperature_c, tds_ppm);
    if (ret == ESP_OK) {
        s_last_tds_ppm = *tds_ppm;
    }
    return ret;
}

float sensor_tds_get_last_ppm(void) {
    return s_last_tds_ppm;
}
