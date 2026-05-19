#ifndef LIGHT_ACTUATOR_H
#define LIGHT_ACTUATOR_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化水草灯，配置 PWM
 * 
 * @return esp_err_t 成功返回 ESP_OK
 */
esp_err_t light_actuator_init(void);

/**
 * @brief 设置水草灯的亮度百分比 (0.0~100.0)
 * 
 * @param brightness_percent 亮度百分比
 * @return esp_err_t 成功返回 ESP_OK
 */
esp_err_t light_actuator_set_brightness(float brightness_percent);

#ifdef __cplusplus
}
#endif

#endif // LIGHT_ACTUATOR_H
