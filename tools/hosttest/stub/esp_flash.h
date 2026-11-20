#ifndef STUB_ESP_FLASH_H
#define STUB_ESP_FLASH_H

#include "esp_err.h"
#include <stdint.h>

typedef struct esp_flash_t esp_flash_t;

esp_err_t esp_flash_get_size(esp_flash_t *chip, uint32_t *out_size);

#endif
