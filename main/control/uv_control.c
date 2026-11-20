#include "control/uv_control.h"

#include "actuators/uv_lamp.h"
#include "config/param_store.h"
#include "esp_log.h"
#include "service/control_mode.h"
#include "service/schedule.h"

static const char *TAG = "UV_CTRL";

static bool s_initialized = false;

static int minutes_of_day(int hour, int minute) {
    return hour * 60 + minute;
}

static bool minute_in_window(int now_min, int start_min, int end_min) {
    if (start_min == end_min) {
        return false;
    }
    if (start_min < end_min) {
        return (now_min >= start_min) && (now_min < end_min);
    }
    return (now_min >= start_min) || (now_min < end_min);
}

esp_err_t uv_control_init(void) {
    esp_err_t ret = uv_lamp_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UV lamp init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    s_initialized = true;
    ESP_LOGI(TAG, "UV control initialized");
    return ESP_OK;
}

esp_err_t uv_control_update(const pcf8563_time_t *now) {
    if (!s_initialized || !now) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 自动时间窗已取消：紫外完全由「定时计划」控制（SCH,uv）。
     * 计划没启用时保持关闭。保留本函数是为了接口稳定。 */
    return ESP_OK;

    const bool auto_enabled = control_mode_auto_enabled() && param_get_bool(PARAM_IDX_AUTO_UV);
    const int on_min = minutes_of_day((int)P(PARAM_IDX_UV_ON_H), (int)P(PARAM_IDX_UV_ON_M));
    const int off_min = minutes_of_day((int)P(PARAM_IDX_UV_OFF_H), (int)P(PARAM_IDX_UV_OFF_M));
    const int now_min = minutes_of_day(now->hour, now->minute);

    const bool should_on = auto_enabled && minute_in_window(now_min, on_min, off_min);

    if (should_on != uv_lamp_get_state()) {
        esp_err_t ret = uv_lamp_set_state(should_on);
        if (ret != ESP_OK) {
            return ret;
        }
        ESP_LOGW(TAG, "UV lamp %s by schedule at %02u:%02u (window %02d:%02d-%02d:%02d)",
                 should_on ? "ON" : "OFF", now->hour, now->minute,
                 on_min / 60, on_min % 60, off_min / 60, off_min % 60);
    }

    return ESP_OK;
}

bool uv_control_is_on(void) {
    return uv_lamp_get_state();
}
