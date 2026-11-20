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
 * @brief 触发一次喂食动作 (转动到设定角度，保持后复位)
 *
 * 阻塞时间 = 2 * feed_hold_ms（默认 1.5s + 1.5s = 3s），
 * 请勿在 UART/定时器回调等对实时性敏感的上下文里调用。
 *
 * @return esp_err_t 成功返回 ESP_OK
 */
esp_err_t feeder_feed_now(void);

/** 当前角度（度） */
float feeder_get_angle(void);

/** 舵机 LEDC 占空比（用于上位机自检） */
unsigned int feeder_get_duty(void);

#ifdef __cplusplus
}
#endif

#endif // FEEDER_H
