#include "sensors/temp_sensor.h"

#include "drivers/ds18b20.h"
#include "service/sensor_hub.h"
#include <stdbool.h>

static bool s_initialized = false;
static float s_last_temperature_c = 25.0f;

/* 缓存新鲜度：超过该时间才主动触发一次采样 */
#define TEMP_CACHE_MAX_AGE_MS  5000

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

    /* 修掉旧实现的“每读一次温度就阻塞 800ms”：优先返回采集任务维护的缓存 */
    if (sensor_hub_is_fresh(SH_TEMP, TEMP_CACHE_MAX_AGE_MS)) {
        *temperature_c = sensor_hub_get(SH_TEMP)->value;
        s_last_temperature_c = *temperature_c;
        return ESP_OK;
    }

    sensor_sample_t sample = {0};
    esp_err_t ret = sensor_hub_sample(SH_TEMP, &sample);
    if (ret == ESP_OK) {
        *temperature_c = sample.value;
        s_last_temperature_c = *temperature_c;
    }
    return ret;
}

float sensor_temp_get_last_c(void) {
    return sensor_hub_temperature_or(s_last_temperature_c);
}
