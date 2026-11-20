#ifndef AIR_PUMP_H
#define AIR_PUMP_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 气泵（底板 QIBENG 2pin，AO3400 低边 MOS）
 *
 * 用途：给水体增氧。目前只做手动/上位机点动，不参与自动策略
 * （没有溶氧传感器，无法闭环，避免瞎开）。
 */
esp_err_t air_pump_init(void);
esp_err_t air_pump_set_state(bool on);
bool air_pump_get_state(void);

/** 引脚实际电平（0/1，-1 表示未初始化） */
int air_pump_get_level(void);

/** 编译期是否启用（AIR_PUMP_ENABLED） */
bool air_pump_is_enabled(void);

#ifdef __cplusplus
}
#endif

esp_err_t air_pump_set_level(uint8_t percent);

/** 设置强度 0~100（0 = 关） */

/** 读取当前强度 0~100 */
uint8_t air_pump_get_percent(void);
#endif // AIR_PUMP_H