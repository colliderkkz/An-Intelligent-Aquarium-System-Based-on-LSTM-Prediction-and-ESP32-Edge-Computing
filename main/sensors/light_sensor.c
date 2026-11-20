#include "sensors/light_sensor.h"

#include "drivers/adc/light_sensor.h"
#include "service/sensor_hub.h"
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

    sensor_sample_t sample = {0};
    esp_err_t ret = sensor_hub_sample(SH_LIGHT, &sample);
    if (ret == ESP_OK) {
        s_last_light_percent = sample.value;
        *light_percent = sample.value;
    }
    return ret;
}

float sensor_light_get_last_percent(void) {
    const sensor_sample_t *s = sensor_hub_get(SH_LIGHT);
    if (s && s->valid) {
        return s->value;
    }
    return s_last_light_percent;
}
