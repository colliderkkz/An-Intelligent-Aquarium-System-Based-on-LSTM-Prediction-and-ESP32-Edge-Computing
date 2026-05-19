#include "sensors/light_sensor.h"

#include "drivers/adc/light_sensor.h"
#include <stdbool.h>

static bool s_initialized = false;
static float s_last_light_percent = 0.0f;

esp_err_t sensor_light_init(void) {
    if (s_initialized) {
        return ESP_OK;
    }

    esp_err_t ret = light_sensor_init();
    if (ret == ESP_OK) {
        s_initialized = true;
    }
    return ret;
}

esp_err_t sensor_light_read_percent(float *light_percent) {
    if (!light_percent) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = sensor_light_init();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = light_sensor_read(light_percent);
    if (ret == ESP_OK) {
        s_last_light_percent = *light_percent;
    }
    return ret;
}

float sensor_light_get_last_percent(void) {
    return s_last_light_percent;
}
