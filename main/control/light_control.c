#include "control/light_control.h"

#include "actuators/co2_valve.h"
#include "actuators/light_actuator.h"
#include "config/error_codes.h"
#include "config/param_store.h"
#include "config/system_config.h"
#include "esp_log.h"
#include "service/control_mode.h"
#include "service/schedule.h"
#include "service/time_service.h"
#include <stdbool.h>

static const char *TAG = "LIGHT_CTRL";

static bool s_initialized = false;
static bool s_last_should_on = false;

/* 把 "时:分" 折算成当日分钟数 */
esp_err_t light_control_init(void) {
    esp_err_t ret = light_actuator_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Light actuator init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = co2_valve_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "CO2 valve init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_initialized = true;
    s_last_should_on = false;
    ESP_LOGI(TAG, "Light control initialized");
    return ESP_OK;
}

esp_err_t light_control_update(const pcf8563_time_t *now) {
    (void)now;
    if (!s_initialized) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 自动时间窗已取消：水草灯（以及跟随的 CO2）完全由「定时计划」控制，
     * 计划没启用时这一路保持关闭，不再按 light_on/off 参数自动开关。
     * 原来 return 之后还留着一整段不可达代码（参数 light_on/off、CO2 跟随），
     * 容易让人以为删了 return 就能生效 —— 这里直接删干净。
     * 保留本函数是为了接口稳定（main 的控制循环仍然会调它）。 */
    return ESP_OK;
}

bool light_control_is_light_on(void) {
    return light_actuator_get_brightness() > 0.5f;
}
