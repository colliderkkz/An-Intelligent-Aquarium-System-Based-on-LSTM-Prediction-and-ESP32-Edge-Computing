#ifndef ACT_HOLD_H
#define ACT_HOLD_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 执行器级"用户手动保持"
 *
 * 用户单独点了某个执行器后，让所有自动逻辑（计划 / 温控 / 补水）
 * 在这段时间内都不要碰它，实现"我点它就得听我的"。
 *
 * 覆盖：light / air / pump / uv / co2 / heater / refill
 */

/** 开始保持；name 不认识或 seconds=0 时忽略 */
void act_hold_set(const char *name, uint32_t seconds);

/** 该执行器是否处于保持期 */
bool act_hold_active(const char *name);

/** 剩余秒数，0 表示没在保持 */
uint32_t act_hold_left_s(const char *name);

/** 所有执行器里最长的剩余秒数（给手机显示用） */
uint32_t act_hold_max_left_s(void);

/** 所有处于保持状态的执行器名，逗号分隔；没有则空串 */
void act_hold_names(char *buf, uint32_t len);

/** 清掉所有保持 */
void act_hold_clear_all(void);

#ifdef __cplusplus
}
#endif

#endif // ACT_HOLD_H
