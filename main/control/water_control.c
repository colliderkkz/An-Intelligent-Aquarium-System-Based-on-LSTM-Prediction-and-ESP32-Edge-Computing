#include "control/water_control.h"

#include "actuators/water_pump.h"
#include "config/error_codes.h"
#include "config/param_store.h"
#include "config/system_config.h"
#include "drivers/adc/water_level.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "service/control_mode.h"
#include "service/act_hold.h"
#include "service/sensor_hub.h"
#include <stdint.h>

static const char *TAG = "WATER_CTRL";

/* 每小时内最多补水的次数，防止水位传感器故障导致反复抽水淹缸 */
#define REFILL_MAX_PER_HOUR 3

/* 低水位必须持续这么久才真正启动补水。
 * 上电瞬间 / 传感器刚插上时读数会跳变，直接动作很危险。 */
#define REFILL_CONFIRM_S 30

static bool s_initialized = false;
static bool s_is_refilling = false;
static float s_last_level_percent = 0.0f;
static TickType_t s_refill_start_tick = 0;
static TickType_t s_last_refill_end_tick = 0;
static bool s_has_refilled = false;

/* 低水位持续计时 */
static bool s_low_active = false;
static TickType_t s_low_since_tick = 0;

static uint32_t refill_count = 0;
static TickType_t s_refill_window_start = 0;

static uint32_t elapsed_seconds_since(TickType_t start_tick) {
    TickType_t now = xTaskGetTickCount();
    return (uint32_t)((now - start_tick) / configTICK_RATE_HZ);
}

esp_err_t water_control_init(void) {
    esp_err_t ret = water_pump_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Water refill pump init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 水位 ADC 通道由 sensor_hub 统一配置 */
    ret = water_level_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Water level sensor init failed: %s", esp_err_to_name(ret));
    }

    s_initialized = true;
    s_is_refilling = false;
    s_last_level_percent = 0.0f;
    s_refill_start_tick = 0;
    s_last_refill_end_tick = 0;
    s_has_refilled = false;
    s_low_active = false;
    s_low_since_tick = 0;
    refill_count = 0;
    s_refill_window_start = xTaskGetTickCount();

    ESP_LOGI(TAG, "Water control initialized");
    return ESP_OK;
}

static void stop_refill(const char *reason) {
    /* BUGFIX（溢水风险）：原来这里 `if (!s_is_refilling) return;`，
     * 导致"用户手动开的泵"永远不会被自动关掉 —— 水位到 100% 也没人管，
     * REFILL_MAX_S 同样不生效。现在不管谁开的泵，都关。
     *
     * 但"冷却计时"只在确实停过补水时才记：原来无条件重置
     * s_last_refill_end_tick + s_has_refilled=true，水位探头失效 >30 秒时
     * 每 2 秒调一次这里，等探头恢复后 cooldown 还在，自动补水被白白锁一小时。 */
    const bool was_ours = s_is_refilling;
    const bool pump_was_on = water_pump_get_state();
    s_is_refilling = false;
    if (!was_ours && !pump_was_on) {
        return;                     /* 本来就没在补水，别动冷却状态 */
    }
    s_last_refill_end_tick = xTaskGetTickCount();
    s_has_refilled = true;
    (void)water_pump_set_state(false);
    ESP_LOGW(TAG, "Refill stopped: %s", reason);
}

esp_err_t water_control_update(void) {
    if (!s_initialized) {
        return APP_ERR_INVALID_STATE;
    }

    /* 补水是最危险的动作：安全联锁永远先跑，连用户手动保持也不能屏蔽它 */
    if (!sensor_hub_is_fresh(SH_LEVEL, SENSOR_STALE_TIMEOUT_MS)) {
        stop_refill("water level sample stale");
        return APP_ERR_SENSOR_STALE;
    }

    const float level_percent = sensor_hub_get(SH_LEVEL)->value;
    s_last_level_percent = level_percent;

    /* 不管泵是谁开的，到达停止水位就必须关 —— 手动开启同样受此约束 */
    if (water_pump_get_state() && level_percent >= P(PARAM_IDX_LEVEL_STOP_PCT)) {
        stop_refill("stop level reached (manual run)");
    }

    /* 用户直接操作补水泵时尊重这个操作（走上面两重保护之后） */
    if (act_hold_active("refill")) {
        return ESP_OK;
    }

    const float stop_pct = P(PARAM_IDX_LEVEL_STOP_PCT);
    float start_pct = P(PARAM_IDX_LEVEL_MIN_PCT);

    /* 参数配置错误（启动水位 >= 停止水位）时禁用自动补水，避免无限补水 */
    if (start_pct >= stop_pct) {
        stop_refill("invalid level thresholds (min>=stop)");
        return APP_ERR_BAD_ARG;
    }

    if (s_is_refilling) {
        const bool reached_target = level_percent >= stop_pct;
        const uint32_t max_s = param_get_u32(PARAM_IDX_REFILL_MAX_S);
        const bool timeout = elapsed_seconds_since(s_refill_start_tick) >= max_s;
        if (reached_target) {
            stop_refill("target level reached");
        } else if (timeout) {
            stop_refill("max duration reached");
        }
        return ESP_OK;
    }

    if (!control_mode_auto_enabled() || !param_get_bool(PARAM_IDX_AUTO_REFILL)) {
        return ESP_OK;
    }

    /* 每小时补水次数限制 */
    if (elapsed_seconds_since(s_refill_window_start) >= 3600) {
        s_refill_window_start = xTaskGetTickCount();
        refill_count = 0;
    }

    const uint32_t cooldown_s = param_get_u32(PARAM_IDX_REFILL_COOLDOWN_S);
    const bool cooldown_done = !s_has_refilled ||
                               (elapsed_seconds_since(s_last_refill_end_tick) >= cooldown_s);

    const bool low_now = (level_percent <= start_pct);

    /* 低水位必须“持续” REFILL_CONFIRM_S 秒才动作：
     * 上电自检、传感器刚插上时读数会乱跳，直接启泵容易误抽水。 */
    if (!low_now) {
        s_low_active = false;
    } else if (!s_low_active) {
        s_low_active = true;
        s_low_since_tick = xTaskGetTickCount();
        ESP_LOGW(TAG, "level low (%.1f%%), waiting %ds confirmation before refill",
                 (double)level_percent, REFILL_CONFIRM_S);
    }

    const bool low_confirmed = s_low_active &&
                               (elapsed_seconds_since(s_low_since_tick) >= REFILL_CONFIRM_S);

    if (low_confirmed && cooldown_done) {
        if (refill_count >= REFILL_MAX_PER_HOUR) {
            ESP_LOGE(TAG, "refill blocked: %lu times in this hour (check water level sensor!)",
                     (unsigned long)refill_count);
            return APP_ERR_ACTUATOR_COOLDOWN;
        }
        esp_err_t ret = water_pump_set_state(true);
        if (ret == ESP_OK) {
            s_is_refilling = true;
            s_refill_start_tick = xTaskGetTickCount();
            refill_count++;
            ESP_LOGW(TAG, "Low water level %.1f%% (start %.0f%%, stop %.0f%%), refill ON",
                     (double)level_percent, (double)start_pct, (double)stop_pct);
        }
        return ret;
    }

    return ESP_OK;
}

bool water_control_is_refilling(void) {
    return s_is_refilling;
}

float water_control_get_last_level_percent(void) {
    const sensor_sample_t *s = sensor_hub_get(SH_LEVEL);
    if (s && s->valid) {
        return s->value;
    }
    return s_last_level_percent;
}
