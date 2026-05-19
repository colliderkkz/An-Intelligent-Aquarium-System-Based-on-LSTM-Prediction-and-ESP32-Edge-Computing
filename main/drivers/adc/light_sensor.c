#include "light_sensor.h"

#include "config/pin_config.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include <stdbool.h>

static const char *TAG = "LIGHT_SENSOR";

#define LIGHT_VOLTAGE_MIN_MV 0
#define LIGHT_VOLTAGE_MAX_MV 3300

static adc_oneshot_unit_handle_t s_adc2_handle = NULL;
static bool s_light_adc_initialized = false;

esp_err_t light_sensor_init(void) {
    if (!s_light_adc_initialized) {
        adc_oneshot_unit_init_cfg_t unit_cfg = {
            .unit_id = ADC_UNIT_LIGHT_SENSOR,
            .ulp_mode = ADC_ULP_MODE_DISABLE,
        };

        esp_err_t ret = adc_oneshot_new_unit(&unit_cfg, &s_adc2_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "ADC2 unit init failed: %s", esp_err_to_name(ret));
            return ret;
        }
        s_light_adc_initialized = true;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN,
        .bitwidth = ADC_WIDTH,
    };
    return adc_oneshot_config_channel(s_adc2_handle, ADC2_CHANNEL_LIGHT, &chan_cfg);
}

esp_err_t light_sensor_read(float *percent) {
    if (!percent) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_light_adc_initialized) {
        esp_err_t ret = light_sensor_init();
        if (ret != ESP_OK) {
            return ret;
        }
    }

    int raw = 0;
    esp_err_t ret = adc_oneshot_read(s_adc2_handle, ADC2_CHANNEL_LIGHT, &raw);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read light sensor raw value: %s", esp_err_to_name(ret));
        return ret;
    }

    int voltage_mv = (raw * 3300) / 4095;

    if (voltage_mv <= LIGHT_VOLTAGE_MIN_MV) {
        *percent = 0.0f;
    } else if (voltage_mv >= LIGHT_VOLTAGE_MAX_MV) {
        *percent = 100.0f;
    } else {
        *percent = (float)(voltage_mv - LIGHT_VOLTAGE_MIN_MV) * 100.0f /
                   (LIGHT_VOLTAGE_MAX_MV - LIGHT_VOLTAGE_MIN_MV);
    }

    return ESP_OK;
}
