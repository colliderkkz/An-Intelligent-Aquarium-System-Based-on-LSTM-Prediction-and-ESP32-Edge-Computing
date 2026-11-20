#include "onewire_bus.h"
#include "config/pin_config.h"
#include "driver/gpio.h"
#include "rom/ets_sys.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ONEWIRE_BUS";

/*
 * BUGFIX: 旧代码把 portMUX_TYPE 定义在函数内部（局部变量），每次调用都是一个
 * 全新的锁，既不能互斥、又会让 taskENTER_CRITICAL 记录错误的嵌套计数。
 * 这里改为文件级静态锁，所有 1-Wire 时序共享同一把自旋锁。
 */
static portMUX_TYPE s_onewire_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_initialized = false;

esp_err_t onewire_bus_init(void) {
    if (s_initialized) {
        return ESP_OK;
    }

    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT_OUTPUT_OD, // 开漏输出，外部需接 4.7K 上拉电阻
        .pin_bit_mask = (1ULL << ONE_WIRE_BUS),
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_ENABLE,  // 内部上拉很弱，仍然必须外接 4.7K
    };
    esp_err_t ret = gpio_config(&io_conf);
    if (ret == ESP_OK) {
        gpio_set_level((gpio_num_t)ONE_WIRE_BUS, 1);
        vTaskDelay(pdMS_TO_TICKS(10));
        s_initialized = true;
        ESP_LOGI(TAG, "OneWire bus initialized on GPIO %d", ONE_WIRE_BUS);
    }
    return ret;
}

static inline void onewire_set_low(void) {
    gpio_set_level((gpio_num_t)ONE_WIRE_BUS, 0);
}

static inline void onewire_set_high(void) {
    gpio_set_level((gpio_num_t)ONE_WIRE_BUS, 1);
}

static inline int onewire_get_level(void) {
    return gpio_get_level((gpio_num_t)ONE_WIRE_BUS);
}

bool onewire_bus_reset(void) {
    bool presence = false;

    taskENTER_CRITICAL(&s_onewire_mux);

    onewire_set_low();
    ets_delay_us(480);          // TX Reset pulse，最小 480us
    onewire_set_high();
    ets_delay_us(80);           // DS18B20 在 15~60us 后拉低，持续 60~240us

    if (onewire_get_level() == 0) {
        presence = true;        // 检测到存在脉冲
    }

    ets_delay_us(480);          // 释放总线并等待复位周期结束(总时长 > 960us)
    taskEXIT_CRITICAL(&s_onewire_mux);

    return presence;
}

void onewire_bus_write_bit(uint8_t bit) {
    taskENTER_CRITICAL(&s_onewire_mux);

    if (bit) {
        // Write 1：拉低 6us 后释放，整个时隙 >= 60us
        onewire_set_low();
        ets_delay_us(6);
        onewire_set_high();
        ets_delay_us(64);
    } else {
        // Write 0：拉低 60us 后释放
        onewire_set_low();
        ets_delay_us(60);
        onewire_set_high();
        ets_delay_us(10);
    }

    taskEXIT_CRITICAL(&s_onewire_mux);
}

uint8_t onewire_bus_read_bit(void) {
    uint8_t bit = 0;

    taskENTER_CRITICAL(&s_onewire_mux);

    // Read 时隙：拉低 >=1us 后释放，主机必须在 15us 内采样
    onewire_set_low();
    ets_delay_us(3);
    onewire_set_high();
    ets_delay_us(10);           // 总延时约 13us，留出余量

    bit = (uint8_t)(onewire_get_level() & 0x01);
    ets_delay_us(53);           // 补足 60us 时隙

    taskEXIT_CRITICAL(&s_onewire_mux);

    return bit;
}

void onewire_bus_write_byte(uint8_t data) {
    for (int i = 0; i < 8; i++) {
        onewire_bus_write_bit((uint8_t)(data & 0x01));
        data >>= 1;
    }
}

uint8_t onewire_bus_read_byte(void) {
    uint8_t data = 0;
    for (int i = 0; i < 8; i++) {
        if (onewire_bus_read_bit()) {
            data |= (uint8_t)(1u << i);
        }
    }
    return data;
}
