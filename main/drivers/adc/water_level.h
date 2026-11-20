#ifndef WATER_LEVEL_H
#define WATER_LEVEL_H

#include "esp_err.h"
#include "hal/adc_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 硬件通道：水位传感器模拟输出 -> GPIO5 (ADC1_CH4) */
#define LEVEL_ADC_UNIT     ADC_UNIT_1
#define LEVEL_ADC_CHANNEL  ADC_CHANNEL_4

esp_err_t water_level_init(void);

/**
 * @brief 采样水位
 * @param percent 输出 0~100%
 * @param raw     输出 12bit raw（可为 NULL）
 * @param mv      输出毫伏值（可为 NULL）
 */
esp_err_t water_level_read_raw(float *percent, int *raw, int *mv);

/** 由毫伏值换算水位百分比（纯函数，标定点来自 param_store） */
float water_level_convert_mv(int mv);

/** 兼容旧接口 */
esp_err_t water_level_read_voltage(float *voltage_v);
esp_err_t water_level_read(float *percent);

#ifdef __cplusplus
}
#endif

#endif // WATER_LEVEL_H
