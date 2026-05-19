#include "uv_lamp.h"
#include "config/pin_config.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "UV_LAMP";
static bool s_is_on = false;

esp_err_t uv_lamp_init(void) {
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << UV_LAMP_PIN),
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
    };
    esp_err_t ret = gpio_config(&io_conf);
    if (ret == ESP_OK) {
        gpio_set_level((gpio_num_t)UV_LAMP_PIN, GPIO_ACTIVE_LOW);
        s_is_on = false;
        ESP_LOGI(TAG, "UV lamp initialized to OFF (GPIO %d)", UV_LAMP_PIN);
    }
    return ret;
}

esp_err_t uv_lamp_set_state(bool on) {
    esp_err_t ret = gpio_set_level((gpio_num_t)UV_LAMP_PIN, on ? GPIO_ACTIVE_HIGH : GPIO_ACTIVE_LOW);
    if (ret == ESP_OK) {
        s_is_on = on;
        ESP_LOGI(TAG, "UV lamp turn %s", on ? "ON" : "OFF");
    }
    return ret;
}

bool uv_lamp_get_state(void) {
    return s_is_on;
}
