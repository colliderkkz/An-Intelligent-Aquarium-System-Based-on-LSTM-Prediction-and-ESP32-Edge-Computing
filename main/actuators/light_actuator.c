#include "light_actuator.h"
#include "config/pin_config.h"
#include "driver/ledc.h"
#include "esp_log.h"

static const char *TAG = "LIGHT_ACTUATOR";

// 水草灯 PWM 通道设置，避免和舵机使用同一个通道
#define LIGHT_LEDC_CHANNEL LEDC_CHANNEL_1

esp_err_t light_actuator_init(void) {
    // 水草灯也可以复用同一个 TIMER，只是通道不同
    // 但是这里如果不确定，直接使用预先定义的宏
    ledc_timer_config_t ledc_timer = {
        .speed_mode       = LEDC_MODE,
        .timer_num        = LEDC_TIMER,
        .duty_resolution  = LEDC_RESOLUTION,
        .freq_hz          = LEDC_FREQUENCY,  // 50Hz或者更高。为了不闪烁可考虑更高频率，如果不支持复用定时器需要分离
        .clk_cfg          = LEDC_AUTO_CLK
    };
    esp_err_t ret = ledc_timer_config(&ledc_timer);
    if (ret != ESP_OK) return ret;

    // 配置通道
    ledc_channel_config_t ledc_channel = {
        .speed_mode     = LEDC_MODE,
        .channel        = LIGHT_LEDC_CHANNEL,
        .timer_sel      = LEDC_TIMER,
        .intr_type      = LEDC_INTR_DISABLE,
        .gpio_num       = LIGHT_PIN,
        .duty           = 0,
        .hpoint         = 0
    };
    ret = ledc_channel_config(&ledc_channel);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Light actuator initialized on GPIO %d", LIGHT_PIN);
    }
    
    // 默认关闭
    light_actuator_set_brightness(0.0f);
    
    return ret;
}

esp_err_t light_actuator_set_brightness(float brightness_percent) {
    if (brightness_percent < 0.0f) brightness_percent = 0.0f;
    if (brightness_percent > 100.0f) brightness_percent = 100.0f;

    // 根据百分比计算亮度 (0 ~ 8191)
    uint32_t duty = (uint32_t)((brightness_percent / 100.0f) * LED_MAX_DUTY);

    ledc_set_duty(LEDC_MODE, LIGHT_LEDC_CHANNEL, duty);
    return ledc_update_duty(LEDC_MODE, LIGHT_LEDC_CHANNEL);
}
