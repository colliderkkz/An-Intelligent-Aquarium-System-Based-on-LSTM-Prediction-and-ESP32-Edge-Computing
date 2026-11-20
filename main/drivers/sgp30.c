#include "sgp30.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "SGP30";

/* 传感器失效时会 1Hz 刷日志，必须限流（60 秒最多一条） */
static bool sgp30_log_due(void) {
    static int64_t last_us = 0;
    const int64_t now = esp_timer_get_time();
    if (last_us != 0 && now - last_us < 60000000LL) {
        return false;
    }
    last_us = now;
    return true;
}

/* 数据手册时序：init_air_quality 后需 15ms；measure 命令后需 12ms 才能取数 */
#define SGP30_INIT_DELAY_MS     20
/* SGP30 手册写 measure_iaq 需要 12ms，实测 15ms 仍然读不到（从机 NACK）。
 * 用手动裸读验证：等 40ms 一定能读到 400ppm/0ppb，说明芯片要求更宽裕的余量。
 * 这里取 30ms 兼顾稳定与采样周期。 */
#define SGP30_MEASURE_DELAY_MS  30
/* 手册要求 iaq_init 之后静置 15s 才能开始 measure_iaq。
 * 在预热期内发送测量命令，SGP30 会进入异常状态：命令收下了但没有数据可读，
 * 后续每次读都 NACK（软件 I2C 上表现为 ESP_FAIL）。 */
#define SGP30_WARMUP_MS         15000u

static bool s_inited = false;
static TickType_t s_init_tick = 0;

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

    /* BUGFIX: 旧代码只等 10ms，短于手册要求的 15ms */
    vTaskDelay(pdMS_TO_TICKS(SGP30_INIT_DELAY_MS));

    s_init_tick = xTaskGetTickCount();
    s_inited = true;
    ESP_LOGI(TAG, "SGP30 initialized (wait %u ms before first measure)",
             (unsigned)SGP30_WARMUP_MS);
    return ESP_OK;
}

esp_err_t sgp30_read_data(i2c_bus_handle_t bus, uint8_t dev_addr, sgp30_data_t *data) {
    if (!bus || !data) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 预热期直接返回，不去打扰芯片 */
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    const TickType_t elapsed = xTaskGetTickCount() - s_init_tick;
    if ((uint32_t)(elapsed * portTICK_PERIOD_MS) < SGP30_WARMUP_MS) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t cmd[2] = {
        (uint8_t)((SGP30_CMD_MEASURE_AIR_QUALITY >> 8) & 0xFF),
        (uint8_t)(SGP30_CMD_MEASURE_AIR_QUALITY & 0xFF),
    };

    esp_err_t ret = i2c_bus_write(bus, dev_addr, cmd, sizeof(cmd));
    if (ret != ESP_OK) {
        if (sgp30_log_due()) {
            ESP_LOGW(TAG, "measure cmd 0x2008 failed: %s", esp_err_to_name(ret));
        }
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(SGP30_MEASURE_DELAY_MS));

    uint8_t buf[6] = {0};
    ret = i2c_bus_read(bus, dev_addr, buf, sizeof(buf));
    if (ret != ESP_OK) {
        if (sgp30_log_due()) {
            ESP_LOGW(TAG, "read 6B failed: %s", esp_err_to_name(ret));
        }
        return ret;
    }

    if (sgp30_calc_crc(&buf[0]) != buf[2]) {
        if (sgp30_log_due()) {
            ESP_LOGW(TAG, "CO2 word CRC error");
        }
        return ESP_ERR_INVALID_CRC;
    }
    if (sgp30_calc_crc(&buf[3]) != buf[5]) {
        if (sgp30_log_due()) {
            ESP_LOGW(TAG, "TVOC word CRC error");
        }
        return ESP_ERR_INVALID_CRC;
    }

    data->co2_eq = (uint16_t)(((uint16_t)buf[0] << 8) | buf[1]);
    data->tvoc = (uint16_t)(((uint16_t)buf[3] << 8) | buf[4]);
    return ESP_OK;
}
