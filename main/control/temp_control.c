#include "control/temp_control.h"

#include "actuators/heater.h"
#include "config/error_codes.h"
#include "config/param_store.h"
#include "config/system_config.h"
#include "drivers/ds18b20.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "service/control_mode.h"
#include "service/act_hold.h"
#include "service/sensor_hub.h"

static const char *TAG = "TEMP_CTRL";

static bool s_initialized = false;
static float s_last_temperature_c = 0.0f;
static TickType_t s_heater_on_tick = 0;
static bool s_heater_on_tracked = false;
/* 超长加热触发冷却锁定的截止时刻：到期前一律不许再开加热 */
static TickType_t s_heater_lockout_tick = 0;

esp_err_t temp_control_init(void) {
    esp_err_t ret = heater_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Heater init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 温度传感器由 sensor_hub 统一初始化，这里只确保总线就绪 */
    ret = ds18b20_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "DS18B20 init failed: %s (temp control stays active)", esp_err_to_name(ret));
    }

    s_initialized = true;
    s_last_temperature_c = P(PARAM_IDX_TEMP_TARGET);
    s_heater_on_tracked = false;
    ESP_LOGI(TAG, "Temperature control initialized");
    return ESP_OK;
}

/* 关断加热，附带日志（只在状态真的变化时打） */
static void force_heater_off(const char *reason) {
    if (heater_get_state()) {
        (void)heater_set_state(false);
        ESP_LOGE(TAG, "heater forced OFF: %s", reason);
    }
    s_heater_on_tracked = false;
}

esp_err_t temp_control_update(void) {
    if (!s_initialized) {
        return APP_ERR_INVALID_STATE;
    }

    /* 关闭自动控制就不干预执行器。
     * 注意：这里刻意不看"手动模式" —— 恒温是保命功能，
     * 不能因为用户点了下灯就罢工 120 秒。 */
    if (!control_mode_auto_enabled() ||
        !param_get_bool(PARAM_IDX_AUTO_HEATER)) {
        return ESP_OK;
    }

    const sensor_sample_t *sample = sensor_hub_get(SH_TEMP);

    /* 安全联锁：温度数据过期（探头掉线/CRC 错）时必须停止加热。
     * BUGFIX：这段原来放在"手动保持"判断之后，导致用户点一下加热后
     * 的 120 秒内探头失效也不关断。安全联锁永远先跑。 */
    if (!sensor_hub_is_fresh(SH_TEMP, SENSOR_STALE_TIMEOUT_MS) || sample == NULL) {
        force_heater_off("temperature sample stale");
        return APP_ERR_SENSOR_STALE;
    }

    const float temperature_c = sample->value;
    s_last_temperature_c = temperature_c;

    /* 已经超温：无条件关断，手动保持也不能屏蔽。
     * 两重判据：
     *   1) 目标值 + 回差（用户自己设的舒适区上限）
     *   2) 硬件安全上限 TEMP_ALERT_MAX —— 手机把目标滑到 35℃、回差 5，
     *      光靠第 1 条要烧到 40℃ 才停，所以必须有这条只读硬上限。 */
    const float target_now = P(PARAM_IDX_TEMP_TARGET);
    const float hys_now = P(PARAM_IDX_TEMP_HYS);
    if (temperature_c >= (target_now + hys_now) || temperature_c >= TEMP_ALERT_MAX) {
        force_heater_off("over temperature (manual run)");
    }

    /* 用户刚手动点过加热：这 120 秒内不做回差调节（但上面两道联锁照旧） */
    if (act_hold_active("heater")) {
        return ESP_OK;
    }

    const float target = P(PARAM_IDX_TEMP_TARGET);
    const float hys = P(PARAM_IDX_TEMP_HYS);
    const bool heater_on = heater_get_state();
    bool should_heat = heater_on;
    /* 上次因为超长加热被强制关断：冷却期内一律不许再开 */
    if ((int32_t)(xTaskGetTickCount() - s_heater_lockout_tick) < 0) {
        if (heater_on) {
            force_heater_off("heater lockout after max on-time");
        }
        return ESP_OK;
    }

    if (temperature_c <= (target - hys)) {
        should_heat = true;
    } else if (temperature_c >= (target + hys)) {
        should_heat = false;
    }

    /* 加热最长连续时间保护 */
    if (should_heat) {
        const TickType_t now = xTaskGetTickCount();
        if (!heater_on || !s_heater_on_tracked) {
            s_heater_on_tick = now;
            s_heater_on_tracked = true;
        } else {
            const uint32_t on_s = (uint32_t)((now - s_heater_on_tick) / configTICK_RATE_HZ);
            const uint32_t max_s = param_get_u32(PARAM_IDX_HEATER_MAX_ON_S);
            if (on_s > max_s) {
                force_heater_off("heater max on-time exceeded");
                /* 触发后必须冷却一段时间，否则下一轮循环（2 秒后）会重新起表、
                 * 再开一次，变成"开 1 小时 / 关 2 秒"的无限循环，保护形同虚设。 */
                s_heater_lockout_tick = xTaskGetTickCount() +
                                        pdMS_TO_TICKS(max_s * 1000u);
                s_heater_on_tracked = false;
                return ESP_OK;
            }
        }
    } else {
        s_heater_on_tracked = false;
    }

    if (should_heat != heater_on) {
        esp_err_t ret = heater_set_state(should_heat);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Temperature %.2f C (target %.1f), heater %s",
                     temperature_c, (double)target, should_heat ? "ON" : "OFF");
        }
        return ret;
    }

    return ESP_OK;
}

float temp_control_get_last_temperature_c(void) {
    const sensor_sample_t *s = sensor_hub_get(SH_TEMP);
    if (s && s->valid) {
        return s->value;
    }
    return s_last_temperature_c;
}

bool temp_control_is_heating(void) {
    return heater_get_state();
}
