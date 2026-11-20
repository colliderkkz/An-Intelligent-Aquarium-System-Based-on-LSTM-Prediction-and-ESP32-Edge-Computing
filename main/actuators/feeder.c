#include "feeder.h"

#include "config/param_store.h"
#include "config/pin_config.h"
#include "config/system_config.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "FEEDER";

/* 喂食舵机 WEISHI(3pin)：50Hz / 13bit，0.5~2.5ms 对应 0~180° */
#define FEEDER_LEDC_CHANNEL LEDC_CHANNEL_0

static float s_angle = 0.0f;
static unsigned int s_duty = 0;
static bool s_initialized = false;

static uint32_t angle_to_duty(float angle) {
    /* 舵机方向反了的时候（机械结构左右手不同），把逻辑角度镜像一下：
     * 0° <-> 180°。用参数控制，上位机「参数」页签里改 servo_reverse 即可，
     * 不用重新烧固件。 */
    if (P(PARAM_IDX_SERVO_REVERSE) > 0.5f) {
        angle = SERVO_MAX_ANGLE - angle;
    }
    float clamped = angle;
    if (clamped < 0.0f) {
        clamped = 0.0f;
    }
    if (clamped > SERVO_MAX_ANGLE) {
        clamped = SERVO_MAX_ANGLE;
    }
    return (uint32_t)((float)SERVO_DUTY_MIN +
                      (clamped / SERVO_MAX_ANGLE) * (float)(SERVO_DUTY_MAX - SERVO_DUTY_MIN));
}

esp_err_t feeder_init(void) {
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_MODE,
        .timer_num = LEDC_TIMER_SERVO,
        .duty_resolution = LEDC_SERVO_RESOLUTION,
        .freq_hz = LEDC_SERVO_FREQUENCY,   /* 50Hz (20ms) */
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t ret = ledc_timer_config(&ledc_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ledc_timer_config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ledc_channel_config_t ledc_channel = {
        .speed_mode = LEDC_MODE,
        .channel = FEEDER_LEDC_CHANNEL,
        .timer_sel = LEDC_TIMER_SERVO,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = FEEDER_PIN,
        .duty = 0,
        .hpoint = 0,
    };
    ret = ledc_channel_config(&ledc_channel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ledc_channel_config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_initialized = true;
    (void)feeder_set_angle(0.0f);
    ESP_LOGI(TAG, "Feeder servo initialized on GPIO %d", FEEDER_PIN);
    return ESP_OK;
}

esp_err_t feeder_set_angle(float angle) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t duty = angle_to_duty(angle);
    esp_err_t ret = ledc_set_duty(LEDC_MODE, FEEDER_LEDC_CHANNEL, duty);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = ledc_update_duty(LEDC_MODE, FEEDER_LEDC_CHANNEL);
    if (ret == ESP_OK) {
        s_angle = angle;
        s_duty = (unsigned int)duty;
    }
    return ret;
}

float feeder_get_angle(void) {
    return s_angle;
}

unsigned int feeder_get_duty(void) {
    return s_duty;
}

esp_err_t feeder_feed_now(void) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    float feed_angle = P(PARAM_IDX_FEED_ANGLE);
    uint32_t hold_ms = param_get_u32(PARAM_IDX_FEED_HOLD_MS);
    if (hold_ms < 100) {
        hold_ms = 100;
    }

    ESP_LOGI(TAG, "Feeding started (angle=%.0f, hold=%lums)", feed_angle, (unsigned long)hold_ms);

    esp_err_t ret = feeder_set_angle(feed_angle);
    if (ret != ESP_OK) {
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(hold_ms));

    ret = feeder_set_angle(0.0f);
    vTaskDelay(pdMS_TO_TICKS(hold_ms));

    ESP_LOGI(TAG, "Feeding completed");
    return ret;
}
