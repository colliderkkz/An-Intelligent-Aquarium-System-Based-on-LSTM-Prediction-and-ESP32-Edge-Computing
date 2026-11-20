#ifndef TURBIDITY_H
#define TURBIDITY_H

#include "esp_err.h"
#include "hal/adc_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 硬件通道：AZDM01 浊度模块模拟输出 -> GPIO3 (ADC1_CH2) */
#define TURB_ADC_UNIT     ADC_UNIT_1
#define TURB_ADC_CHANNEL  ADC_CHANNEL_2

esp_err_t turbidity_init(void);

/**
 * @brief 采样浊度
 * @param temperature_c 水温（模块公式自带 -0.0192*(T-25) 补偿）
 * @param ntu           输出 NTU
 * @param raw           输出 12bit raw（可为 NULL）
 * @param mv            输出毫伏值（可为 NULL）
 */
esp_err_t turbidity_read_raw(float temperature_c, float *ntu, int *raw, int *mv);

/** 由毫伏值换算浊度（纯函数） */
float turbidity_convert_mv(int mv, float temperature_c);

/** 兼容旧接口 */
esp_err_t turbidity_read(float *ntu);
esp_err_t turbidity_read_compensated(float temperature_c, float *ntu);

#ifdef __cplusplus
}
#endif

#endif // TURBIDITY_H
