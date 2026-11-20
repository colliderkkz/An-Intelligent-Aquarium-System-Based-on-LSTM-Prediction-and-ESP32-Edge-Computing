/**
 * @file time_service.h
 * @brief 统一时间源：优先使用 PCF8563，RTC 掉线/未校准时自动退化为软件时钟
 *
 * 旧代码的问题：每个定时任务各自读 RTC，一旦 RTC 读失败（未焊、掉电、I2C 抖动）
 * 所有定时功能（灯光/喂食/紫外）就永久失效。这里改为：
 *   - 一个统一的“当前时间”，从 RTC 同步一次后靠 tick 自行走时；
 *   - RTC 不可用时用编译时间 + 上电时长兜底，保证定时逻辑仍可运行；
 *   - 周期性与 RTC 比对，自动纠正漂移。
 */
#ifndef TIME_SERVICE_H
#define TIME_SERVICE_H

#include "bus/i2c_bus.h"
#include "drivers/pcf8563.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化（bus 可为 NULL，表示没有 RTC） */
esp_err_t time_service_init(i2c_bus_handle_t bus);

/** 周期调用（建议 1s 一次），用于与 RTC 对时 */
void time_service_poll(void);

/** 获取当前时间，永不失败（out 不能为 NULL） */
esp_err_t time_service_now(pcf8563_time_t *out);

/** 当前 epoch 秒（本地民用时间，不做时区换算） */
uint32_t time_service_epoch(void);

/** 设置时间：写 RTC 并更新软件时钟 */
esp_err_t time_service_set(const pcf8563_time_t *t);

/** RTC 是否正常工作 */
bool time_service_rtc_ok(void);

/** 时间源描述："RTC" / "SOFT" */
const char *time_service_source(void);

/** RTC 是否被判定为"走时停止"（晶振不起振） */
bool time_service_rtc_frozen(void);

/** 标记当前时间来自网络（SNTP） */
void time_service_set_net_synced(bool on);

/** 当前时间是否来自网络对时 */
bool time_service_net_synced(void);

/** 把时间格式化成 "YYYY-MM-DD HH:MM:SS" */
void time_service_format(const pcf8563_time_t *t, char *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif // TIME_SERVICE_H
