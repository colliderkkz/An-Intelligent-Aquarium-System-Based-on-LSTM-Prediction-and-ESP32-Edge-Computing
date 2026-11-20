#ifndef HEATER_H
#define HEATER_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t heater_init(void);

/**
 * @brief 设置加热丝状态
 *
 * @param on true 打开，false 关闭
 * @return esp_err_t 成功返回 ESP_OK
 */
esp_err_t heater_set_state(bool on);

/**
 * @brief 获取当前加热丝状态
 *
 * @return true 开启情况，false 关闭情况
 */
bool heater_get_state(void);

/** 引脚实际电平（0/1，-1 表示未初始化），用于现场校验极性 */
int heater_get_level(void);

#ifdef __cplusplus
}
#endif

esp_err_t heater_set_level(uint8_t percent);

/** 设置强度 0~100（0 = 关） */

/** 读取当前强度 0~100 */
uint8_t heater_get_percent(void);
#endif // HEATER_H