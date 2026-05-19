#include "feeder.h"
#include "config/pin_config.h"
#include "config/system_config.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "FEEDER";

// 舵机 PWM 通道设置
#define FEEDER_LEDC_CHANNEL LEDC_CHANNEL_0

esp_err_t feeder_init(void) {
    // 配置定时器
    ledc_timer_config_t ledc_timer = {
        .speed_mode       = LEDC_MODE,
        .timer_num        = LEDC_TIMER,
        .duty_resolution  = LEDC_RESOLUTION,
        .freq_hz          = LEDC_FREQUENCY,  // 50Hz (20ms)
        .clk_cfg          = LEDC_AUTO_CLK
    };
    esp_err_t ret = ledc_timer_config(&ledc_timer);
    if (ret != ESP_OK) return ret;

    // 配置通道
    ledc_channel_config_t ledc_channel = {
        .speed_mode     = LEDC_MODE,
        .channel        = FEEDER_LEDC_CHANNEL,
        .timer_sel      = LEDC_TIMER,
        .intr_type      = LEDC_INTR_DISABLE,
        .gpio_num       = FEEDER_PIN,
        .duty           = 0,
        .hpoint         = 0
    };
    ret = ledc_channel_config(&ledc_channel);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Feeder servo initialized on GPIO %d", FEEDER_PIN);
    }
    
    // 默认回到0度
    feeder_set_angle(0.0f);
    
    return ret;
}

esp_err_t feeder_set_angle(float angle) {
    if (angle < 0.0f) angle = 0.0f;
    if (angle > SERVO_MAX_ANGLE) angle = SERVO_MAX_ANGLE;

    // 50Hz (20ms cycle). 13-bit resolution (8192).
    // 0 deg = 0.5ms = 2.5% duty ratio
    // 180 deg = 2.5ms = 12.5% duty ratio
    // 2.5% of 8192 = 205
    // 12.5% of 8192 = 1024
    uint32_t duty = (uint32_t)((float)SERVO_DUTY_MIN
                    + (angle / SERVO_MAX_ANGLE) * (float)(SERVO_DUTY_MAX - SERVO_DUTY_MIN));

    ledc_set_duty(LEDC_MODE, FEEDER_LEDC_CHANNEL, duty);
    return ledc_update_duty(LEDC_MODE, FEEDER_LEDC_CHANNEL);
}

esp_err_t feeder_feed_now(void) {
    ESP_LOGI(TAG, "Feeding started (angle=%.1f)...", SERVO_FEED_ANGLE);
    
    esp_err_t ret = feeder_set_angle(SERVO_FEED_ANGLE);
    if (ret != ESP_OK) return ret;
    
    // 等待舵机转动并倒出饲料
    vTaskDelay(pdMS_TO_TICKS(FEEDER_RETURN_DELAY_MS));
    
    // 复位
    ret = feeder_set_angle(0.0f);
    
    // 等待复位完成
    vTaskDelay(pdMS_TO_TICKS(FEEDER_RETURN_DELAY_MS));
    
    ESP_LOGI(TAG, "Feeding completed");
    return ret;
}
