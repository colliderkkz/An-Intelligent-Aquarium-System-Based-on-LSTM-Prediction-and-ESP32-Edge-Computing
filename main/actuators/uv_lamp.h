#ifndef UV_LAMP_H
#define UV_LAMP_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t uv_lamp_init(void);
esp_err_t uv_lamp_set_state(bool on);
bool uv_lamp_get_state(void);

/** 引脚实际电平（0/1，-1 表示未初始化），用于现场校验极性 */
int uv_lamp_get_level(void);

#ifdef __cplusplus
}
#endif

esp_err_t uv_lamp_set_level(uint8_t percent);

/** 设置强度 0~100（0 = 关） */

/** 读取当前强度 0~100 */
uint8_t uv_lamp_get_percent(void);
#endif // UV_LAMP_H