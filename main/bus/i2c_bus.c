/**
 * @file i2c_bus.c
 * @brief 软件模拟（bit-bang）I2C 总线
 *
 * 为什么不用 ESP-IDF 的 i2c_master：
 *   IDF 5.4 的新 I2C 驱动对 SGP30 这种"先写命令 → 等 12ms → 再裸读 6 字节"
 *   的器件不兼容：
 *     - trans_queue_depth=0（同步）  -> i2c_master_receive 一律 ESP_ERR_INVALID_STATE
 *       （驱动内部 s_i2c_send_commands 之后检查 status != I2C_STATUS_DONE）
 *     - trans_queue_depth>0（异步）  -> 事务槽位用完不释放，几秒钟就 "ops list is full"
 *   而 legacy 驱动（driver/i2c.h）在 IDF 5.4 里已经被移除。
 *
 *   所以这里直接用 GPIO 开漏 + 上拉模拟 I2C：时序完全可控，
 *   PCF8563（寄存器式）和 SGP30（命令式）都能正常工作。
 *   板子上 SDA/SCL 到 3.3V 有 4.7k 上拉，开漏模式写 1 即释放总线。
 */
#include "i2c_bus.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "I2C_SW";

struct i2c_bus_t {
    gpio_num_t sda;
    gpio_num_t scl;
    uint32_t clk_speed_hz;
    uint32_t half_us;       /* 由 clk_speed_hz 换算出的半周期 */
    uint32_t timeout_ms;
    SemaphoreHandle_t lock;
    int last_error;
};

static inline void sw_delay(i2c_bus_handle_t b) {
    esp_rom_delay_us(b->half_us);
}

static inline void sda_set(i2c_bus_handle_t b, int v) {
    gpio_set_level(b->sda, v ? 1 : 0);
}

static inline void scl_set(i2c_bus_handle_t b, int v) {
    gpio_set_level(b->scl, v ? 1 : 0);
}

static inline int sda_get(i2c_bus_handle_t b) {
    return gpio_get_level(b->sda) ? 1 : 0;
}

static inline int scl_get(i2c_bus_handle_t b) {
    return gpio_get_level(b->scl) ? 1 : 0;
}

static void sw_stop(i2c_bus_handle_t b);

/* 从机异常（掉电/热插拔/复位中途）可能把 SDA 拉死在低电平，
 * 之后所有事务都会失败，必须打 9 个 SCL 时钟把它"顶"出来，再补一个 STOP
 * 让从机状态机回到空闲。这里只在初始化失败/事务失败时调用，不影响正常时序。 */
static void sw_bus_recover(i2c_bus_handle_t b) {
    if (!b) {
        return;
    }
    sda_set(b, 1);                 /* 释放 SDA，交给上拉 */
    for (int i = 0; i < 9 && !sda_get(b); i++) {
        scl_set(b, 0);
        sw_delay(b);
        scl_set(b, 1);
        sw_delay(b);
    }
    scl_set(b, 0);
    sw_delay(b);
    sw_stop(b);                    /* 补一个 STOP，复位从机状态机 */
}

static void sw_start(i2c_bus_handle_t b) {
    sda_set(b, 1);
    scl_set(b, 1);
    sw_delay(b);
    sda_set(b, 0);      /* SCL 高时 SDA 下降 = START */
    sw_delay(b);
    scl_set(b, 0);
}

static void sw_stop(i2c_bus_handle_t b) {
    sda_set(b, 0);
    sw_delay(b);
    scl_set(b, 1);
    sw_delay(b);
    sda_set(b, 1);      /* SCL 高时 SDA 上升 = STOP */
    sw_delay(b);
}

static bool sw_write_byte(i2c_bus_handle_t b, uint8_t v) {
    for (int i = 0; i < 8; i++) {
        sda_set(b, (v & 0x80) ? 1 : 0);
        v = (uint8_t)(v << 1);
        sw_delay(b);
        scl_set(b, 1);
        sw_delay(b);
        scl_set(b, 0);
    }
    /* 第 9 个时钟：释放 SDA 读从机 ACK（低电平有效） */
    sda_set(b, 1);
    sw_delay(b);
    scl_set(b, 1);
    sw_delay(b);
    const bool ack = (sda_get(b) == 0);
    scl_set(b, 0);
    return ack;
}

