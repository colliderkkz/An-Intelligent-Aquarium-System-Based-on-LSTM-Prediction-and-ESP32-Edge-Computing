#include "sensors/turbidity_sensor.h"

#include "drivers/adc/turbidity.h"
#include "service/sensor_hub.h"
#include <stdbool.h>

static bool s_initialized = false;
static float s_last_turbidity_ntu = 0.0f;

esp_err_t sensor_turbidity_init(void) {
    if (s_initialized) {
        return ESP_OK;
    }
    esp_err_t ret = turbidity_init();
    if (ret == ESP_OK) {
        s_initialized = true;
    }
    return ret;
}

esp_err_t sensor_turbidity_read_ntu(float *ntu) {
    if (!ntu) {
        return ESP_ERR_INVALID_ARG;
    }

    sensor_sample_t sample = {0};
    esp_err_t ret = sensor_hub_sample(SH_TURBIDITY, &sample);
    if (ret == ESP_OK) {
        s_last_turbidity_ntu = sample.value;
        *ntu = sample.value;
    }
    return ret;
}

float sensor_turbidity_get_last_ntu(void) {
    const sensor_sample_t *s = sensor_hub_get(SH_TURBIDITY);
    if (s && s->valid) {
        return s->value;
    }
    return s_last_turbidity_ntu;
}
