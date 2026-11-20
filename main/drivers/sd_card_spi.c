#include "sd_card_spi.h"

#include "config/pin_config.h"
#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

static const char *TAG = "SD_CARD";

static sdmmc_card_t *s_card = NULL;
static bool s_spi_owned = false;
/* 最近一次挂载失败的原始错误码，用来区分"根本没插卡"和"卡在但文件系统不支持" */
static esp_err_t s_last_err = ESP_OK;
/* 卡有没有被识别出来（SD 协议层通了但文件系统挂不上时要能分辨） */
static bool s_card_detected = false;

esp_err_t sd_card_init(const char *mount_point) {
    if (!mount_point) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 没插卡时 sdmmc 会打一堆 E 级日志，这里压到 WARN 保持控制台干净 */
    esp_log_level_set("vfs_fat_sdmmc", ESP_LOG_WARN);
    esp_log_level_set("sdmmc_common", ESP_LOG_WARN);
    esp_log_level_set("sdmmc_init", ESP_LOG_WARN);

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

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = SPI_CS_SD_CARD;
    slot_config.host_id = host.slot;

    /* SD 规范：初始化阶段（CMD0/CMD8/ACMD41...）SPI 时钟必须在 100~400kHz。
     * 之前直接给 10MHz，卡在 CMD8 就不响应（0x108）。
     * 这里先用 400kHz；有些卡/长杜邦线在 400kHz 也不应答，再降到 100kHz 试一次。
     * 全部失败说明是电气层问题（接触/供电），不是文件系统。 */
    static const int kInitFreqKhz[] = {400, 100};
    ret = ESP_FAIL;
    for (size_t fi = 0; fi < sizeof(kInitFreqKhz) / sizeof(kInitFreqKhz[0]); ++fi) {
        host.max_freq_khz = kInitFreqKhz[fi];
        ret = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config,
                                      &mount_config, &s_card);
        if (ret == ESP_OK) {
            if (fi > 0) {
                ESP_LOGW(TAG, "挂载成功（降速到 %d kHz 才应答）", kInitFreqKhz[fi]);
            }
            break;
        }
        ESP_LOGW(TAG, "%d kHz 初始化失败: %s", kInitFreqKhz[fi], esp_err_to_name(ret));
        /* 失败后 mount 内部可能已经释放/保留了总线，下一次尝试前重新确保总线在 */
        if (!s_spi_owned) {
            esp_err_t br = spi_bus_initialize(SPI_HOST, &bus_cfg, SPI_DMA_CHAN);
            if (br == ESP_OK) {
                s_spi_owned = true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (ret != ESP_OK) {
        if (s_spi_owned) {
            (void)spi_bus_free(SPI_HOST);
            s_spi_owned = false;
        }
        s_last_err = ret;
        s_card_detected = (ret != ESP_ERR_NOT_FOUND && ret != ESP_ERR_TIMEOUT &&
                           !(ret == ESP_ERR_INVALID_RESPONSE));
        ESP_LOGE(TAG, "Failed to mount SD card: %s (card_detected=%d)",
                 esp_err_to_name(ret), s_card_detected ? 1 : 0);
        if (s_card_detected) {
            ESP_LOGE(TAG, "卡能被识别但文件系统挂载失败 —— ESP32 只支持 FAT12/16/32，"
                          "exFAT/NTFS/多分区 PE 启动盘都挂不上，请格式化成 FAT32 单分区");
        }
        return ret;
    }

    s_last_err = ESP_OK;
    s_card_detected = true;

    sdmmc_card_print_info(stdout, s_card);

    /* 识别阶段必须 400kHz（IDF 的 sdmmc_card_init 直接拿 max_freq_khz 当识别时钟，
     * 不会自动降速），但挂载完还停在 400kHz 就太慢了：
     * 每 10 秒一次的 close+stat+reopen 同步要花 1 秒以上，直接把那一秒的数据行吃掉
     * （实测 CSV 覆盖率只有 61.7%）。这里识别成功后把时钟提上去。 */
    esp_err_t clk_ret = sdspi_host_set_card_clk(host.slot, 10000);
    if (clk_ret == ESP_OK) {
        ESP_LOGI(TAG, "SD clock raised to 10 MHz");
    } else {
        ESP_LOGW(TAG, "raise SD clock failed (%s), keep 400kHz", esp_err_to_name(clk_ret));
    }

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
    s_last_err = ESP_OK;
    s_card_detected = false;
    if (s_spi_owned) {
        (void)spi_bus_free(SPI_HOST);
        s_spi_owned = false;
    }

    ESP_LOGI(TAG, "SD card unmounted from %s", mount_point);
    return ESP_OK;
}

bool sd_card_is_mounted(void) {
    return s_card != NULL;
}

esp_err_t sd_card_last_error(void) {
    return s_last_err;
}

bool sd_card_was_detected(void) {
    return s_card_detected;
}

const char *sd_card_last_error_hint(void) {
    if (s_card != NULL) {
        return "ok";
    }
    if (s_last_err == ESP_OK) {
        return "not probed yet";
    }
    if (!s_card_detected) {
        return "no card / no response";
    }
    return "card ok but filesystem unsupported (need FAT32, single partition)";
}
