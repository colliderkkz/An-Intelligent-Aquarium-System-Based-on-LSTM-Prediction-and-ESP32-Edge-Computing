#ifndef LIGHT_SENSOR_H
#define LIGHT_SENSOR_H

#include "esp_err.h"
#include "hal/adc_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 硬件通道：光敏模块 AO -> GPIO14 (ADC2_CH3)
 * 注意：ADC2 与 WiFi 共用仲裁，WiFi 工作时可能读失败；本驱动会重试并保持上次有效值。 */
#define LIGHT_ADC_UNIT     ADC_UNIT_2
#define LIGHT_ADC_CHANNEL  ADC_CHANNEL_3

esp_err_t light_sensor_init(void);

/**
 * @brief 采样光照
 * @param percent 输出 0~100%
 * @param raw     输出 12bit raw（可为 NULL）
 * @param mv      输出毫伏值（可为 NULL）
 */
esp_err_t light_sensor_read_raw(float *percent, int *raw, int *mv);

/** 由毫伏值换算光照百分比（纯函数） */
float light_sensor_convert_mv(int mv);

/** 兼容旧接口（带 ADC2 失败重试/保持） */
esp_err_t light_sensor_read(float *percent);

#ifdef __cplusplus
}
#endif

#endif // LIGHT_SENSOR_H
