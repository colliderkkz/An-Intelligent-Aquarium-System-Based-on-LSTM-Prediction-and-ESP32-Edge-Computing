#include "sensors/ph_sensor.h"

#include "drivers/adc/ph_sensor_adc.h"
#include "service/sensor_hub.h"
#include <stdbool.h>

static bool s_initialized = false;
static float s_last_ph = 7.0f;

esp_err_t sensor_ph_init(void) {
    if (s_initialized) {
        return ESP_OK;
    }
    esp_err_t ret = ph_sensor_init();
    if (ret == ESP_OK) {
        s_initialized = true;
    }
    return ret;
}

esp_err_t sensor_ph_read(float *ph_value) {
    if (!ph_value) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 温度补偿值取自采集缓存，不再在这里触发一次 800ms 的 DS18B20 转换 */
    sensor_sample_t sample = {0};
    esp_err_t ret = sensor_hub_sample(SH_PH, &sample);
    if (ret == ESP_OK) {
        s_last_ph = sample.value;
        *ph_value = sample.value;
    }
    return ret;
}

float sensor_ph_get_last(void) {
    const sensor_sample_t *s = sensor_hub_get(SH_PH);
    if (s && s->valid) {
        return s->value;
    }
    return s_last_ph;
}
