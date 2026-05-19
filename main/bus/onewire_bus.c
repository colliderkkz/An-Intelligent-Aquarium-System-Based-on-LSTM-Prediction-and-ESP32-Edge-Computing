#include "onewire_bus.h"
#include "config/pin_config.h"
#include "driver/gpio.h"
#include "rom/ets_sys.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ONEWIRE_BUS";

esp_err_t onewire_bus_init(void) {
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT_OUTPUT_OD, // 开漏输出，外部需接 4.7K 上拉电阻
        .pin_bit_mask = (1ULL << ONE_WIRE_BUS),
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_ENABLE,  // 启用内部上拉，但依然强烈建议外部上拉
    };
    esp_err_t ret = gpio_config(&io_conf);
    if (ret == ESP_OK) {
        gpio_set_level((gpio_num_t)ONE_WIRE_BUS, 1);
        // 让总线稳定
        vTaskDelay(pdMS_TO_TICKS(10));
        ESP_LOGI(TAG, "OneWire bus initialized on GPIO %d", ONE_WIRE_BUS);
    }
    return ret;
}

static void onewire_set_low(void) {
    gpio_set_level((gpio_num_t)ONE_WIRE_BUS, 0);
}

static void onewire_set_high(void) {
    gpio_set_level((gpio_num_t)ONE_WIRE_BUS, 1);
}

static int onewire_get_level(void) {
    return gpio_get_level((gpio_num_t)ONE_WIRE_BUS);
}

bool onewire_bus_reset(void) {
    bool presence = false;
    
    portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    taskENTER_CRITICAL(&mux);
    
    onewire_set_low();
    ets_delay_us(480); // TX Reset pulse (最少480us)
    onewire_set_high();
    ets_delay_us(70);  // 等待设备拉低总线 (15~60us 后开始拉低，持续 60~240us)
    
    if (onewire_get_level() == 0) {
        presence = true; // 检测到设备拉低了引脚
    }
    
    taskEXIT_CRITICAL(&mux);
    
    // 等待复位周期结束并且总线恢复拉高状态
    ets_delay_us(410); 
    
    return presence;
}

void onewire_bus_write_bit(uint8_t bit) {
    portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    taskENTER_CRITICAL(&mux);
    
    if (bit) {
        // Write 1 周期
        onewire_set_low();
        ets_delay_us(6);
        onewire_set_high();
        ets_delay_us(64);
    } else {
        // Write 0 周期
        onewire_set_low();
        ets_delay_us(60);
        onewire_set_high();
        ets_delay_us(10);
    }
    
    taskEXIT_CRITICAL(&mux);
}

uint8_t onewire_bus_read_bit(void) {
    uint8_t bit = 0;
    
    portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    taskENTER_CRITICAL(&mux);
    
    // Read 周期开始
    onewire_set_low();
    ets_delay_us(6);
    onewire_set_high();
    ets_delay_us(9); // 等待接近读时窗中心
    
    bit = onewire_get_level();
    
    taskEXIT_CRITICAL(&mux);
    
    ets_delay_us(55); // 完成 Read 周期
    return bit;
}

void onewire_bus_write_byte(uint8_t data) {
    for (int i = 0; i < 8; i++) {
        onewire_bus_write_bit(data & 0x01);
        data >>= 1;
    }
}

uint8_t onewire_bus_read_byte(void) {
    uint8_t data = 0;
    for (int i = 0; i < 8; i++) {
        if (onewire_bus_read_bit()) {
            data |= (1 << i);
        }
    }
    return data;
}
