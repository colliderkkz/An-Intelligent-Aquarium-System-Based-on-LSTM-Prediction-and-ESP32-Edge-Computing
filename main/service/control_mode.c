#include "service/control_mode.h"

#include "config/param_store.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "MODE";

static bool s_manual = false;
static TickType_t s_manual_deadline = 0;

void control_mode_init(void) {
    s_manual = false;
    s_manual_deadline = 0;
    ESP_LOGI(TAG, "control mode: AUTO");
}

bool control_mode_is_manual(void) {
    if (!s_manual) {
        return false;
    }
    /* 用有符号比较判断超时，避免 tick 回绕出错 */
    if ((int32_t)(xTaskGetTickCount() - s_manual_deadline) >= 0) {
        s_manual = false;
        ESP_LOGW(TAG, "manual mode timeout, back to AUTO");
        return false;
    }
    return true;
}

uint32_t control_mode_manual_left_s(void) {
    if (!s_manual) {
        return 0;
    }
    TickType_t now = xTaskGetTickCount();
    if ((int32_t)(now - s_manual_deadline) >= 0) {
        return 0;
    }
    return (uint32_t)(((s_manual_deadline - now) * portTICK_PERIOD_MS) / 1000u);
}

void control_mode_enter_manual(uint32_t timeout_s) {
    if (timeout_s == 0) {
        timeout_s = (uint32_t)param_get_u32(PARAM_IDX_MANUAL_TIMEOUT_S);
    }
    if (timeout_s == 0) {
        timeout_s = 120;
    }
    if (timeout_s > 3600) {
        timeout_s = 3600;
    }

    s_manual_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_s * 1000u);
    if (!s_manual) {
        s_manual = true;
        ESP_LOGW(TAG, "manual mode ON (%us)", (unsigned)timeout_s);
    }
}

void control_mode_exit_manual(void) {
    if (s_manual) {
        s_manual = false;
        ESP_LOGI(TAG, "manual mode OFF, back to AUTO");
    }
}

uint32_t control_mode_manual_remaining_s(void) {
    if (!control_mode_is_manual()) {
        return 0;
    }
    int32_t remain_ticks = (int32_t)(s_manual_deadline - xTaskGetTickCount());
    if (remain_ticks <= 0) {
        return 0;
    }
    return (uint32_t)remain_ticks / configTICK_RATE_HZ;
}

const char *control_mode_name(void) {
    return control_mode_is_manual() ? "MANUAL" : "AUTO";
}

bool control_mode_auto_enabled(void) {
    return param_get_bool(PARAM_IDX_AUTO_ENABLE);
}
