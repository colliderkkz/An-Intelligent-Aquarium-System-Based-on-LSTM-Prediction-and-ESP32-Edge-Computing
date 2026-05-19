#include "tds_sensor.h"

#include "adc_manager.h"
#include "config/pin_config.h"
#include "esp_log.h"

static const char *TAG = "TDS_SENSOR";

esp_err_t tds_sensor_init(void) {
    return adc_manager_config_channel(ADC1_CHANNEL_TDS);
}

esp_err_t tds_sensor_read(float temperature, float *tds_value) {
    if (!tds_value) {
        return ESP_ERR_INVALID_ARG;
    }

    int voltage_mv = 0;
    esp_err_t ret = adc_manager_read_voltage(ADC1_CHANNEL_TDS, &voltage_mv);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read TDS voltage");
        return ret;
    }

    float voltage = (float)voltage_mv / 1000.0f;
    float compensation_coefficient = 1.0f + 0.02f * (temperature - 25.0f);
    float compensation_voltage = voltage / compensation_coefficient;

    *tds_value = (133.42f * compensation_voltage * compensation_voltage * compensation_voltage
                - 255.86f * compensation_voltage * compensation_voltage
                + 857.39f * compensation_voltage) * 0.5f;
    if (*tds_value < 0.0f) {
        *tds_value = 0.0f;
    }

    return ESP_OK;
}
