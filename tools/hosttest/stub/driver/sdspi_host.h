#ifndef STUB_SDSPI_HOST_H
#define STUB_SDSPI_HOST_H

#include "driver/sdmmc_types.h"
#include "driver/gpio.h"

#define SDSPI_DEFAULT_HOST 1

typedef struct {
    int host_id;
    gpio_num_t gpio_cs;
    gpio_num_t gpio_cd;
    gpio_num_t gpio_wp;
    gpio_num_t gpio_int;
} sdspi_device_config_t;

#define SDSPI_DEVICE_CONFIG_DEFAULT() { .host_id = SDSPI_DEFAULT_HOST, .gpio_cs = GPIO_NUM_NC, .gpio_cd = GPIO_NUM_NC, .gpio_wp = GPIO_NUM_NC, .gpio_int = GPIO_NUM_NC }

#define SDSPI_HOST_DEFAULT() { .flags = 0, .slot = SDSPI_DEFAULT_HOST, .max_freq_khz = 20000 }

#endif
