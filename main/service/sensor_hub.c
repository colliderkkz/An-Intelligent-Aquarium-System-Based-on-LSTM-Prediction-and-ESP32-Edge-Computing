#include "service/sensor_hub.h"

#include "config/error_codes.h"
#include "config/pin_config.h"
#include "config/system_config.h"
#include "drivers/adc/adc_manager.h"
#include "drivers/adc/light_sensor.h"
#include "drivers/adc/ph_sensor_adc.h"
#include "drivers/adc/tds_sensor.h"
#include "drivers/adc/turbidity.h"
#include "drivers/adc/water_level.h"
#include "drivers/ds18b20.h"
#include "drivers/pcf8563.h"
#include "drivers/sgp30.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sensors/co2_sensor.h"
#include "service/time_service.h"
#include <string.h>

static const char *TAG = "SENSOR_HUB";

typedef struct {
    const char *id_name;
    const char *label;
    const char *unit;
    uint8_t decimals;
    uint32_t period_ms;
} sensor_meta_t;

static const sensor_meta_t s_meta[SH_COUNT] = {
    [SH_TEMP]      = {"temp",   "水温",  "C",   2, SENSOR_TEMP_PERIOD_MS},
    [SH_PH]        = {"ph",     "pH",    "pH",  2, 2000},
    [SH_TDS]       = {"tds",    "TDS",   "ppm", 0, 2000},
    [SH_TURBIDITY] = {"turb",   "浊度",  "NTU", 1, 2000},
    [SH_LEVEL]     = {"level",  "水位",  "%",   1, 1000},
    [SH_LIGHT]     = {"light",  "光照",  "lux", 0, 2000},
    [SH_CO2]       = {"co2",    "CO2",   "ppm", 0, SENSOR_CO2_PERIOD_MS},
};

static sensor_sample_t s_samples[SH_COUNT];
static portMUX_TYPE s_samples_mux = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_lock = NULL;
static i2c_bus_handle_t s_bus = NULL;
static bool s_initialized = false;
static bool s_started = false;
static bool s_co2_ready = false;

/* SGP30 连续失败时按倍数退避采样周期（最大 10 倍），
 * 避免模块没插时每秒都做一次注定超时的 I2C 事务。 */
static uint32_t s_co2_backoff = 1;

/* 温度连续失败计数（用于日志限流） */
static uint32_t s_temp_err_streak = 0;

static uint32_t now_ms(void) {
    return (uint32_t)((uint64_t)xTaskGetTickCount() * 1000u / configTICK_RATE_HZ);
}

/* 采样一次并把结果写回缓存；并发保护在调用方外面统一加 */
static esp_err_t sample_sensor(sensor_id_t id, sensor_sample_t *out) {
    sensor_sample_t result = s_samples[id];
    float temp_c = sensor_hub_temperature_or(25.0f);

    result.attempted_ms = now_ms();
    result.value2 = 0.0f;

    switch (id) {
        case SH_TEMP: {
            float value = 0.0f;
            result.raw = 0;
            result.mv = 0;
            result.err = ds18b20_read_temperature_c(&value);
            if (result.err == ESP_OK) {
                result.value = value;
            }
            break;
        }
        case SH_PH:
            result.err = ph_sensor_read_raw(temp_c, &result.value, &result.raw, &result.mv);
            break;
        case SH_TDS:
            result.err = tds_sensor_read_raw(temp_c, &result.value, &result.raw, &result.mv);
            break;
        case SH_TURBIDITY:
            result.err = turbidity_read_raw(temp_c, &result.value, &result.raw, &result.mv);
            break;
        case SH_LEVEL:
            result.err = water_level_read_raw(&result.value, &result.raw, &result.mv);
            break;
        case SH_LIGHT:
            result.err = light_sensor_read_raw(&result.value, &result.raw, &result.mv);
            break;
        case SH_CO2: {
            if (!s_co2_ready) {
                result.err = ESP_ERR_NOT_FOUND;
                break;
            }
            sensor_co2_data_t data = {0};
            result.err = sensor_co2_read(&data);
            if (result.err == ESP_OK) {
                result.value = (float)data.co2_eq_ppm;
                result.value2 = (float)data.tvoc_ppb;
            }
            break;
        }
        default:
            result.err = ESP_ERR_INVALID_ARG;
            break;
    }

    if (result.err == ESP_OK) {
        result.valid = true;
        result.updated_ms = result.attempted_ms;
        result.ok_count++;
    } else {
        /* 失败必须把 valid 清掉：否则只判 valid 的消费者（状态上报、
         * 温度联锁）会一直拿着最后一次成功的旧值，探头拔了也看不出来。 */
        result.valid = false;
        result.err_count++;
    }

    /* 整结构体赋值不是原子操作：别的任务可能读到"新 value + 旧 valid/err"的组合，
     * 从而把一次失败的采样当成有效值发出去。用小临界区把这次拷贝保护起来
     * （读侧仍是无锁指针，但整块写入期间不会被切走，撕裂窗口从"整个赋值"
     * 缩到几乎为零）。 */
    taskENTER_CRITICAL(&s_samples_mux);
    s_samples[id] = result;
    taskEXIT_CRITICAL(&s_samples_mux);
    if (out) {
        *out = result;
    }
    return result.err;
}

