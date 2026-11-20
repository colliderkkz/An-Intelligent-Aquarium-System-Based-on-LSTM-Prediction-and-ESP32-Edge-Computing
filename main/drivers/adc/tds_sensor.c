#include "tds_sensor.h"

#include "adc_manager.h"
#include "config/param_store.h"
#include "config/pin_config.h"
#include "config/system_config.h"
#include "esp_log.h"

static const char *TAG = "TDS_SENSOR";

esp_err_t tds_sensor_init(void) {
    return adc_manager_config(TDS_ADC_UNIT, TDS_ADC_CHANNEL);
}

float tds_sensor_convert_mv(int mv, float temperature) {
    /* 水温异常（例如 DS18B20 掉线返回 -127）时退回 25℃，避免补偿系数变成负数 */
    if (!(temperature > -5.0f && temperature < 80.0f)) {
        temperature = 25.0f;
    }

    const float voltage = (float)mv / 1000.0f;
    const float comp = 1.0f + TDS_TEMP_COEFFICIENT * (temperature - 25.0f);
    const float v = voltage / comp;

    float tds = (133.42f * v * v * v - 255.86f * v * v + 857.39f * v) * 0.5f;
    tds *= P(PARAM_IDX_TDS_K);

    if (tds < 0.0f) {
        tds = 0.0f;
    }
    return tds;
}

esp_err_t tds_sensor_read_raw(float temperature, float *tds_ppm, int *raw, int *mv) {
    int raw_val = 0;
    int mv_val = 0;
    esp_err_t ret = adc_manager_read(TDS_ADC_UNIT, TDS_ADC_CHANNEL, &raw_val, &mv_val);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TDS ADC read failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 贴轨 = 模块未接入/短路，别把 0V 或 3.3V 算成 8000+ ppm */
    if (raw_val < ADC_RAIL_LOW_RAW || raw_val > ADC_RAIL_HIGH_RAW) {
        ESP_LOGD(TAG, "TDS sensor not connected? raw=%d mv=%d", raw_val, mv_val);
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (tds_ppm) {
        *tds_ppm = tds_sensor_convert_mv(mv_val, temperature);
    }
    if (raw) {
        *raw = raw_val;
    }
    if (mv) {
        *mv = mv_val;
    }
    return ESP_OK;
}

esp_err_t tds_sensor_read(float temperature, float *tds_value) {
    if (!tds_value) {
        return ESP_ERR_INVALID_ARG;
    }
    return tds_sensor_read_raw(temperature, tds_value, NULL, NULL);
}
