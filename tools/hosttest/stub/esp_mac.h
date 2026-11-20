#ifndef STUB_ESP_MAC_H
#define STUB_ESP_MAC_H

#include "esp_err.h"
#include <stdint.h>

typedef enum {
    ESP_MAC_WIFI_STA = 0,
    ESP_MAC_WIFI_SOFTAP = 1,
    ESP_MAC_BT = 2,
    ESP_MAC_ETH = 3,
} esp_mac_type_t;

esp_err_t esp_read_mac(uint8_t *mac, esp_mac_type_t type);

#endif
