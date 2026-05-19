#include "spi_bus.h"

#include "config/pin_config.h"
#include "driver/spi_master.h"
#include "esp_log.h"

static const char *TAG = "SPI_BUS";
static bool spi_bus_initialized = false;

esp_err_t spi_bus_init(void) {
    if (spi_bus_initialized) {
        return ESP_OK;
    }

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SPI_MOSI,
        .miso_io_num = SPI_MISO,
        .sclk_io_num = SPI_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };

    esp_err_t ret = spi_bus_initialize(SPI_HOST, &bus_cfg, SPI_DMA_CHAN);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SPI bus: %s", esp_err_to_name(ret));
        return ret;
    }

    spi_bus_initialized = true;
    ESP_LOGI(TAG, "SPI bus initialized on host %d", SPI_HOST);
    return ESP_OK;
}

esp_err_t spi_bus_deinit(void) {
    if (!spi_bus_initialized) {
        return ESP_OK;
    }

    esp_err_t ret = spi_bus_free(SPI_HOST);
    if (ret == ESP_OK) {
        spi_bus_initialized = false;
        ESP_LOGI(TAG, "SPI bus released");
    } else {
        ESP_LOGE(TAG, "Failed to free SPI bus: %s", esp_err_to_name(ret));
    }

    return ret;
}
