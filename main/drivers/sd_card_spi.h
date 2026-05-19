#ifndef SD_CARD_SPI_H
#define SD_CARD_SPI_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t sd_card_init(const char *mount_point);
esp_err_t sd_card_deinit(const char *mount_point);

#ifdef __cplusplus
}
#endif

#endif // SD_CARD_SPI_H