static uint8_t sw_read_byte(i2c_bus_handle_t b, bool ack) {
    uint8_t v = 0;
    sda_set(b, 1);      /* 释放 SDA 由从机驱动 */
    for (int i = 0; i < 8; i++) {
        sw_delay(b);
        scl_set(b, 1);
        sw_delay(b);
        v = (uint8_t)((v << 1) | (sda_get(b) ? 1 : 0));
        scl_set(b, 0);
    }
    /* 第 9 个时钟：主机给 ACK(0) / NACK(1) */
    sda_set(b, ack ? 0 : 1);
    sw_delay(b);
    scl_set(b, 1);
    sw_delay(b);
    scl_set(b, 0);
    sda_set(b, 1);
    return v;
}

/* ---------------- 对外接口 ---------------- */

i2c_bus_handle_t i2c_bus_init(const i2c_bus_config_t *config) {
    if (!config) {
        return NULL;
    }

    i2c_bus_handle_t bus = calloc(1, sizeof(*bus));
    if (!bus) {
        return NULL;
    }
    bus->sda = config->sda_io_num;
    bus->scl = config->scl_io_num;
    bus->clk_speed_hz = config->clk_speed_hz;
    bus->timeout_ms = config->timeout_ms;

    /* 速度真正生效：半周期 = 500000 / 频率(Hz)。
     * 之前这里写死 4us，实际跑 ~110kHz，超过 SGP30 手册的 100kHz 上限，
     * 而配置里的 50kHz 完全没被使用。这里按配置换算并夹在 2~100us。 */
    uint32_t half = config->clk_speed_hz ? (500000u / config->clk_speed_hz) : 5u;
    if (half < 2u) {
        half = 2u;
    }
    if (half > 100u) {
        half = 100u;
    }
    bus->half_us = half;

    bus->lock = xSemaphoreCreateMutex();
    if (!bus->lock) {
        free(bus);
        return NULL;
    }

    /* 开漏输入输出：写 1 释放（由上拉拉高），写 0 拉低；同时能读回电平。
     * 板上已有 4.7k 外部上拉，内部上拉只是兜底，按配置开关。 */
    gpio_config_t io = {
        .pin_bit_mask = ((uint64_t)1 << bus->sda) | ((uint64_t)1 << bus->scl),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = config->pullup_enable ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&io);
    if (ret != ESP_OK) {
        vSemaphoreDelete(bus->lock);
        free(bus);
        return NULL;
    }

    sda_set(bus, 1);
    scl_set(bus, 1);
    vTaskDelay(pdMS_TO_TICKS(5));

    /* 上电自检：两条线释放后应为高。为低说明总线被拽住/短路/没上拉，
     * 先尝试恢复，恢复不了就明确报出来（比后面一堆 NACK 好查得多）。 */
    if (!sda_get(bus) || !scl_get(bus)) {
        ESP_LOGW(TAG, "bus not idle (SDA=%d SCL=%d), trying recovery",
                 sda_get(bus), scl_get(bus));
        sw_bus_recover(bus);
        vTaskDelay(pdMS_TO_TICKS(2));
        if (!sda_get(bus) || !scl_get(bus)) {
            ESP_LOGE(TAG, "bus stuck LOW after recovery (SDA=%d SCL=%d) "
                          "-> check pull-ups / wiring / powered sensors",
                     sda_get(bus), scl_get(bus));
        } else {
            ESP_LOGW(TAG, "bus recovered to idle");
        }
    }

    ESP_LOGI(TAG, "software I2C ready: SDA=%d SCL=%d, %lu Hz (half=%lu us)",
             (int)bus->sda, (int)bus->scl,
             (unsigned long)config->clk_speed_hz, (unsigned long)bus->half_us);
    return bus;
}

