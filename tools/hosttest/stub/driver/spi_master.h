#ifndef STUB_DRIVER_SPI_MASTER_H
#define STUB_DRIVER_SPI_MASTER_H

#include "esp_err.h"
#include "driver/gpio.h"
#include <stdint.h>
#include <stddef.h>

typedef int spi_host_device_t;
#define SPI1_HOST 0
#define SPI2_HOST 1
#define SPI3_HOST 2

#define SPI_DMA_CH_AUTO 3

typedef struct {
    int mosi_io_num;
    int miso_io_num;
    int sclk_io_num;
    int quadwp_io_num;
    int quadhd_io_num;
    int max_transfer_sz;
    int flags;
    int intr_flags;
} spi_bus_config_t;

esp_err_t spi_bus_initialize(spi_host_device_t host_id, const spi_bus_config_t *bus_config, int dma_chan);
esp_err_t spi_bus_free(spi_host_device_t host_id);

#endif
