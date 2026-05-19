#include "ds18b20.h"

#include "bus/onewire_bus.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "DS18B20";

#define DS18B20_CMD_SKIP_ROM     0xCCu
#define DS18B20_CMD_CONVERT_T    0x44u
#define DS18B20_CMD_READ_SCRATCH 0xBEu

static SemaphoreHandle_t s_lock;

static uint8_t ds18b20_crc8(const uint8_t *data, size_t len) {
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t in_byte = data[i];
        for (int j = 0; j < 8; j++) {
            uint8_t mix = (uint8_t)((crc ^ in_byte) & 0x01u);
            crc >>= 1;
            if (mix) {
                crc ^= 0x8Cu;
            }
            in_byte >>= 1;
        }
    }
    return crc;
}

esp_err_t ds18b20_init(void) {
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) {
            return ESP_ERR_NO_MEM;
        }
    }

    esp_err_t ret = onewire_bus_init();
    if (ret != ESP_OK) {
        return ret;
    }

    if (!onewire_bus_reset()) {
        ESP_LOGW(TAG, "No DS18B20 detected on the OneWire bus");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "DS18B20 detected");
    return ESP_OK;
}

esp_err_t ds18b20_read_temperature_c(float *celsius) {
    if (!celsius) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    uint8_t scratch[9] = {0};

    if (!onewire_bus_reset()) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }

    onewire_bus_write_byte(DS18B20_CMD_SKIP_ROM);
    onewire_bus_write_byte(DS18B20_CMD_CONVERT_T);
    vTaskDelay(pdMS_TO_TICKS(800));

    if (!onewire_bus_reset()) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }

    onewire_bus_write_byte(DS18B20_CMD_SKIP_ROM);
    onewire_bus_write_byte(DS18B20_CMD_READ_SCRATCH);
    for (int i = 0; i < 9; i++) {
        scratch[i] = onewire_bus_read_byte();
    }

    xSemaphoreGive(s_lock);

    if (ds18b20_crc8(scratch, 8) != scratch[8]) {
        ESP_LOGW(TAG, "Scratchpad CRC error");
        return ESP_ERR_INVALID_CRC;
    }

    int16_t raw = (int16_t)((uint16_t)scratch[0] | ((uint16_t)scratch[1] << 8));
    *celsius = (float)raw / 16.0f;
    return ESP_OK;
}
