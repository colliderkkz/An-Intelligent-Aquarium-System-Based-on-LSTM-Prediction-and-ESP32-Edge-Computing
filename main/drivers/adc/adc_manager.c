#include "adc_manager.h"

#include "config/pin_config.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdbool.h>

static const char *TAG = "ADC_MANAGER";

static adc_oneshot_unit_handle_t s_adc1_handle = NULL;
static adc_cali_handle_t s_adc1_cali_handle = NULL;
static bool s_adc1_cali_enabled = false;
static bool s_adc_initialized = false;

static esp_err_t adc_manager_init_calibration(void) {
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_SENSOR,
        .atten = ADC_ATTEN,
        .bitwidth = ADC_WIDTH,
    };
    esp_err_t ret = adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_adc1_cali_handle);
    if (ret == ESP_OK) {
        s_adc1_cali_enabled = true;
    }
    return ret;
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_SENSOR,
        .atten = ADC_ATTEN,
        .bitwidth = ADC_WIDTH,
    };
    esp_err_t ret = adc_cali_create_scheme_line_fitting(&cali_cfg, &s_adc1_cali_handle);
    if (ret == ESP_OK) {
        s_adc1_cali_enabled = true;
    }
    return ret;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t adc_manager_init(void) {
    if (s_adc_initialized) {
        return ESP_OK;
    }

    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = ADC_UNIT_SENSOR,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };

    esp_err_t ret = adc_oneshot_new_unit(&unit_cfg, &s_adc1_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC unit init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = adc_manager_init_calibration();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "ADC calibration enabled");
    } else {
        s_adc1_cali_enabled = false;
        ESP_LOGW(TAG, "ADC calibration unavailable, fallback to raw conversion");
    }

    s_adc_initialized = true;
    return ESP_OK;
}

esp_err_t adc_manager_config_channel(adc_channel_t channel) {
    if (!s_adc_initialized) {
        esp_err_t ret = adc_manager_init();
        if (ret != ESP_OK) {
            return ret;
        }
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN,
        .bitwidth = ADC_WIDTH,
    };

    esp_err_t ret = adc_oneshot_config_channel(s_adc1_handle, channel, &chan_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC channel %d config failed: %s", (int)channel, esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t adc_manager_read_raw(adc_channel_t channel, int *out_val) {
    if (!out_val) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_adc_initialized) {
        esp_err_t ret = adc_manager_init();
        if (ret != ESP_OK) {
            return ret;
        }
    }

    int raw_sum = 0;
    for (int i = 0; i < ADC_SAMPLE_COUNT; i++) {
        int raw = 0;
        esp_err_t ret = adc_oneshot_read(s_adc1_handle, channel, &raw);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "ADC read failed on channel %d: %s", (int)channel, esp_err_to_name(ret));
            return ret;
        }
        raw_sum += raw;
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    *out_val = raw_sum / ADC_SAMPLE_COUNT;
    return ESP_OK;
}

esp_err_t adc_manager_read_voltage(adc_channel_t channel, int *out_mv) {
    if (!out_mv) {
        return ESP_ERR_INVALID_ARG;
    }

    int raw = 0;
    esp_err_t ret = adc_manager_read_raw(channel, &raw);
    if (ret != ESP_OK) {
        return ret;
    }

    if (s_adc1_cali_enabled) {
        return adc_cali_raw_to_voltage(s_adc1_cali_handle, raw, out_mv);
    }

    *out_mv = (raw * 3300) / 4095;
    return ESP_OK;
}
