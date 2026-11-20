/**
 * @file gpio_switch.h
 * @brief 通用 GPIO 开关量输出（带极性反转 / 上电即时拉低防误动作）
 *
 * 底板上 6 路负载都是 AO3400 N-MOS 低边开关，但不同批次/接线可能是高有效
 * 也可能是低有效（继电器模块常见低有效）。把极性做成参数（inv_xxx），
 * 现场调试时用 SET,PARAM,inv_xxx,1 就能翻转，不用改固件。
 */
#ifndef GPIO_SWITCH_H
#define GPIO_SWITCH_H

#include "config/param_store.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    gpio_num_t pin;
    param_id_t invert_param;   /**< 极性反转参数索引，没有则传 PARAM_IDX_COUNT */
    const char *name;
    bool state;                /**< 逻辑状态（percent > 0 即为开） */
    bool initialized;
    uint8_t percent;           /**< 当前输出强度 0~100 */
} gpio_switch_t;

/** 初始化：先写输出锁存为“关”，再切输出模式，避免上电瞬间误动作 */
esp_err_t gpio_switch_init(gpio_switch_t *sw);

/** 设置逻辑开关状态（内部按极性换算电平） */
esp_err_t gpio_switch_set(gpio_switch_t *sw, bool on);

/** 读取逻辑状态（软件记录） */
bool gpio_switch_get(const gpio_switch_t *sw);

/** 读取引脚实际电平（0/1），可用于现场校验 */
int gpio_switch_read_level(const gpio_switch_t *sw);

/**
 * @brief 设置输出强度（PWM）
 *
 * 这 6 路负载都是 AO3400 MOS 低边开关，可用 LEDC 做 PWM。
 * 不在 PWM 映射表里的引脚会退化成开关量（percent>0 即开）。
 *
 * @param percent 0~100，0 = 关
 */
esp_err_t gpio_switch_set_percent(gpio_switch_t *sw, uint8_t percent);

/** 读取当前强度 0~100 */
uint8_t gpio_switch_get_percent(const gpio_switch_t *sw);

#ifdef __cplusplus
}
#endif

#endif // GPIO_SWITCH_H
