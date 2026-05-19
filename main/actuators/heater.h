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

#ifdef __cplusplus
}
#endif

#endif // HEATER_H
