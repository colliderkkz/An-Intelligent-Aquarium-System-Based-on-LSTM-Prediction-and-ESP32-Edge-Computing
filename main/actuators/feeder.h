#ifndef FEEDER_H
#define FEEDER_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化喂食器，配置舵机的 PWM
 * 
 * @return esp_err_t 成功返回 ESP_OK
 */
esp_err_t feeder_init(void);

/**
 * @brief 设置喂食器舵机的角度 (0-180)
 * 
 * @param angle 角度 (度)
 * @return esp_err_t 成功返回 ESP_OK
 */
esp_err_t feeder_set_angle(float angle);

/**
 * @brief 触发一次喂食动作 (转动到设定角度，然后复位)
 * 
 * @return esp_err_t 成功返回 ESP_OK
 */
esp_err_t feeder_feed_now(void);

#ifdef __cplusplus
}
#endif

#endif // FEEDER_H
