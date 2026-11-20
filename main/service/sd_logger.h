/**
 * @file sd_logger.h
 * @brief SD 卡记录：每秒一行的传感器数据表 + 板子工作日志
 *
 * 目录结构（挂载点为 SD_MOUNT_POINT）：
 *   /sdcard/data/YYYY-MM-DD.csv   传感器，每秒一行，首行是表头
 *   /sdcard/log/YYYY-MM-DD.log    工作日志，ESP_LOG 的每一行都会落盘
 *
 * 设计取舍：
 *   - 两个文件按**日期**切分，插拔卡、跨天都不用人工干预；
 *   - 传感器数据来自 sensor_hub 的缓存（不触发采样，不阻塞控制任务）；
 *   - 日志走一个小的内存队列，写卡慢的时候丢日志也不阻塞 UART/控制；
 *   - 没插卡时任务自动待机，插卡后最多 5 秒自动开始记录。
 */
#ifndef SD_LOGGER_H
#define SD_LOGGER_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SD_LOGGER_DATA_DIR  "data"
#define SD_LOGGER_LOG_DIR   "log"

esp_err_t sd_logger_start(void);
esp_err_t sd_logger_stop(void);

/** 是否正在记录（卡已挂载且文件已打开） */
bool sd_logger_is_active(void);

/** 已写入的传感器行数（给上位机显示） */
uint32_t sd_logger_rows(void);

/** 因队列满而丢弃的日志行数 */
uint32_t sd_logger_dropped(void);

/** 当前数据文件路径（未记录时为空串） */
const char *sd_logger_data_path(void);

/** 当前日志文件路径（未记录时为空串） */
const char *sd_logger_log_path(void);

/**
 * @brief 把一行日志塞进落盘队列（非阻塞，队列满则丢弃并计数）
 *        由 debug_console 的日志输出钩子调用
 */
void sd_logger_push_log(const char *line, size_t len);


/** 当前两个文件的真实大小（字节），每 10 秒同步时更新 */
uint32_t sd_logger_data_size(void);
uint32_t sd_logger_log_size(void);

/** 开关实时推送：打开后每写入一行都会发一帧 #SDLINE 给上位机，
 *  同时回调给已注册的监听者（MQTT 用它在手机上实时显示） */
void sd_logger_set_stream(bool on);

/** 每写入一行的回调：kind = "csv" / "log"，text 不是以 \0 结尾时按 len 取 */
typedef void (*sd_line_cb_t)(const char *kind, const char *text, size_t len);
void sd_logger_set_line_cb(sd_line_cb_t cb);
bool sd_logger_is_streaming(void);
uint32_t sd_logger_streamed_csv(void);
uint32_t sd_logger_streamed_log(void);

/** 立即 flush 两个文件（上位机可主动调用） */
esp_err_t sd_logger_flush(void);

#ifdef __cplusplus
}
#endif

#endif // SD_LOGGER_H
