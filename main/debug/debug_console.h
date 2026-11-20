/**
 * @file debug_console.h
 * @brief 上位机调试控制台（UART0 传输层 + 遥测流）
 *
 * 与 ESP_LOG 共用 UART0：
 *   - 日志行不带前缀，上位机忽略；
 *   - 协议行以 '#’(数据) / '!'(错误) 开头；
 *   - 上行以 '@' 开头（也允许省略，方便串口助手手敲）。
 *
 * 这样只需要一根 USB 线（板载 UART 口）就能既看日志又做现场调试，
 * 不需要额外占用 ASRPRO 的 UART2。
 */
#ifndef DEBUG_CONSOLE_H
#define DEBUG_CONSOLE_H

#include "bus/i2c_bus.h"
#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化并启动调试控制台任务 */
esp_err_t debug_console_start(i2c_bus_handle_t bus);

/** 是否已启动 */
bool debug_console_is_running(void);

/** 主动向上位机推送一行（自动加 '#' 前缀与 CRLF），线程安全 */
void debug_console_notify(const char *fmt, ...);

/** 长命令中断检测：UART 上有任何字节到达即返回 true（并丢弃该字节） */
bool debug_console_abort_requested(void);

#ifdef __cplusplus
}
#endif

#endif // DEBUG_CONSOLE_H
