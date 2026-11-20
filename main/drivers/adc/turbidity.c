#include "turbidity.h"

#include "adc_manager.h"
#include "config/param_store.h"
#include "config/pin_config.h"
#include "esp_log.h"

static const char *TAG = "TURBIDITY";

/* 厂家(AZDM01)参考公式：TU=-865.68*V+3347.19，V 单位为伏，
 * 温度补偿：V_cal = -0.0192*(T-25) + V。斜率和截距改由 param_store 提供，
 * 方便用标准液在现场做两点标定。 */

#define TURBIDITY_NTU_MIN   0.0f
#define TURBIDITY_NTU_MAX   3000.0f
#define TURBIDITY_TEMP_REF  25.0f
#define TURBIDITY_TEMP_COEF (-0.0192f)

esp_err_t turbidity_init(void) {
    return adc_manager_config(TURB_ADC_UNIT, TURB_ADC_CHANNEL);
}

float turbidity_convert_mv(int mv, float temperature_c) {
    if (!(temperature_c > -5.0f && temperature_c < 80.0f)) {
        temperature_c = TURBIDITY_TEMP_REF;
    }

    const float voltage = (float)mv / 1000.0f;
    const float calibrated = TURBIDITY_TEMP_COEF * (temperature_c - TURBIDITY_TEMP_REF) + voltage;
    float ntu = P(PARAM_IDX_TURB_SLOPE) * calibrated + P(PARAM_IDX_TURB_OFFSET);

    if (ntu < TURBIDITY_NTU_MIN) {
        ntu = TURBIDITY_NTU_MIN;
    } else if (ntu > TURBIDITY_NTU_MAX) {
        ntu = TURBIDITY_NTU_MAX;
    }
    return ntu;
}

esp_err_t turbidity_read_raw(float temperature_c, float *ntu, int *raw, int *mv) {
    int raw_val = 0;
    int mv_val = 0;
    esp_err_t ret = adc_manager_read(TURB_ADC_UNIT, TURB_ADC_CHANNEL, &raw_val, &mv_val);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Turbidity ADC read failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 贴轨 = 模块未接入/短路 */
    if (raw_val < ADC_RAIL_LOW_RAW || raw_val > ADC_RAIL_HIGH_RAW) {
        ESP_LOGD(TAG, "Turbidity sensor not connected? raw=%d mv=%d", raw_val, mv_val);
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (ntu) {
        *ntu = turbidity_convert_mv(mv_val, temperature_c);
    }
    if (raw) {
        *raw = raw_val;
    }
    if (mv) {
        *mv = mv_val;
    }
    return ESP_OK;
}

esp_err_t turbidity_read(float *ntu) {
    return turbidity_read_compensated(TURBIDITY_TEMP_REF, ntu);
}

esp_err_t turbidity_read_compensated(float temperature_c, float *ntu) {
    if (!ntu) {
        return ESP_ERR_INVALID_ARG;
    }
    return turbidity_read_raw(temperature_c, ntu, NULL, NULL);
}
