#include "water_pump.h"

#include "actuators/gpio_switch.h"
#include "config/pin_config.h"
#include "esp_log.h"

static const char *TAG = "WATER_PUMP";

static gpio_switch_t s_refill = {
    .pin = WATER_PUMP_PIN,
    .invert_param = PARAM_IDX_INV_REFILL,
    .name = "refill_pump",
};

esp_err_t water_pump_init(void) {
    return gpio_switch_init(&s_refill);
}

esp_err_t water_pump_set_state(bool on) {
    const bool was = s_refill.state;
    esp_err_t ret = gpio_switch_set(&s_refill, on);
    /* 只有真的变了才打日志：补水控制每个周期都会调一次关闭，
     * 不判断的话 2 秒一条 "Refill pump turn OFF" 把 SD 日志和实时推送灌满 */
    if (ret == ESP_OK && was != on) {
        ESP_LOGI(TAG, "Refill pump turn %s", on ? "ON" : "OFF");
    }
    return ret;
}

bool water_pump_get_state(void) {
    return gpio_switch_get(&s_refill);
}

int water_pump_get_level(void) {
    return gpio_switch_read_level(&s_refill);
}

/* ---- 强度控制（PWM 0~100，0 = 关） ---- */
esp_err_t water_pump_set_level(uint8_t percent) {
    return gpio_switch_set_percent(&s_refill, percent);
}

uint8_t water_pump_get_percent(void) {
    return gpio_switch_get_percent(&s_refill);
}
