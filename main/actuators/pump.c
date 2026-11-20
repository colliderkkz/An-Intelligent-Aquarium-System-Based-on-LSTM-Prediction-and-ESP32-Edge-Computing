#include "pump.h"

#include "actuators/gpio_switch.h"
#include "config/pin_config.h"
#include "esp_log.h"

static const char *TAG = "PUMP";

static gpio_switch_t s_pump = {
    .pin = PUMP_PIN,
    .invert_param = PARAM_IDX_INV_PUMP,
    .name = "circulation_pump",
};

esp_err_t pump_init(void) {
    return gpio_switch_init(&s_pump);
}

esp_err_t pump_set_state(bool on) {
    esp_err_t ret = gpio_switch_set(&s_pump, on);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Circulation pump turn %s", on ? "ON" : "OFF");
    }
    return ret;
}

bool pump_get_state(void) {
    return gpio_switch_get(&s_pump);
}

int pump_get_level(void) {
    return gpio_switch_read_level(&s_pump);
}

/* ---- 强度控制（PWM 0~100，0 = 关） ---- */
esp_err_t pump_set_level(uint8_t percent) {
    return gpio_switch_set_percent(&s_pump, percent);
}

uint8_t pump_get_percent(void) {
    return gpio_switch_get_percent(&s_pump);
}
