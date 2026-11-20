/**
 * @file sntp_sync.h
 * @brief 用 SNTP 从互联网取时间，作为没有可用 RTC 时的权威时间源
 *
 * 背景：这块板子的 PCF8563 晶振没起振（负载电容缺失），芯片能读写但秒不走。
 * 不换硬件的前提下，只要板子连着 WiFi，就可以从 NTP 服务器取时间：
 *   - 开机连上 WiFi 后自动对一次
 *   - 之后每小时重新对齐，抵消软件时钟的漂移
 *   - 断电重启后只要 WiFi 能连上，时间就自动恢复正确
 * 所以"断开上位机"完全不影响板子走时。
 */
#ifndef SNTP_SYNC_H
#define SNTP_SYNC_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 启动守护任务：WiFi 一连上就自动对时，之后周期性重对齐 */
esp_err_t sntp_sync_start(void);

/** 立即对一次时（异步，结果看 sntp_sync_ok） */
void sntp_sync_now(void);

/** 是否已经成功对过时 */
bool sntp_sync_ok(void);

/** 上次对时的 epoch（0 表示还没对过） */
uint32_t sntp_sync_last_epoch(void);

/** 状态摘要，给 NET,STATUS / INFO 用 */
const char *sntp_sync_status(void);

#ifdef __cplusplus
}
#endif

#endif // SNTP_SYNC_H
