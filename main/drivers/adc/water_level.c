#include "water_level.h"

#include "adc_manager.h"
#include "config/param_store.h"
#include "config/pin_config.h"
#include "esp_log.h"

static const char *TAG = "WATER_LEVEL";

esp_err_t water_level_init(void) {
    return adc_manager_config(LEVEL_ADC_UNIT, LEVEL_ADC_CHANNEL);
}

float water_level_convert_mv(int mv) {
    const float min_mv = P(PARAM_IDX_LEVEL_MIN_MV);
    float max_mv = P(PARAM_IDX_LEVEL_MAX_MV);
    if (max_mv <= min_mv + 50.0f) {
        /* 标定非法时退回默认量程，避免除零 */
        max_mv = min_mv + 3000.0f;
    }

    float percent = ((float)mv - min_mv) * 100.0f / (max_mv - min_mv);
    if (percent < 0.0f) {
        percent = 0.0f;
    } else if (percent > 100.0f) {
        percent = 100.0f;
    }
    return percent;
}

esp_err_t water_level_read_raw(float *percent, int *raw, int *mv) {
    int raw_val = 0;
    int mv_val = 0;
    esp_err_t ret = adc_manager_read(LEVEL_ADC_UNIT, LEVEL_ADC_CHANNEL, &raw_val, &mv_val);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Water level ADC read failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 贴轨 = 水位传感器未接入/短路。这条很关键：否则 0V 会算成 0%，
     * 补水逻辑会以为缸里没水而一直抽水。 */
    if (raw_val < ADC_RAIL_LOW_RAW || raw_val > ADC_RAIL_HIGH_RAW) {
        ESP_LOGD(TAG, "Water level sensor not connected? raw=%d mv=%d", raw_val, mv_val);
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (percent) {
        *percent = water_level_convert_mv(mv_val);
    }
    if (raw) {
        *raw = raw_val;
    }
    if (mv) {
        *mv = mv_val;
    }
    return ESP_OK;
}

esp_err_t water_level_read_voltage(float *voltage_v) {
    if (!voltage_v) {
        return ESP_ERR_INVALID_ARG;
    }
    int mv_val = 0;
    esp_err_t ret = water_level_read_raw(NULL, NULL, &mv_val);
    if (ret == ESP_OK) {
        *voltage_v = (float)mv_val / 1000.0f;
    }
    return ret;
}

esp_err_t water_level_read(float *percent) {
    if (!percent) {
        return ESP_ERR_INVALID_ARG;
    }
    return water_level_read_raw(percent, NULL, NULL);
}
