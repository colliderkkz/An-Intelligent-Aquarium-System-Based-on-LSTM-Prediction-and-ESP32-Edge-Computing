#ifndef SD_CARD_SPI_H
#define SD_CARD_SPI_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t sd_card_init(const char *mount_point);
esp_err_t sd_card_deinit(const char *mount_point);

/** SD 卡是否已挂载（供上位机 SELFTEST 显示） */
bool sd_card_is_mounted(void);

/** 最近一次挂载失败的原始错误码（ESP_OK 表示上次成功） */
esp_err_t sd_card_last_error(void);

/** 卡有没有被 SD 协议层识别出来（用于区分"没插卡"和"文件系统不支持"） */
bool sd_card_was_detected(void);

/** 给上位机看的一句话诊断 */
const char *sd_card_last_error_hint(void);

#ifdef __cplusplus
}
#endif

#endif // SD_CARD_SPI_H
