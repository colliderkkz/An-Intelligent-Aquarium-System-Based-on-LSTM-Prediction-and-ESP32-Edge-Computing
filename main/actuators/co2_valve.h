#ifndef CO2_VALVE_H
#define CO2_VALVE_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t co2_valve_init(void);
esp_err_t co2_valve_set_state(bool on);
bool co2_valve_get_state(void);

/** 引脚实际电平（0/1，-1 表示未初始化），用于现场校验极性 */
int co2_valve_get_level(void);

#ifdef __cplusplus
}
#endif

esp_err_t co2_valve_set_level(uint8_t percent);

/** 设置强度 0~100（0 = 关） */

/** 读取当前强度 0~100 */
uint8_t co2_valve_get_percent(void);
#endif // CO2_VALVE_H