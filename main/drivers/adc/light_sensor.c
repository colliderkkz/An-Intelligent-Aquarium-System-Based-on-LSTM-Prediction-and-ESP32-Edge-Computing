#include "light_sensor.h"

#include "adc_manager.h"
#include "config/param_store.h"
#include "config/pin_config.h"
#include "esp_log.h"
#include <math.h>
#include <stdbool.h>

static const char *TAG = "LIGHT_SENSOR";

#define LIGHT_RETRY_COUNT     3

/*
 * 光敏模块的分压电路：
 *     VCC(3.3V) --- R_fixed --- AO --- Rs(光敏电阻) --- GND
 * 于是 AO = VCC * Rs / (R_fixed + Rs)，越亮 Rs 越小、AO 电压越低
 * （这就是原来"很亮的时候百分比反而很小"的原因）。
 * 反解：Rs = R_fixed * AO / (VCC - AO)
 * 光敏电阻按 GL5528 典型曲线近似：100 lux 时约 2kΩ。
 */
#define LIGHT_DIVIDER_R_OHM   10000.0f
#define LIGHT_VCC_MV          3300.0f
#define LIGHT_LUX_REF         100.0f
#define LIGHT_RS_REF_OHM      2000.0f
#define LIGHT_GAMMA           (-1.25f)
#define LIGHT_RS_MIN_OHM      300.0f
#define LIGHT_RS_MAX_OHM      2000000.0f
#define LIGHT_LUX_MIN         0.05f
#define LIGHT_LUX_MAX         2000.0f

static float s_last_percent = 0.0f;
static bool s_has_value = false;

esp_err_t light_sensor_init(void) {
#if LIGHT_SENSOR_ON_ADC2
    return adc_manager_config(LIGHT_ADC_UNIT, LIGHT_ADC_CHANNEL);
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

/**
 * @brief 把 ADC 毫伏换算成照度 lux
 *
 * 原来是把电压线性映射成 0~100%，既没有物理意义，方向还反了
 * （分压电路下越亮电压越低）。这里按光敏电阻的幂函数特性换算：
 *   Rs = R_fixed * AO / (VCC - AO)
 *   lux = 100 * (Rs / 2000)^(-1.25)
 */
float light_sensor_convert_mv(int mv) {
    if (mv <= 0) {
        return LIGHT_LUX_MAX;
    }
    if ((float)mv >= LIGHT_VCC_MV) {
        return LIGHT_LUX_MIN;
    }

    const float ao = (float)mv;
    float rs = LIGHT_DIVIDER_R_OHM * ao / (LIGHT_VCC_MV - ao);
    if (rs < LIGHT_RS_MIN_OHM) {
        rs = LIGHT_RS_MIN_OHM;
    }
    if (rs > LIGHT_RS_MAX_OHM) {
        rs = LIGHT_RS_MAX_OHM;
    }

    float lux = LIGHT_LUX_REF * powf(rs / LIGHT_RS_REF_OHM, LIGHT_GAMMA);
    if (lux < LIGHT_LUX_MIN) {
        lux = LIGHT_LUX_MIN;
    }
    if (lux > LIGHT_LUX_MAX) {
        lux = LIGHT_LUX_MAX;
    }
    return lux;
}

esp_err_t light_sensor_read_raw(float *percent, int *raw, int *mv) {
#if LIGHT_SENSOR_ON_ADC2
    int raw_val = 0;
    int mv_val = 0;
    esp_err_t ret = ESP_FAIL;

    /* ADC2 与 WiFi 共用仲裁，偶发 TIMEOUT/INVALID_STATE 属于正常现象，重试即可 */
    for (int attempt = 0; attempt < LIGHT_RETRY_COUNT; attempt++) {
        ret = adc_manager_read(LIGHT_ADC_UNIT, LIGHT_ADC_CHANNEL, &raw_val, &mv_val);
        if (ret == ESP_OK) {
            break;
        }
    }
    if (ret != ESP_OK) {
        return ret;
    }

    /* 贴轨 = 光敏模块未接入；此时读数没有意义，宁可报错让上层保持上次值 */
    if (raw_val < ADC_RAIL_LOW_RAW) {
        if (percent) {
            *percent = 0.0f;
        }
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (percent) {
        float lux = light_sensor_convert_mv(mv_val);
        const float scale = param_get(PARAM_IDX_LIGHT_LUX_SCALE);
        if (scale > 0.01f && scale < 100.0f) {
            lux *= scale;
        }
        *percent = lux;
    }
    if (raw) {
        *raw = raw_val;
    }
    if (mv) {
        *mv = mv_val;
    }
    return ESP_OK;
#else
    (void)percent;
    (void)raw;
    (void)mv;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t light_sensor_read(float *percent) {
    if (!percent) {
        return ESP_ERR_INVALID_ARG;
    }

    float value = 0.0f;
    esp_err_t ret = light_sensor_read_raw(&value, NULL, NULL);
    if (ret == ESP_OK) {
        s_last_percent = value;
        s_has_value = true;
        *percent = value;
        return ESP_OK;
    }

    /* ADC2 被 WiFi 占用时，宁可返回上一次的有效值，也不要让上层任务炸掉 */
    if (s_has_value) {
        ESP_LOGD(TAG, "light read failed (%s), keep last %.1f lux",
                 esp_err_to_name(ret), s_last_percent);
        *percent = s_last_percent;
    }
    return ret;
}