esp_err_t i2c_bus_deinit(i2c_bus_handle_t bus) {
    if (!bus) {
        return ESP_ERR_INVALID_ARG;
    }
    if (bus->lock) {
        vSemaphoreDelete(bus->lock);
    }
    free(bus);
    return ESP_OK;
}

esp_err_t i2c_bus_write(i2c_bus_handle_t bus, uint8_t dev_addr,
                        uint8_t *data, size_t len) {
    if (!bus || !data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(bus->lock, pdMS_TO_TICKS(bus->timeout_ms)) != pdTRUE) {
        bus->last_error = I2C_ERR_TIMEOUT;
        return ESP_ERR_TIMEOUT;   /* 拿不到总线就别再 bit-bang，更不能乱 Give */
    }

    sw_start(bus);
    bool ok = sw_write_byte(bus, (uint8_t)(dev_addr << 1));      /* W */
    for (size_t i = 0; ok && i < len; i++) {
        ok = sw_write_byte(bus, data[i]);
    }
    sw_stop(bus);
    if (!ok) {
        sw_bus_recover(bus);   /* 从机 NACK 后补一次恢复，别让总线被拽死 */
    }

    xSemaphoreGive(bus->lock);
    if (!ok) {
        bus->last_error = I2C_ERR_WRITE_FAIL;
        return ESP_FAIL;      /* 从机 NACK */
    }
    return ESP_OK;
}

