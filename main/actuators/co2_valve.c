#include "co2_valve.h"

#include "actuators/gpio_switch.h"
#include "config/param_store.h"
#include "config/pin_config.h"
#include "esp_log.h"
#include "service/sensor_hub.h"

static const char *TAG = "CO2_VALVE";

static gpio_switch_t s_co2 = {
    .pin = CO2_VALVE_PIN,
    .invert_param = PARAM_IDX_INV_CO2,
    .name = "co2_valve",
};

esp_err_t co2_valve_init(void) {
    return gpio_switch_init(&s_co2);
}

/**
 * pH 偏低时开 CO2 会继续酸化。
 * 这个联锁放在执行器层，让 MQTT、定时计划、语音三条入口都被兜住
 * （原来只有 MQTT 命令处理器里检查过，计划和语音会把阀门直接打开）。
 */
static bool co2_blocked_by_ph(void) {
    const sensor_sample_t *s = sensor_hub_get(SH_PH);
    if (!s || !s->valid || s->err != ESP_OK) {
        return false;               /* 没有有效 pH 读数就不拦，避免误伤 */
    }
    return s->value < param_get(PARAM_IDX_PH_MIN);
}

esp_err_t co2_valve_set_state(bool on) {
    if (on && co2_blocked_by_ph()) {
        ESP_LOGW(TAG, "open refused: pH below threshold");
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = gpio_switch_set(&s_co2, on);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "CO2 valve turn %s", on ? "ON" : "OFF");
    }
    return ret;
}

bool co2_valve_get_state(void) {
    return gpio_switch_get(&s_co2);
}

int co2_valve_get_level(void) {
    return gpio_switch_read_level(&s_co2);
}

/* ---- 强度控制（PWM 0~100，0 = 关） ---- */
esp_err_t co2_valve_set_level(uint8_t percent) {
    if (percent > 0 && co2_blocked_by_ph()) {
        ESP_LOGW(TAG, "level refused: pH below threshold");
        return ESP_ERR_INVALID_STATE;
    }
    return gpio_switch_set_percent(&s_co2, percent);
}

uint8_t co2_valve_get_percent(void) {
    return gpio_switch_get_percent(&s_co2);
}