static void sensor_hub_task(void *pvParameters) {
    (void)pvParameters;

    uint32_t last_ms[SH_COUNT] = {0};
    bool first_round = true;

    for (int i = 0; i < SH_COUNT; i++) {
        last_ms[i] = now_ms();
    }

    while (1) {
        const uint32_t ms = now_ms();

        for (int i = 0; i < SH_COUNT; i++) {
            const sensor_id_t id = (sensor_id_t)i;
            uint32_t period = s_meta[i].period_ms;
            if (id == SH_CO2) {
                period *= s_co2_backoff;
            }
            if (!first_round && (uint32_t)(ms - last_ms[i]) < period) {
                continue;
            }
            if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(2000)) != pdTRUE) {
                continue;
            }
            esp_err_t err = sample_sensor(id, NULL);
            xSemaphoreGive(s_lock);
            last_ms[i] = now_ms();

            if (id == SH_CO2) {
                if (err == ESP_OK) {
                    s_co2_backoff = 1;
                } else if (s_co2_backoff < 10) {
                    s_co2_backoff++;
                    ESP_LOGD(TAG, "SGP30 failing, backoff x%lu", (unsigned long)s_co2_backoff);
                }
            }

            if (id == SH_TEMP && err != ESP_OK) {
                /* 探头没接时每 2s 都会失败，限流成每 15 次报一条，避免刷屏 */
                s_temp_err_streak++;
                if ((s_temp_err_streak % 15u) == 1u) {
                    ESP_LOGW(TAG, "temperature sensor error: %s (streak=%lu)",
                             esp_err_to_name(err), (unsigned long)s_temp_err_streak);
                }
            } else if (id == SH_TEMP) {
                s_temp_err_streak = 0;
            }
        }
        first_round = false;

        /* 顺便驱动 RTC 对时（每 60s 一次，本身有节流） */
        time_service_poll();

        vTaskDelay(pdMS_TO_TICKS(SENSOR_HUB_PERIOD_MS));
    }
}

esp_err_t sensor_hub_init(i2c_bus_handle_t bus) {
    if (s_initialized) {
        return ESP_OK;
    }

    memset(s_samples, 0, sizeof(s_samples));
    s_bus = bus;

    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    /* ADC 单元 + 各通道 */
    esp_err_t ret = adc_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC manager init failed: %s", esp_err_to_name(ret));
    }

    (void)ds18b20_init();
    (void)ph_sensor_init();
    (void)tds_sensor_init();
    (void)turbidity_init();
    (void)water_level_init();
    (void)light_sensor_init();

    if (bus) {
        s_co2_ready = (sensor_co2_init(bus) == ESP_OK);
        if (!s_co2_ready) {
            ESP_LOGW(TAG, "SGP30 not available");
        }
    }

    s_initialized = true;
    ESP_LOGI(TAG, "sensor hub initialized (co2=%s)", s_co2_ready ? "ok" : "absent");
    return ESP_OK;
}

esp_err_t sensor_hub_start(void) {
    if (!s_initialized) {
        return APP_ERR_INVALID_STATE;
    }
    if (s_started) {
        return ESP_OK;
    }

    BaseType_t ok = xTaskCreate(sensor_hub_task, "sensor_hub", TASK_STACK_SENSOR, NULL,
                                TASK_PRIORITY_SENSOR, NULL);
    if (ok != pdPASS) {
        return ESP_FAIL;
    }
    s_started = true;
    return ESP_OK;
}

const sensor_sample_t *sensor_hub_get(sensor_id_t id) {
    if ((int)id < 0 || (int)id >= SH_COUNT) {
        return NULL;
    }
    return &s_samples[id];
}

esp_err_t sensor_hub_sample(sensor_id_t id, sensor_sample_t *out) {
    if ((int)id < 0 || (int)id >= SH_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return APP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(3000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t ret = sample_sensor(id, out);
    xSemaphoreGive(s_lock);
    return ret;
}

bool sensor_hub_is_fresh(sensor_id_t id, uint32_t max_age_ms) {
    const sensor_sample_t *s = sensor_hub_get(id);
    if (!s || !s->valid) {
        return false;
    }
    return (uint32_t)(now_ms() - s->updated_ms) <= max_age_ms;
}

float sensor_hub_temperature_or(float fallback_c) {
    const sensor_sample_t *s = sensor_hub_get(SH_TEMP);
    if (s && s->valid && sensor_hub_is_fresh(SH_TEMP, 60000)) {
        return s->value;
    }
    return fallback_c;
}

const char *sensor_hub_id_name(sensor_id_t id) {
    if ((int)id < 0 || (int)id >= SH_COUNT) {
        return "?";
    }
    return s_meta[id].id_name;
}

const char *sensor_hub_label(sensor_id_t id) {
    if ((int)id < 0 || (int)id >= SH_COUNT) {
        return "?";
    }
    return s_meta[id].label;
}

const char *sensor_hub_unit(sensor_id_t id) {
    if ((int)id < 0 || (int)id >= SH_COUNT) {
        return "";
    }
    return s_meta[id].unit;
}

int sensor_hub_decimals(sensor_id_t id) {
    if ((int)id < 0 || (int)id >= SH_COUNT) {
        return 2;
    }
    return s_meta[id].decimals;
}

uint32_t sensor_hub_period_ms(sensor_id_t id) {
    if ((int)id < 0 || (int)id >= SH_COUNT) {
        return 1000;
    }
    return s_meta[id].period_ms;
}

int sensor_hub_find(const char *name) {
    if (!name) {
        return -1;
    }
    for (int i = 0; i < SH_COUNT; i++) {
        if (strcmp(s_meta[i].id_name, name) == 0) {
            return i;
        }
    }
    return -1;
}

const char *sensor_hub_status_str(sensor_id_t id, uint32_t max_age_ms) {
    const sensor_sample_t *s = sensor_hub_get(id);
    if (!s || !s->valid) {
        return (s && s->err_count > 0) ? "ERROR" : "INIT";
    }
    if ((uint32_t)(now_ms() - s->updated_ms) > max_age_ms) {
        return "STALE";
    }
    return (s->err == ESP_OK) ? "OK" : "ERROR";
}
