#ifndef TDS_SENSOR_H
#define TDS_SENSOR_H

#include "esp_err.h"
#include "hal/adc_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 硬件通道：TDS 模块模拟输出 -> GPIO2 (ADC1_CH1) */
#define TDS_ADC_UNIT     ADC_UNIT_1
#define TDS_ADC_CHANNEL  ADC_CHANNEL_1

esp_err_t tds_sensor_init(void);

/**
 * @brief 采样 TDS
 * @param temperature 水温（用于 2%/℃ 温度补偿）
 * @param tds_ppm     输出 ppm
 * @param raw         输出 12bit raw（可为 NULL）
 * @param mv          输出毫伏值（可为 NULL）
 */
esp_err_t tds_sensor_read_raw(float temperature, float *tds_ppm, int *raw, int *mv);

/** 由毫伏值换算 TDS（纯函数） */
float tds_sensor_convert_mv(int mv, float temperature);

/** 兼容旧接口 */
esp_err_t tds_sensor_read(float temperature, float *tds_value);

#ifdef __cplusplus
}
#endif

#endif // TDS_SENSOR_H
