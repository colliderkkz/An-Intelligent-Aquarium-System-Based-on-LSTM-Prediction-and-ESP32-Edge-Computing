#ifndef POWER_DIAG_H
#define POWER_DIAG_H

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 读取本次复位原因并累加计数（必须在 NVS 初始化之后调用一次） */
esp_err_t power_diag_init(void);

/** 本次复位原因的文本，如 "POWERON" / "BROWNOUT" */
const char *power_diag_reset_reason_text(void);

/** 本次复位原因的原始枚举值 */
int power_diag_reset_reason_raw(void);

/** 累计启动次数（掉电也保留） */
uint32_t power_diag_boot_count(void);

/** 累计"欠压复位"次数 —— 判断供电不足的关键指标 */
uint32_t power_diag_brownout_count(void);

/** 累计"正常上电"次数 */
uint32_t power_diag_poweron_count(void);

/** 上一次启动时的复位原因文本 */
const char *power_diag_last_reason_text(void);

#ifdef __cplusplus
}
#endif

#endif // POWER_DIAG_H
