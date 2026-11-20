#ifndef ACT_REGISTRY_H
#define ACT_REGISTRY_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file act_registry.h
 * @brief 执行器注册表
 *
 * 所有执行器在一张表里描述，加新执行器只需要：
 *   1) 在 act_registry.c 的表里加一行
 *   2) 板子会自动通过 caps 广播告知 App
 * App/网页不需要改代码，也不需要重新打包。
 */

typedef enum {
    ACT_KIND_SWITCH = 0,   /**< 开关量（PWM 满档/关闭） */
    ACT_KIND_DIMMABLE = 1, /**< 可调（真正按百分比输出） */
} act_kind_t;

typedef struct {
    const char *id;        /**< 设备内部名："light" / "pump" ... */
    const char *label;     /**< 中文名，给界面显示 */
    uint8_t kind;          /**< act_kind_t */
    uint8_t min_pct;       /**< 界面滑条下限（开关量给 100，可调给 10） */
    uint8_t max_pct;       /**< 上限，一般 100 */
    esp_err_t (*set_level)(uint8_t pct);
    uint8_t (*get_percent)(void);
    bool (*get_state)(void);
} act_desc_t;

int act_registry_count(void);
const act_desc_t *act_registry_at(int idx);
const act_desc_t *act_registry_find(const char *id);

#ifdef __cplusplus
}
#endif

#endif // ACT_REGISTRY_H
