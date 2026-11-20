#include "ds18b20.h"

#include "bus/onewire_bus.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "DS18B20";

/* 同类错误日志限流：60 秒最多一条 */
static bool ds18b20_log_due(void) {
    static int64_t last_us = 0;
    const int64_t now = esp_timer_get_time();
    if (last_us != 0 && now - last_us < 60000000LL) {
        return false;
    }
    last_us = now;
    return true;
}

#define DS18B20_CMD_SKIP_ROM     0xCCu
#define DS18B20_CMD_CONVERT_T    0x44u
#define DS18B20_CMD_READ_SCRATCH 0xBEu

/* 12bit 分辨率转换时间 750ms，这里留 50ms 余量 */
#define DS18B20_CONVERT_MS       800

/* 温度合理范围：探头掉线时读出 85℃(上电默认) 或 -127℃(断线) */
#define DS18B20_TEMP_MIN_C       (-55.0f)
#define DS18B20_TEMP_MAX_C       (125.0f)

static SemaphoreHandle_t s_lock = NULL;
static bool s_bus_ready = false;

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

static esp_err_t ensure_lock(void) {
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

esp_err_t ds18b20_init(void) {
    esp_err_t ret = ensure_lock();
    if (ret != ESP_OK) {
        return ret;
    }

    /* BUGFIX: 旧实现把“总线上没找到器件”当成初始化失败返回，导致温控任务
     * 直接被 main 跳过，而且探头后插上也不会恢复。现在初始化只负责配置总线，
     * 器件是否存在由每次读取的结果反映。 */
    ret = onewire_bus_init();
    if (ret != ESP_OK) {
        s_bus_ready = false;
        return ret;
    }
    s_bus_ready = true;

    if (!onewire_bus_reset()) {
        ESP_LOGW(TAG, "No DS18B20 detected on the OneWire bus (will retry on read)");
    } else {
        ESP_LOGI(TAG, "DS18B20 detected");
    }
    return ESP_OK;
}

esp_err_t ds18b20_read_temperature_c(float *celsius) {
    if (!celsius) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = ensure_lock();
    if (ret != ESP_OK) {
        return ret;
    }

    if (!s_bus_ready) {
        ret = ds18b20_init();
        if (ret != ESP_OK) {
            return ret;
        }
    }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    uint8_t scratch[9] = {0};
    esp_err_t result = ESP_OK;

    if (!onewire_bus_reset()) {
        result = ESP_ERR_NOT_FOUND;
        goto exit;
    }

    onewire_bus_write_byte(DS18B20_CMD_SKIP_ROM);
    onewire_bus_write_byte(DS18B20_CMD_CONVERT_T);
    vTaskDelay(pdMS_TO_TICKS(DS18B20_CONVERT_MS));

    if (!onewire_bus_reset()) {
        result = ESP_ERR_NOT_FOUND;
        goto exit;
    }

    onewire_bus_write_byte(DS18B20_CMD_SKIP_ROM);
    onewire_bus_write_byte(DS18B20_CMD_READ_SCRATCH);
    for (int i = 0; i < 9; i++) {
        scratch[i] = onewire_bus_read_byte();
    }

    if (ds18b20_crc8(scratch, 8) != scratch[8]) {
        /* 每 2 秒采样一次，错误时不能每 2 秒刷一条日志：
         * 一条日志 = UART 同步写 + 一次 SD 写，会把整个系统拖慢。
         * 60 秒最多一条，够判断问题就行。 */
        if (ds18b20_log_due()) {
            ESP_LOGW(TAG, "Scratchpad CRC error");
        }
        result = ESP_ERR_INVALID_CRC;
        goto exit;
    }

    {
        int16_t raw = (int16_t)((uint16_t)scratch[0] | ((uint16_t)scratch[1] << 8));

        /* DS18B20 的几种“看起来合法、其实是通信失败”的读数，必须拦掉，
         * 否则 0.00C 会被当成真实的低温，直接导致加热棒一直加热：
         *   0x0000  -> 0.00C，数据线被一直拉低 / 器件没真正响应
         *   0x0550  -> +85.0C，上电默认值，表示 CONVERT_T 没有执行
         *   0xFFFF  -> -0.0625C，总线浮空（全 1）
         */
        if (raw == 0x0000 || raw == 0x0550 || (uint16_t)raw == 0xFFFF) {
            ESP_LOGD(TAG, "Invalid DS18B20 raw value 0x%04X (bus/convert failure)",
                     (unsigned)(uint16_t)raw);
            result = ESP_ERR_INVALID_RESPONSE;
            goto exit;
        }

        float temp = (float)raw / 16.0f;
        if (temp < DS18B20_TEMP_MIN_C || temp > DS18B20_TEMP_MAX_C) {
            result = ESP_ERR_INVALID_RESPONSE;
            goto exit;
        }
        *celsius = temp;
    }

exit:
    xSemaphoreGive(s_lock);
    return result;
}
