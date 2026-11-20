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

/**
 * @brief 读取当前亮度百分比
 *
 * 旧实现没有 getter，手动模式退出后自动控制无法判断实际亮度，只能靠内部
 * 状态标志，容易出现“自动认为灯已开、实际却是关的”不同步问题。
 */
float light_actuator_get_brightness(void);

/** LEDC 实际写入的占空比（用于上位机自检） */
unsigned int light_actuator_get_duty(void);

/**
 * @brief 只驱动板载 PWM（绕过"光源二选一"路由）
 *
 * 供 netlea_lamp 在切换光源时使用：切到尼特利要把板载灯关掉，
 * 但不能把逻辑亮度清成 0，否则切回来就"灯灭了"。
 */
esp_err_t light_actuator_apply_board_duty(float brightness_percent);

#ifdef __cplusplus
}
#endif

#endif // LIGHT_ACTUATOR_H
