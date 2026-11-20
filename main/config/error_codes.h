/**
 * @file error_codes.h
 * @brief 全局错误码定义与字符串化
 *
 * 说明：
 *  - ESP-IDF 自带 esp_err_t（0x100 以下为通用错误，0x100 以上为组件错误）。
 *  - 本文件定义“业务层”错误码，统一放在 0x8000 起，避免和 IDF 组件冲突。
 *  - 调试上位机收到 !ERR,<code>,<msg> 时可以直接查表显示中文含义。
 */
#ifndef ERROR_CODES_H
#define ERROR_CODES_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 业务错误码基址（避开 IDF 组件错误码段） */
#define APP_ERR_BASE                    0x8000

/* ==================== 通用 ==================== */
#define APP_ERR_INVALID_STATE           (APP_ERR_BASE + 1)   /* 模块未初始化 */
#define APP_ERR_BAD_FRAME               (APP_ERR_BASE + 2)   /* 帧格式错误 */
#define APP_ERR_UNKNOWN_CMD             (APP_ERR_BASE + 3)   /* 未知命令 */
#define APP_ERR_BAD_ARG                 (APP_ERR_BASE + 4)   /* 参数个数/取值非法 */
#define APP_ERR_UNKNOWN_ID              (APP_ERR_BASE + 5)   /* 未知传感器/执行器/参数名 */
#define APP_ERR_NOT_ALLOWED             (APP_ERR_BASE + 6)   /* 当前模式不允许该操作 */
#define APP_ERR_OVERFLOW                (APP_ERR_BASE + 7)   /* 缓冲区溢出 */

/* ==================== 传感器 ==================== */
#define APP_ERR_SENSOR_READ             (APP_ERR_BASE + 0x10)
#define APP_ERR_SENSOR_STALE            (APP_ERR_BASE + 0x11)
#define APP_ERR_SENSOR_RANGE            (APP_ERR_BASE + 0x12)

/* ==================== 执行器 ==================== */
#define APP_ERR_ACTUATOR                (APP_ERR_BASE + 0x20)
#define APP_ERR_ACTUATOR_COOLDOWN       (APP_ERR_BASE + 0x21)

/* ==================== 持久化 ==================== */
#define APP_ERR_NVS                     (APP_ERR_BASE + 0x30)
#define APP_ERR_IO                      (APP_ERR_BASE + 0x40)   /* 文件/SD IO 失败 */
#define APP_ERR_NVS_BLOB                (APP_ERR_BASE + 0x31)
#define APP_ERR_NVS_CRC                 (APP_ERR_BASE + 0x32)

/**
 * @brief 把 esp_err_t / APP_ERR_* 转成简短 ASCII 标识（用于串口协议）
 */
const char *app_err_name(esp_err_t err);

/**
 * @brief 把 esp_err_t / APP_ERR_* 转成中文描述（用于日志/上位机）
 */
const char *app_err_desc(esp_err_t err);

#ifdef __cplusplus
}
#endif

#endif /* ERROR_CODES_H */
