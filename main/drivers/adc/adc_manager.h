/**
 * @file adc_manager.h
 * @brief ESP32-S3 ADC 统一管理（ADC1 + ADC2，单次采样 + 校准）
 *
 * 相对旧版的改动：
 *  - 同时管理 ADC1/ADC2 两个单元（光照传感器在 GPIO14 = ADC2_CH3）；
 *  - 一次采样同时返回 raw 与校准后的毫伏值，避免重复采样；
 *  - 通道首次使用自动配置，忘记调用 config 也不会读失败；
 *  - 校准失败时自动退化为 raw * Vref / fullscale。
 */
#ifndef ADC_MANAGER_H
#define ADC_MANAGER_H

#include "esp_err.h"
#include "hal/adc_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化 ADC1 / ADC2（可重复调用） */
esp_err_t adc_manager_init(void);

/** ADC 是否可用 */
bool adc_manager_ready(void);

/** 配置某个通道（一般不用手动调用，read 会懒配置） */
esp_err_t adc_manager_config(adc_unit_t unit, adc_channel_t channel);

/**
 * @brief 采样并返回均值
 * @param unit     ADC_UNIT_1 / ADC_UNIT_2
 * @param channel  通道号
 * @param raw_out  12bit 原始值（可为 NULL）
 * @param mv_out   校准后的毫伏值（可为 NULL）
 */
esp_err_t adc_manager_read(adc_unit_t unit, adc_channel_t channel, int *raw_out, int *mv_out);

/** 12bit raw -> 毫伏（无校准时使用线性换算） */
esp_err_t adc_manager_raw_to_mv(adc_unit_t unit, int raw, int *mv_out);

#ifdef __cplusplus
}
#endif

#endif // ADC_MANAGER_H
