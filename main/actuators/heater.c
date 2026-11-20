#include "heater.h"

#include "actuators/gpio_switch.h"
#include "config/pin_config.h"
#include "esp_log.h"

static const char *TAG = "HEATER";

static gpio_switch_t s_heater = {
    .pin = HEATER_PIN,
    .invert_param = PARAM_IDX_INV_HEATER,
    .name = "heater",
};

esp_err_t heater_init(void) {
    return gpio_switch_init(&s_heater);
}

esp_err_t heater_set_state(bool on) {
    esp_err_t ret = gpio_switch_set(&s_heater, on);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Heater turn %s", on ? "ON" : "OFF");
    }
    return ret;
}

bool heater_get_state(void) {
    return gpio_switch_get(&s_heater);
}

int heater_get_level(void) {
    return gpio_switch_read_level(&s_heater);
}

/* ---- 强度控制（PWM 0~100，0 = 关） ---- */
esp_err_t heater_set_level(uint8_t percent) {
    return gpio_switch_set_percent(&s_heater, percent);
}

uint8_t heater_get_percent(void) {
    return gpio_switch_get_percent(&s_heater);
}
