#include "sgp30.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "SGP30";

static uint8_t sgp30_calc_crc(const uint8_t data[2]) {
    uint8_t crc = 0xFF;
    for (int i = 0; i < 2; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

esp_err_t sgp30_init(i2c_bus_handle_t bus, uint8_t dev_addr) {
    if (!bus) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t cmd[2] = {
        (uint8_t)((SGP30_CMD_INIT_AIR_QUALITY >> 8) & 0xFF),
        (uint8_t)(SGP30_CMD_INIT_AIR_QUALITY & 0xFF),
    };

    esp_err_t ret = i2c_bus_write(bus, dev_addr, cmd, sizeof(cmd));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SGP30: %s", esp_err_to_name(ret));
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_LOGI(TAG, "SGP30 initialized");
    return ESP_OK;
}

esp_err_t sgp30_read_data(i2c_bus_handle_t bus, uint8_t dev_addr, sgp30_data_t *data) {
    if (!bus || !data) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t cmd[2] = {
        (uint8_t)((SGP30_CMD_MEASURE_AIR_QUALITY >> 8) & 0xFF),
        (uint8_t)(SGP30_CMD_MEASURE_AIR_QUALITY & 0xFF),
    };

    esp_err_t ret = i2c_bus_write(bus, dev_addr, cmd, sizeof(cmd));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to trigger SGP30 measurement: %s", esp_err_to_name(ret));
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(15));

    uint8_t buf[6] = {0};
    ret = i2c_bus_read(bus, dev_addr, buf, sizeof(buf));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read SGP30 data: %s", esp_err_to_name(ret));
        return ret;
    }

    if (sgp30_calc_crc(&buf[0]) != buf[2]) {
        return ESP_ERR_INVALID_CRC;
    }
    if (sgp30_calc_crc(&buf[3]) != buf[5]) {
        return ESP_ERR_INVALID_CRC;
    }

    data->co2_eq = (uint16_t)((buf[0] << 8) | buf[1]);
    data->tvoc = (uint16_t)((buf[3] << 8) | buf[4]);
    return ESP_OK;
}
