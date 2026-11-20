#include "air_pump.h"

#include "actuators/gpio_switch.h"
#include "config/pin_config.h"
#include "esp_log.h"

static const char *TAG = "AIR_PUMP";

static gpio_switch_t s_air = {
    .pin = AIR_PUMP_PIN,
    .invert_param = PARAM_IDX_INV_AIR,
    .name = "air_pump",
};

bool air_pump_is_enabled(void) {
#if AIR_PUMP_ENABLED
    return true;
#else
    return false;
#endif
}

esp_err_t air_pump_init(void) {
#if AIR_PUMP_ENABLED
    return gpio_switch_init(&s_air);
#else
    ESP_LOGW(TAG, "air pump disabled by config (AIR_PUMP_ENABLED=0)");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t air_pump_set_state(bool on) {
#if AIR_PUMP_ENABLED
    esp_err_t ret = gpio_switch_set(&s_air, on);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Air pump turn %s", on ? "ON" : "OFF");
    }
    return ret;
#else
    (void)on;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

bool air_pump_get_state(void) {
#if AIR_PUMP_ENABLED
    return gpio_switch_get(&s_air);
#else
    return false;
#endif
}

int air_pump_get_level(void) {
#if AIR_PUMP_ENABLED
    return gpio_switch_read_level(&s_air);
#else
    return -1;
#endif
}

/* ---- 强度控制（PWM 0~100，0 = 关） ---- */
esp_err_t air_pump_set_level(uint8_t percent) {
    return gpio_switch_set_percent(&s_air, percent);
}

uint8_t air_pump_get_percent(void) {
    return gpio_switch_get_percent(&s_air);
}
