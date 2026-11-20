#include "sensors/tds_sensor.h"

#include "drivers/adc/tds_sensor.h"
#include "service/sensor_hub.h"
#include <stdbool.h>

static bool s_initialized = false;
static float s_last_tds_ppm = 0.0f;

esp_err_t sensor_tds_init(void) {
    if (s_initialized) {
        return ESP_OK;
    }
    esp_err_t ret = tds_sensor_init();
    if (ret == ESP_OK) {
        s_initialized = true;
    }
    return ret;
}

esp_err_t sensor_tds_read_ppm(float *tds_ppm) {
    if (!tds_ppm) {
        return ESP_ERR_INVALID_ARG;
    }

    sensor_sample_t sample = {0};
    esp_err_t ret = sensor_hub_sample(SH_TDS, &sample);
    if (ret == ESP_OK) {
        s_last_tds_ppm = sample.value;
        *tds_ppm = sample.value;
    }
    return ret;
}

float sensor_tds_get_last_ppm(void) {
    const sensor_sample_t *s = sensor_hub_get(SH_TDS);
    if (s && s->valid) {
        return s->value;
    }
    return s_last_tds_ppm;
}
