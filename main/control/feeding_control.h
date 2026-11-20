#ifndef FEEDING_CONTROL_H
#define FEEDING_CONTROL_H

#include "drivers/pcf8563.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t feeding_control_init(void);
esp_err_t feeding_control_update(const pcf8563_time_t *now);

/** 清空去重键（例如上位机修改了喂食时间后希望立即重新判定） */
void feeding_control_reset_schedule(void);

#ifdef __cplusplus
}
#endif

#endif // FEEDING_CONTROL_H
