#include "uv_lamp.h"

#include "actuators/gpio_switch.h"
#include "config/pin_config.h"
#include "esp_log.h"

static const char *TAG = "UV_LAMP";

static gpio_switch_t s_uv = {
    .pin = UV_LAMP_PIN,
    .invert_param = PARAM_IDX_INV_UV,
    .name = "uv_lamp",
};

esp_err_t uv_lamp_init(void) {
    return gpio_switch_init(&s_uv);
}

esp_err_t uv_lamp_set_state(bool on) {
    esp_err_t ret = gpio_switch_set(&s_uv, on);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "UV lamp turn %s", on ? "ON" : "OFF");
    }
    return ret;
}

bool uv_lamp_get_state(void) {
    return gpio_switch_get(&s_uv);
}

int uv_lamp_get_level(void) {
    return gpio_switch_read_level(&s_uv);
}

/* ---- 强度控制（PWM 0~100，0 = 关） ---- */
esp_err_t uv_lamp_set_level(uint8_t percent) {
    return gpio_switch_set_percent(&s_uv, percent);
}

uint8_t uv_lamp_get_percent(void) {
    return gpio_switch_get_percent(&s_uv);
}
