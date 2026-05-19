#include "ph_sensor_adc.h"

#include "adc_manager.h"
#include "config/pin_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "PH_SENSOR";

static void sort_descending(int *arr, int len) {
    for (int i = 0; i < len; i++) {
        for (int k = i + 1; k < len; k++) {
            if (arr[i] < arr[k]) {
                int tmp = arr[i];
                arr[i] = arr[k];
                arr[k] = tmp;
            }
        }
    }
}

esp_err_t ph_sensor_init(void) {
    return adc_manager_config_channel(ADC1_CHANNEL_PH);
}

esp_err_t ph_sensor_read(float temperature, float *ph_value) {
    if (!ph_value) {
        return ESP_ERR_INVALID_ARG;
    }

    int samples[PH_MEDIAN_SAMPLE_COUNT] = {0};
    for (int i = 0; i < PH_MEDIAN_SAMPLE_COUNT; i++) {
        int raw = 0;
        esp_err_t ret = adc_manager_read_raw(ADC1_CHANNEL_PH, &raw);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read pH ADC raw value");
            return ret;
        }
        samples[i] = raw;
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    sort_descending(samples, PH_MEDIAN_SAMPLE_COUNT);
    int median_raw = samples[PH_MEDIAN_SAMPLE_COUNT / 2];

    if (temperature > PH_TEMP_COMP_HIGH) {
        median_raw += PH_TEMP_COMP_MAX_ADJ;
    } else if (temperature > PH_TEMP_COMP_THRESHOLD) {
        median_raw += (int)(PH_TEMP_COMP_MAX_ADJ * (temperature - PH_TEMP_COMP_THRESHOLD) /
                            (PH_TEMP_COMP_HIGH - PH_TEMP_COMP_THRESHOLD));
    }

    float voltage = (float)median_raw * PH_ADC_REFERENCE_VOLTAGE / (float)PH_ADC_RESOLUTION;
    float ph = PH_SLOPE * voltage + PH_OFFSET;
    if (ph > 14.1f) {
        ph = 14.1f;
    }
    if (ph < 0.0f) {
        ph = 0.0f;
    }

    *ph_value = ((float)((int)(ph * 10.0f + 0.5f))) / 10.0f;
    return ESP_OK;
}
