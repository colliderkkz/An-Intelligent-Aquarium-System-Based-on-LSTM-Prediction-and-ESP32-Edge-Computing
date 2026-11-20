/**
 * @file debug_commands.h
 * @brief 上位机调试命令的实现与分发
 *
 * 命令表见 debug_commands.c 里的 s_cmds[]；HELP 命令会把整张表打印给上位机，
 * 因此新增命令只需要加一行表项 + 一个处理函数。
 */
#ifndef DEBUG_COMMANDS_H
#define DEBUG_COMMANDS_H

#include "bus/i2c_bus.h"
#include "debug/debug_protocol.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 输出通道（由 debug_console 提供） */
typedef struct {
    void (*send_line)(void *ctx, const char *line);
    void (*send_err)(void *ctx, const char *code, const char *msg);
    void *ctx;
} dbg_io_t;

/** 初始化（需要 I2C 总线句柄用于扫描/读写） */
esp_err_t debug_commands_init(i2c_bus_handle_t bus);

/**
 * @brief 分发一条已切分的命令
 * @return ESP_OK 成功；错误码由调用方转成 !ERR 行（handler 通常已自行发送错误）
 */
esp_err_t debug_commands_dispatch(const dbg_fields_t *fields, const dbg_io_t *io);

/** 构造一帧遥测数据（STREAM 用） */
void debug_commands_build_telemetry(char *buf, size_t cap);

/** 遥测流开关与周期（由 STREAM 命令设置，debug_console 轮询） */
bool debug_commands_stream_enabled(void);
uint32_t debug_commands_stream_period_ms(void);

/** 长耗时命令（如 TEST,ACTUATORS）中轮询，返回 true 表示上位机要求中断 */
bool debug_commands_should_abort(void);

/** 由 debug_console 注册中断检测回调 */
void debug_commands_set_abort_check(bool (*fn)(void));

#ifdef __cplusplus
}
#endif

#endif // DEBUG_COMMANDS_H
