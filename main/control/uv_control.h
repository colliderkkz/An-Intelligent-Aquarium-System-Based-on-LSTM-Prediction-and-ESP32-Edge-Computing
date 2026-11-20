/**
 * @file uv_control.h
 * @brief 紫外灯定时消杀（原本写在 main.c 的 uv_schedule_task 里，抽成独立模块）
 */
#ifndef UV_CONTROL_H
#define UV_CONTROL_H

#include "drivers/pcf8563.h"
#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t uv_control_init(void);

/** 每分钟调用一次即可（内部按“时:分”窗口判断） */
esp_err_t uv_control_update(const pcf8563_time_t *now);

bool uv_control_is_on(void);

#ifdef __cplusplus
}
#endif

#endif // UV_CONTROL_H
