#include "sd_card_spi.h"

#include "config/pin_config.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

static const char *TAG = "SD_CARD";

static sdmmc_card_t *s_card = NULL;
static bool s_spi_owned = false;

esp_err_t sd_card_init(const char *mount_point) {
    if (!mount_point) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SPI_MOSI,
        .miso_io_num = SPI_MISO,
        .sclk_io_num = SPI_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };

    esp_err_t ret = spi_bus_initialize(SPI_HOST, &bus_cfg, SPI_DMA_CHAN);
    if (ret == ESP_OK) {
        s_spi_owned = true;
    } else if (ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to initialize SPI bus: %s", esp_err_to_name(ret));
        return ret;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI_HOST;
    host.max_freq_khz = SPI_CLOCK_SPEED_HZ / 1000;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = SPI_CS_SD_CARD;
    slot_config.host_id = host.slot;

    ret = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config, &mount_config, &s_card);
    if (ret != ESP_OK) {
        if (s_spi_owned) {
            (void)spi_bus_free(SPI_HOST);
            s_spi_owned = false;
        }
        ESP_LOGE(TAG, "Failed to mount SD card: %s", esp_err_to_name(ret));
        return ret;
    }

    sdmmc_card_print_info(stdout, s_card);
    ESP_LOGI(TAG, "SD card mounted at %s", mount_point);
    return ESP_OK;
}

esp_err_t sd_card_deinit(const char *mount_point) {
    if (!s_card) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = esp_vfs_fat_sdcard_unmount(mount_point, s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to unmount SD card: %s", esp_err_to_name(ret));
        return ret;
    }

    s_card = NULL;
    if (s_spi_owned) {
        (void)spi_bus_free(SPI_HOST);
        s_spi_owned = false;
    }

    ESP_LOGI(TAG, "SD card unmounted from %s", mount_point);
    return ESP_OK;
}
