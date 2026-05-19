#include "sensors/turbidity_sensor.h"

#include "drivers/adc/turbidity.h"
#include "sensors/temp_sensor.h"
#include <stdbool.h>

static bool s_initialized = false;
static float s_last_turbidity_ntu = 0.0f;

esp_err_t sensor_turbidity_init(void) {
    if (s_initialized) {
        return ESP_OK;
    }

    esp_err_t ret = sensor_temp_init();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = turbidity_init();
    if (ret == ESP_OK) {
        s_initialized = true;
    }
    return ret;
}

esp_err_t sensor_turbidity_read_ntu(float *ntu) {
    if (!ntu) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = sensor_turbidity_init();
    if (ret != ESP_OK) {
        return ret;
    }

    float temperature_c = sensor_temp_get_last_c();
    ret = sensor_temp_read_c(&temperature_c);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = turbidity_read_compensated(temperature_c, ntu);
    if (ret == ESP_OK) {
        s_last_turbidity_ntu = *ntu;
    }
    return ret;
}

float sensor_turbidity_get_last_ntu(void) {
    return s_last_turbidity_ntu;
}
