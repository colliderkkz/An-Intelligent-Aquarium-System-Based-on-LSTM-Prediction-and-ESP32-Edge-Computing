#ifndef PH_SENSOR_ADC_H
#define PH_SENSOR_ADC_H

#include "esp_err.h"
#include "hal/adc_types.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 硬件通道：pH 模块模拟输出 -> GPIO1 (ADC1_CH0) */
#define PH_ADC_UNIT     ADC_UNIT_1
#define PH_ADC_CHANNEL  ADC_CHANNEL_0

esp_err_t ph_sensor_init(void);

/**
 * @brief 采样 pH 并给出原始值/电压（便于上位机标定）
 * @param temperature 当前水温（用于温度补偿，传 25 表示不补偿）
 * @param ph          输出 pH
 * @param raw         输出中值滤波后的 12bit raw（可为 NULL）
 * @param mv          输出对应的毫伏值（可为 NULL）
 */
esp_err_t ph_sensor_read_raw(float temperature, float *ph, int *raw, int *mv);

/**
 * @brief 由毫伏值换算 pH（纯函数，方便用标准液做两点标定/自检）
 */
float ph_sensor_convert_mv(int mv, float temperature);

/** 带校验的换算：越界（pH<0 或 >14）返回 ESP_ERR_INVALID_RESPONSE，
 *  out_ph 仍会带出原始换算值供日志排查。 */
esp_err_t ph_sensor_convert_checked(int mv, float temperature, float *out_ph);

/** 兼容旧接口 */
esp_err_t ph_sensor_read(float temperature, float *ph_value);

#ifdef __cplusplus
}
#endif

#endif // PH_SENSOR_ADC_H
