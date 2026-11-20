/**
 * @file sensor_hub.h
 * @brief 传感器采集中枢：单一任务周期采样 + 缓存，供控制任务与上位机读取
 *
 * 为什么需要它：
 *  旧代码里 pH/TDS/浊度 读取函数内部又去调 sensor_temp_read_c()，而 DS18B20
 *  一次转换要 750ms，于是“读一次 pH”要 800ms 以上；如果探头掉线，三个模拟量
 *  传感器全部读不出来。这里改成后台任务统一采样并缓存：
 *    - 温度 2s 一次，其余按需，控制任务/上位机读缓存（毫秒级返回）；
 *    - 每个通道记录 raw/mv/工程值/时间戳/错误计数，上位机可完整监控；
 *    - 温度失效时用最近一次有效值或 25℃ 兜底，不会拖垮其它传感器。
 */
#ifndef SENSOR_HUB_H
#define SENSOR_HUB_H

#include "bus/i2c_bus.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SH_TEMP = 0,      /**< 水温 (DS18B20) */
    SH_PH,            /**< pH */
    SH_TDS,           /**< TDS ppm */
    SH_TURBIDITY,     /**< 浊度 NTU */
    SH_LEVEL,         /**< 水位 % */
    SH_LIGHT,         /**< 光照 % */
    SH_CO2,           /**< CO2 当量 ppm (SGP30)，副值 TVOC 在 value2 */
    SH_COUNT
} sensor_id_t;

typedef struct {
    float value;          /**< 工程值 */
    float value2;         /**< 副值（如 TVOC ppb），无则为 0 */
    int raw;              /**< ADC 12bit 原始值，非 ADC 通道为 0 */
    int mv;               /**< ADC 毫伏值，非 ADC 通道为 0 */
    esp_err_t err;        /**< 最近一次采样错误码 */
    uint32_t updated_ms;  /**< 最近一次成功采样的系统运行时间(ms) */
    uint32_t attempted_ms;/**< 最近一次尝试采样时间(ms) */
    uint32_t ok_count;
    uint32_t err_count;
    bool valid;           /**< 是否曾经成功采样过 */
} sensor_sample_t;

/** 初始化所有传感器驱动（内部会 init i2c 上的 SGP30） */
esp_err_t sensor_hub_init(i2c_bus_handle_t bus);

/** 创建采样任务 */
esp_err_t sensor_hub_start(void);

/** 读缓存（不会阻塞，不会触发采样） */
const sensor_sample_t *sensor_hub_get(sensor_id_t id);

/** 立即采样一次并刷新缓存（可能阻塞：温度约 800ms，其余 <20ms） */
esp_err_t sensor_hub_sample(sensor_id_t id, sensor_sample_t *out);

/** 数据是否新鲜（成功采样且未超过 max_age_ms） */
bool sensor_hub_is_fresh(sensor_id_t id, uint32_t max_age_ms);

/** 最近一次有效水温；从未成功则返回 fallback_c */
float sensor_hub_temperature_or(float fallback_c);

/** 元信息 */
const char *sensor_hub_id_name(sensor_id_t id);   /**< "temp" / "ph" / ... */
const char *sensor_hub_label(sensor_id_t id);     /**< "水温" / "pH" / ... */
const char *sensor_hub_unit(sensor_id_t id);      /**< "C" / "pH" / "ppm" ... */
int sensor_hub_decimals(sensor_id_t id);
uint32_t sensor_hub_period_ms(sensor_id_t id);

/** 按名字查 id，返回 -1 表示未知 */
int sensor_hub_find(const char *name);

/** "OK" / "STALE" / "ERROR" / "INIT" */
const char *sensor_hub_status_str(sensor_id_t id, uint32_t max_age_ms);

#ifdef __cplusplus
}
#endif

#endif // SENSOR_HUB_H
