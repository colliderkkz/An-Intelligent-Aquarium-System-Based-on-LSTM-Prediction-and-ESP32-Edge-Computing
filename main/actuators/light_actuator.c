#include "light_actuator.h"

#include "actuators/netlea_lamp.h"
#include "config/param_store.h"
#include "config/pin_config.h"
#include "driver/ledc.h"
#include "esp_log.h"

static const char *TAG = "LIGHT_ACTUATOR";

/* 水草灯用独立的 LEDC 定时器：舵机需要 50Hz，灯不需要那么低，
 * 50Hz 调光会人眼可见闪烁，这里用 5kHz。 */
#define LIGHT_LEDC_CHANNEL LEDC_CHANNEL_1

static float s_brightness = 0.0f;
static unsigned int s_duty = 0;
static bool s_initialized = false;

esp_err_t light_actuator_init(void) {
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_MODE,
        .timer_num = LEDC_TIMER_LIGHT,
        .duty_resolution = LEDC_LIGHT_RESOLUTION,
        .freq_hz = LEDC_LIGHT_FREQUENCY,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t ret = ledc_timer_config(&ledc_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ledc_timer_config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ledc_channel_config_t ledc_channel = {
        .speed_mode = LEDC_MODE,
        .channel = LIGHT_LEDC_CHANNEL,
        .timer_sel = LEDC_TIMER_LIGHT,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = LIGHT_PIN,
        .duty = 0,
        .hpoint = 0,
    };
    ret = ledc_channel_config(&ledc_channel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ledc_channel_config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_initialized = true;
    (void)light_actuator_set_brightness(0.0f);
    ESP_LOGI(TAG, "Light actuator initialized on GPIO %d (%d Hz)", LIGHT_PIN, LEDC_LIGHT_FREQUENCY);
    return ESP_OK;
}

esp_err_t light_actuator_apply_board_duty(float brightness_percent) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (brightness_percent < 0.0f) {
        brightness_percent = 0.0f;
    }
    if (brightness_percent > 100.0f) {
        brightness_percent = 100.0f;
    }

    unsigned int duty = (unsigned int)((brightness_percent / 100.0f) * (float)LED_MAX_DUTY);
    /* 有些底板用 PNP/PMOS 驱动灯板，此时占空比需要取反 */
    if (param_get_bool(PARAM_IDX_INV_LIGHT)) {
        duty = (unsigned int)LED_MAX_DUTY - duty;
    }
    if (duty > (unsigned int)LED_MAX_DUTY) {
        duty = (unsigned int)LED_MAX_DUTY;
    }

    esp_err_t ret = ledc_set_duty(LEDC_MODE, LIGHT_LEDC_CHANNEL, duty);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = ledc_update_duty(LEDC_MODE, LIGHT_LEDC_CHANNEL);
    if (ret == ESP_OK) {
        s_duty = duty;
    }
    return ret;
}

esp_err_t light_actuator_set_brightness(float brightness_percent) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (brightness_percent < 0.0f) {
        brightness_percent = 0.0f;
    }
    if (brightness_percent > 100.0f) {
        brightness_percent = 100.0f;
    }

    /* 光源二选一：选了尼特利就把板载 PWM 输出关掉，
     * 但"逻辑亮度"仍然记在 s_brightness 里 —— 切回板载灯时立刻恢复。
     * 这样计划表 / 手动保持 / App 滑条等原有调用方一行都不用改。 */
    if (lamp_source_get() == LAMP_SRC_NETLEA) {
        (void)light_actuator_apply_board_duty(0.0f);
        s_brightness = brightness_percent;
        return netlea_lamp_set_level((uint8_t)(brightness_percent + 0.5f));
    }

    esp_err_t ret = light_actuator_apply_board_duty(brightness_percent);
    if (ret == ESP_OK) {
        s_brightness = brightness_percent;
    }
    return ret;
}

float light_actuator_get_brightness(void) {
    return s_brightness;
}

unsigned int light_actuator_get_duty(void) {
    return s_duty;
}
