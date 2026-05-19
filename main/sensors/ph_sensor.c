#include "sensors/ph_sensor.h"

#include "drivers/adc/ph_sensor_adc.h"
#include "sensors/temp_sensor.h"
#include <stdbool.h>

static bool s_initialized = false;
static float s_last_ph = 7.0f;

esp_err_t sensor_ph_init(void) {
    if (s_initialized) {
        return ESP_OK;
    }

    esp_err_t ret = sensor_temp_init();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = ph_sensor_init();
    if (ret == ESP_OK) {
        s_initialized = true;
    }
    return ret;
}

esp_err_t sensor_ph_read(float *ph_value) {
    if (!ph_value) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = sensor_ph_init();
    if (ret != ESP_OK) {
        return ret;
    }

    float temperature_c = sensor_temp_get_last_c();
    ret = sensor_temp_read_c(&temperature_c);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = ph_sensor_read(temperature_c, ph_value);
    if (ret == ESP_OK) {
        s_last_ph = *ph_value;
    }
    return ret;
}

float sensor_ph_get_last(void) {
    return s_last_ph;
}