esp_err_t i2c_bus_read(i2c_bus_handle_t bus, uint8_t dev_addr,
                       uint8_t *data, size_t len) {
    if (!bus || !data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(bus->lock, pdMS_TO_TICKS(bus->timeout_ms)) != pdTRUE) {
        bus->last_error = I2C_ERR_TIMEOUT;
        return ESP_ERR_TIMEOUT;   /* 拿不到总线就别再 bit-bang，更不能乱 Give */
    }

    sw_start(bus);
    bool ok = sw_write_byte(bus, (uint8_t)((dev_addr << 1) | 1));  /* R */
    if (ok) {
        for (size_t i = 0; i < len; i++) {
            data[i] = sw_read_byte(bus, (i + 1) < len);           /* 最后一字节 NACK */
        }
    }
    sw_stop(bus);
    if (!ok) {
        sw_bus_recover(bus);   /* 从机 NACK 后补一次恢复，别让总线被拽死 */
    }

    xSemaphoreGive(bus->lock);
    if (!ok) {
        bus->last_error = I2C_ERR_READ_FAIL;
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t i2c_bus_read_reg8(i2c_bus_handle_t bus, uint8_t dev_addr,
                            uint8_t reg_addr, uint8_t *data, size_t len) {
    if (!bus || !data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(bus->lock, pdMS_TO_TICKS(bus->timeout_ms)) != pdTRUE) {
        bus->last_error = I2C_ERR_TIMEOUT;
        return ESP_ERR_TIMEOUT;   /* 拿不到总线就别再 bit-bang，更不能乱 Give */
    }

    sw_start(bus);
    bool ok = sw_write_byte(bus, (uint8_t)(dev_addr << 1));      /* W */
    if (ok) {
        ok = sw_write_byte(bus, reg_addr);
    }
    if (ok) {
        sw_start(bus);                                            /* repeated START */
        ok = sw_write_byte(bus, (uint8_t)((dev_addr << 1) | 1));  /* R */
        if (ok) {
            for (size_t i = 0; i < len; i++) {
                data[i] = sw_read_byte(bus, (i + 1) < len);
            }
        }
    }
    sw_stop(bus);
    if (!ok) {
        sw_bus_recover(bus);   /* 从机 NACK 后补一次恢复，别让总线被拽死 */
    }

    xSemaphoreGive(bus->lock);
    if (!ok) {
        bus->last_error = I2C_ERR_READ_FAIL;
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t i2c_bus_write_reg8(i2c_bus_handle_t bus, uint8_t dev_addr,
                             uint8_t reg_addr, uint8_t *data, size_t len) {
    if (!bus || !data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(bus->lock, pdMS_TO_TICKS(bus->timeout_ms)) != pdTRUE) {
        bus->last_error = I2C_ERR_TIMEOUT;
        return ESP_ERR_TIMEOUT;   /* 拿不到总线就别再 bit-bang，更不能乱 Give */
    }

    sw_start(bus);
    bool ok = sw_write_byte(bus, (uint8_t)(dev_addr << 1));      /* W */
    if (ok) {
        ok = sw_write_byte(bus, reg_addr);
    }
    for (size_t i = 0; ok && i < len; i++) {
        ok = sw_write_byte(bus, data[i]);
    }
    sw_stop(bus);
    if (!ok) {
        sw_bus_recover(bus);   /* 从机 NACK 后补一次恢复，别让总线被拽死 */
    }

    xSemaphoreGive(bus->lock);
    if (!ok) {
        bus->last_error = I2C_ERR_WRITE_FAIL;
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool i2c_bus_probe(i2c_bus_handle_t bus, uint8_t dev_addr) {
    if (!bus) {
        return false;
    }
    if (xSemaphoreTake(bus->lock, pdMS_TO_TICKS(bus->timeout_ms)) != pdTRUE) {
        bus->last_error = I2C_ERR_TIMEOUT;
        return false;   /* bool 函数不能把错误码当 true 返回 */
    }
    /* 总线被从机拽死在低电平时，任何地址都会读到 ACK（假阳性），
     * 这里先确认空闲电平，免得扫描结果满屏都是不存在的器件。 */
    if (!sda_get(bus) || !scl_get(bus)) {
        bus->last_error = I2C_ERR_INIT_FAIL;
        return false;
    }

    sw_start(bus);
    const bool ack = sw_write_byte(bus, (uint8_t)(dev_addr << 1));
    sw_stop(bus);
    xSemaphoreGive(bus->lock);
    return ack;
}

int i2c_bus_scan(i2c_bus_handle_t bus, uint8_t *addr_list, int max_count) {
    if (!bus || !addr_list || max_count <= 0) {
        return 0;
    }
    /* 扫描前先确认总线空闲；被拽死就尝试恢复，仍不行直接返回 0，
     * 避免把「SDA 恒低」误报成扫到一堆器件。 */
    if (xSemaphoreTake(bus->lock, pdMS_TO_TICKS(bus->timeout_ms)) == pdTRUE) {
        bool stuck = (!sda_get(bus) || !scl_get(bus));
        if (stuck) {
            sw_bus_recover(bus);
            stuck = (!sda_get(bus) || !scl_get(bus));
        }
        xSemaphoreGive(bus->lock);
        if (stuck) {
            bus->last_error = I2C_ERR_INIT_FAIL;
            ESP_LOGE(TAG, "scan aborted: bus stuck LOW (SDA=%d SCL=%d)",
                     sda_get(bus), scl_get(bus));
            return 0;
        }
    }

    int count = 0;
    for (uint8_t addr = 0x08; addr < 0x78 && count < max_count; addr++) {
        if (i2c_bus_probe(bus, addr)) {
            addr_list[count++] = addr;
        }
    }
    return count;
}

esp_err_t i2c_bus_read_u16(i2c_bus_handle_t bus, uint8_t dev_addr,
                           uint8_t reg_addr, uint16_t *value) {
    if (!value) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t data[2] = {0};
    esp_err_t ret = i2c_bus_read_reg8(bus, dev_addr, reg_addr, data, sizeof(data));
    if (ret == ESP_OK) {
        *value = (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
    }
    return ret;
}

esp_err_t i2c_bus_write_u16(i2c_bus_handle_t bus, uint8_t dev_addr,
                            uint8_t reg_addr, uint16_t value) {
    uint8_t data[2] = {(uint8_t)(value >> 8), (uint8_t)(value & 0xFF)};
    return i2c_bus_write_reg8(bus, dev_addr, reg_addr, data, sizeof(data));
}

int i2c_bus_last_error(i2c_bus_handle_t bus) {
    return bus ? bus->last_error : I2C_ERR_INIT_FAIL;
}
