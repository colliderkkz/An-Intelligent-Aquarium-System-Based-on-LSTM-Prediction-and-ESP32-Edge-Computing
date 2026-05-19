#include "turbidity.h"

#include "adc_manager.h"
#include "config/pin_config.h"
#include "esp_log.h"

static const char *TAG = "TURBIDITY";

// Based on the AZDM01 Arduino reference code.
static const float TURBIDITY_TEMP_REFERENCE = 25.0f;
static const float TURBIDITY_TEMP_COEFF = -0.0192f;
static const float TURBIDITY_NTU_SLOPE = -865.68f;
static const float TURBIDITY_NTU_OFFSET = 3347.19f;
static const float TURBIDITY_NTU_MIN = 0.0f;
static const float TURBIDITY_NTU_MAX = 3000.0f;

esp_err_t turbidity_init(void) {
    return adc_manager_config_channel(ADC1_CHANNEL_TURBIDITY);
}

esp_err_t turbidity_read(float *ntu) {
    return turbidity_read_compensated(TURBIDITY_TEMP_REFERENCE, ntu);
}

esp_err_t turbidity_read_compensated(float temperature_c, float *ntu) {
    if (!ntu) {
        return ESP_ERR_INVALID_ARG;
    }

    int voltage_mv = 0;
    esp_err_t ret = adc_manager_read_voltage(ADC1_CHANNEL_TURBIDITY, &voltage_mv);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read turbidity voltage");
        return ret;
    }

    float voltage = (float)voltage_mv / 1000.0f;
    float calibrated_voltage = TURBIDITY_TEMP_COEFF * (temperature_c - TURBIDITY_TEMP_REFERENCE) + voltage;
    float ntu_value = TURBIDITY_NTU_SLOPE * calibrated_voltage + TURBIDITY_NTU_OFFSET;

    if (ntu_value < TURBIDITY_NTU_MIN) {
        ntu_value = TURBIDITY_NTU_MIN;
    } else if (ntu_value > TURBIDITY_NTU_MAX) {
        ntu_value = TURBIDITY_NTU_MAX;
    }

    *ntu = ntu_value;
    return ESP_OK;
}
