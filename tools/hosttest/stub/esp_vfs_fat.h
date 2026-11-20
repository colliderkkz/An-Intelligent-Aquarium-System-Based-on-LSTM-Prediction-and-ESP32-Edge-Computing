#ifndef STUB_ESP_VFS_FAT_H
#define STUB_ESP_VFS_FAT_H

#include "esp_err.h"
#include "driver/sdmmc_types.h"
#include "driver/sdspi_host.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    bool format_if_mount_failed;
    int max_files;
    size_t allocation_unit_size;
} esp_vfs_fat_sdmmc_mount_config_t;

esp_err_t esp_vfs_fat_sdspi_mount(const char *base_path, const sdmmc_host_t *host_config_input,
                                  const sdspi_device_config_t *slot_config,
                                  const esp_vfs_fat_sdmmc_mount_config_t *mount_config,
                                  sdmmc_card_t **out_card);
esp_err_t esp_vfs_fat_sdcard_unmount(const char *base_path, sdmmc_card_t *card);

#endif
