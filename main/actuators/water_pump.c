#include "water_pump.h"
#include "config/pin_config.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "WATER_PUMP";
static bool s_is_on = false;

esp_err_t water_pump_init(void) {
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << WATER_PUMP_PIN),
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
    };
    esp_err_t ret = gpio_config(&io_conf);
    if (ret == ESP_OK) {
        gpio_set_level((gpio_num_t)WATER_PUMP_PIN, GPIO_ACTIVE_LOW);
        s_is_on = false;
        ESP_LOGI(TAG, "Refill pump initialized to OFF");
    }
    return ret;
}

esp_err_t water_pump_set_state(bool on) {
    esp_err_t ret = gpio_set_level((gpio_num_t)WATER_PUMP_PIN, on ? GPIO_ACTIVE_HIGH : GPIO_ACTIVE_LOW);
    if (ret == ESP_OK) {
        s_is_on = on;
        ESP_LOGI(TAG, "Refill pump turn %s", on ? "ON" : "OFF");
    }
    return ret;
}

bool water_pump_get_state(void) {
    return s_is_on;
}
