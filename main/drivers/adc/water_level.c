#include "water_level.h"

#include "adc_manager.h"
#include "config/pin_config.h"
#include "esp_log.h"

static const char *TAG = "WATER_LEVEL";

// Based on the vendor sample: treat the sensor as a calibrated analog voltage source.
static const float WATER_LEVEL_SENSOR_VOLTAGE_MIN = 0.0f;
static const float WATER_LEVEL_SENSOR_VOLTAGE_MAX = 3.0f;

esp_err_t water_level_init(void) {
    return adc_manager_config_channel(ADC1_CHANNEL_WATER_LEVEL);
}

esp_err_t water_level_read_voltage(float *voltage_v) {
    if (!voltage_v) {
        return ESP_ERR_INVALID_ARG;
    }

    int voltage_mv = 0;
    esp_err_t ret = adc_manager_read_voltage(ADC1_CHANNEL_WATER_LEVEL, &voltage_mv);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read water level voltage");
        return ret;
    }

    *voltage_v = (float)voltage_mv / 1000.0f;
    return ESP_OK;
}

esp_err_t water_level_read(float *percent) {
    if (!percent) {
        return ESP_ERR_INVALID_ARG;
    }

    float voltage_v = 0.0f;
    esp_err_t ret = water_level_read_voltage(&voltage_v);
    if (ret != ESP_OK) {
        return ret;
    }

    if (voltage_v <= WATER_LEVEL_SENSOR_VOLTAGE_MIN) {
        *percent = 0.0f;
    } else if (voltage_v >= WATER_LEVEL_SENSOR_VOLTAGE_MAX) {
        *percent = 100.0f;
    } else {
        *percent = (voltage_v - WATER_LEVEL_SENSOR_VOLTAGE_MIN) * 100.0f /
                   (WATER_LEVEL_SENSOR_VOLTAGE_MAX - WATER_LEVEL_SENSOR_VOLTAGE_MIN);
    }

    return ESP_OK;
}
