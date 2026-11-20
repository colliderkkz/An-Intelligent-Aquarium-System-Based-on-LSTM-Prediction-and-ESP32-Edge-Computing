#ifndef STUB_SDMMC_TYPES_H
#define STUB_SDMMC_TYPES_H

#include "esp_err.h"
#include <stdint.h>
#include <stdio.h>

typedef struct sdmmc_card_t {
    unsigned short rca;
    int slot;
} sdmmc_card_t;

typedef struct {
    int flags;
    int slot;
    int max_freq_khz;
    int command_timeout_ms;
    int (*io_voltage)(void);
} sdmmc_host_t;

void sdmmc_card_print_info(FILE *stream, const sdmmc_card_t *card);

#endif
