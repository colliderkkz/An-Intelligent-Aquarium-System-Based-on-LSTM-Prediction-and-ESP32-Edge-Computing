#include "pcf8563.h"

#include "esp_log.h"

static const char *TAG = "PCF8563";

static uint8_t dec_to_bcd(uint8_t dec) {
    return (uint8_t)(((dec / 10U) << 4) | (dec % 10U));
}

static uint8_t bcd_to_dec(uint8_t bcd) {
    return (uint8_t)(((bcd >> 4) * 10U) + (bcd & 0x0FU));
}

esp_err_t pcf8563_init(i2c_bus_handle_t bus, uint8_t dev_addr) {
    if (!bus) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t data = 0;
    esp_err_t ret = i2c_bus_write_reg8(bus, dev_addr, 0x00, &data, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to reset control register 1");
        return ret;
    }

    ret = i2c_bus_write_reg8(bus, dev_addr, 0x01, &data, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to reset control register 2");
        return ret;
    }

    ESP_LOGI(TAG, "PCF8563 initialized");
    return ESP_OK;
}

esp_err_t pcf8563_set_time(i2c_bus_handle_t bus, uint8_t dev_addr, const pcf8563_time_t *time) {
    if (!bus || !time) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t data[7] = {0};
    data[0] = dec_to_bcd(time->second) & 0x7F;
    data[1] = dec_to_bcd(time->minute) & 0x7F;
    data[2] = dec_to_bcd(time->hour) & 0x3F;
    data[3] = dec_to_bcd(time->day) & 0x3F;
    data[4] = time->weekday & 0x07;

    uint8_t month_bcd = dec_to_bcd(time->month) & 0x1F;
    if (time->year >= 2000U) {
        month_bcd &= (uint8_t)~0x80U;
        data[6] = dec_to_bcd((uint8_t)(time->year - 2000U));
    } else {
        month_bcd |= 0x80U;
        data[6] = dec_to_bcd((uint8_t)(time->year - 1900U));
    }
    data[5] = month_bcd;

    return i2c_bus_write_reg8(bus, dev_addr, 0x02, data, sizeof(data));
}

esp_err_t pcf8563_get_time(i2c_bus_handle_t bus, uint8_t dev_addr, pcf8563_time_t *time) {
    if (!bus || !time) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t data[7] = {0};
    esp_err_t ret = i2c_bus_read_reg8(bus, dev_addr, 0x02, data, sizeof(data));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read RTC time");
        return ret;
    }

    time->second = bcd_to_dec(data[0] & 0x7F);
    time->minute = bcd_to_dec(data[1] & 0x7F);
    time->hour = bcd_to_dec(data[2] & 0x3F);
    time->day = bcd_to_dec(data[3] & 0x3F);
    time->weekday = data[4] & 0x07;
    time->month = bcd_to_dec(data[5] & 0x1F);
    time->year = (data[5] & 0x80U) ? (uint16_t)(1900U + bcd_to_dec(data[6]))
                                   : (uint16_t)(2000U + bcd_to_dec(data[6]));
    return ESP_OK;
}
