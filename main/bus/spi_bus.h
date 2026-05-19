#ifndef SPI_BUS_H
#define SPI_BUS_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化系统级 SPI 总线
 * 
 * @return esp_err_t 成功返回 ESP_OK
 */
esp_err_t spi_bus_init(void);

/**
 * @brief 反初始化系统级 SPI 总线
 * 
 * @return esp_err_t 成功返回 ESP_OK
 */
esp_err_t spi_bus_deinit(void);

#ifdef __cplusplus
}
#endif

#endif // SPI_BUS_H
