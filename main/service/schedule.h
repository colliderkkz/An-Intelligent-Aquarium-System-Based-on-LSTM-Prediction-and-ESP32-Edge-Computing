/**
 * @file schedule.h
 * @brief 执行器定时计划：给每一路输出预设"每天什么时候、以多大强度开启"
 *
 * 设计取舍：
 *   - 只做"每日重复的单个时间窗"（开始分钟 + 持续分钟 + 强度%），
 *     够覆盖水草灯补光、气泵增氧、造浪时段、UV 杀菌、CO2 随灯开这些真实需求，
 *     又不至于引入完整的 cron 解析器；
 *   - 计划存 NVS（掉电保留），并在启动时自动加载；
 *   - 某一路一旦启用了计划，就由计划**完全接管**该执行器（原自动策略让位），
 *     避免两套逻辑每秒互相覆盖；
 *   - 手动模式（MANUAL）下计划暂停，用户点动优先。
 */
#ifndef SCHEDULE_H
#define SCHEDULE_H

#include "drivers/pcf8563.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SCH_LIGHT = 0,   /**< 水草灯（支持 0~100% PWM 强度） */
    SCH_AIR,         /**< 气泵（强度>0 即开） */
    SCH_PUMP,        /**< 循环泵 */
    SCH_UV,          /**< 紫外灯 */
    SCH_CO2,         /**< CO2 阀 */
    SCH_COUNT
} sched_id_t;

typedef struct {
    uint8_t enabled;     /**< 0=未启用 1=启用（启用即接管该执行器） */
    uint8_t percent;     /**< 0~100，0 视为关 */
    uint16_t start_min;  /**< 起始时间：一天中的第几分钟 0~1439 */
    uint16_t dur_min;    /**< 持续分钟 1~1440（可跨午夜） */
} sched_entry_t;

esp_err_t schedule_init(void);

/** 从 NVS 加载；无有效数据时全部置为"未启用" */
esp_err_t schedule_load(void);

/** 保存到 NVS */
esp_err_t schedule_save(void);

/** 全部恢复为"未启用"（不自动保存） */
void schedule_reset_all(void);

int schedule_count(void);

/** "light" / "air" / "pump" / "uv" / "co2" */
const char *schedule_name(int idx);

/** 按名字查下标，找不到返回 false */
bool schedule_find(const char *name, int *out_idx);

/** 只读访问；idx 越界返回 NULL */
const sched_entry_t *schedule_get(int idx);

/** 写入一条（会做范围裁剪）；不自动保存到 NVS */
esp_err_t schedule_set(int idx, const sched_entry_t *e);

/** 该执行器是否启用了计划 */
bool schedule_is_active(int idx);

/** 当前时刻该计划应输出的强度；未启用返回 -1 */
int schedule_desired_percent(int idx, const pcf8563_time_t *now);

/**
 * @brief 查询某一路计划此刻的窗口状态
 * @param in_window       当前是否处于计划窗口内
 * @param remain_min      窗口内还剩多少分钟（不在窗口内时为 0）
 * @param until_start_min 距离下次开始还有多少分钟（窗口内时为 0）
 */
void schedule_window_info(int idx, const pcf8563_time_t *now,
                          bool *out_in_window, uint16_t *remain_min,
                          uint16_t *until_start_min);

/** 把"一天中的第几分钟"格式化成 "HH:MM" */
void schedule_format_minutes(uint16_t minutes, char *buf, size_t len);

/**
 * @brief 每秒调用：按计划驱动执行器（只在目标值变化时动作，不会刷日志）
 */
void schedule_update(const pcf8563_time_t *now);

/* ---- 按执行器独立的手动保持 ----------------------------------------
 * 用户单独操作某个执行器时，只让"这一个"的计划暂停，
 * 其它执行器的计划、以及温控/补水/喂食照常运行。
 */
void schedule_hold_manual(const char *name, uint32_t seconds);
bool schedule_is_held(int idx);
uint32_t schedule_hold_left_s(int idx);
uint32_t schedule_hold_max_left_s(void);
/** 当前处于保持状态的执行器名字，逗号分隔（给 state 上报用），无则空串 */
void schedule_held_names(char *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif // SCHEDULE_H
