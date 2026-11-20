/**
 * @file param_store.h
 * @brief 运行时可读写参数表（阈值 / 标定系数 / 极性 / 定时），支持 NVS 掉电保存
 *
 * 设计要点
 *  - 全部参数统一用 float 存储，避免联合体/类型转换带来的麻烦；
 *    时间是秒/毫秒量级（<= 86400），float 的 24bit 尾数完全够用。
 *  - 参数表用 X-Macro 定义一次，枚举、描述表、名字查找全部自动生成，
 *    因此“固件支持哪些参数”与“上位机显示哪些参数”永远一致：
 *    上位机启动时发 PARAM,LIST 动态拉取即可。
 *  - 写参数只发生在调试控制台任务里；控制任务只读，Xtensa 上对齐的
 *    float 读写在单字内是原子的，故不加锁。
 */
#ifndef PARAM_STORE_H
#define PARAM_STORE_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 参数定义表
 *   X(ID, 名字, 分组, 默认值, 最小值, 最大值, 单位, 小数位, 说明)
 * 名字必须是稳定的 ASCII 标识（上位机脚本依赖它，不要随意改名）。
 */
#define PARAM_LIST(X)                                                                     \
    X(AUTO_ENABLE,      "auto_enable",      "sys",   1.0f,     0.0f,     1.0f,    "",    0, "全局自动控制开关") \
    X(MANUAL_TIMEOUT_S, "manual_timeout_s", "sys",   120.0f,   10.0f,    3600.0f, "s",   0, "手动保持时长(之后回归计划)") \
    X(STREAM_PERIOD_MS, "stream_period_ms", "sys",   1000.0f,  100.0f,   60000.0f,"ms",  0, "遥测默认周期")     \
    X(TEMP_TARGET,      "temp_target_c",    "temp",  26.0f,    15.0f,    35.0f,   "C",   1, "目标水温")          \
    X(TEMP_HYS,         "temp_hys_c",       "temp",  1.0f,     0.2f,     5.0f,    "C",   1, "加热回差")          \
    X(TEMP_ALERT_MIN,   "temp_alert_min_c", "temp",  18.0f,    0.0f,     40.0f,   "C",   1, "低温告警")          \
    X(TEMP_ALERT_MAX,   "temp_alert_max_c", "temp",  34.0f,    0.0f,     45.0f,   "C",   1, "高温告警")          \
    X(HEATER_MAX_ON_S,  "heater_max_on_s",  "temp",  3600.0f,  60.0f,    7200.0f, "s",   0, "加热最长连续时间")  \
    X(AUTO_HEATER,      "auto_heater",      "temp",  1.0f,     0.0f,     1.0f,    "",    0, "温控自动使能")      \
    X(PH_MIN,           "ph_min",           "ph",    6.5f,     0.0f,     14.0f,   "pH",  2, "pH 正常下限")       \
    X(PH_MAX,           "ph_max",           "ph",    7.5f,     0.0f,     14.0f,   "pH",  2, "pH 正常上限")       \
    X(PH_SLOPE,         "ph_slope",         "ph",   -5.887f,  -20.0f,    20.0f,   "pH/V",3, "pH 电压斜率")       \
    X(PH_OFFSET,        "ph_offset",        "ph",   21.677f,  -30.0f,    40.0f,   "pH",  3, "pH 截距")           \
    X(PH_TEMP_COMP,     "ph_temp_comp",     "ph",    0.0f,     -1.0f,     1.0f,   "pH/C",3, "pH 温度补偿")       \
    X(TDS_K,            "tds_k",            "tds",   1.0f,      0.2f,     3.0f,   "x",   3, "TDS 校准系数")      \
    X(TDS_ALERT,        "tds_alert",        "tds",   500.0f,    0.0f,  5000.0f,   "ppm", 0, "TDS 告警")          \
    X(TURB_SLOPE,       "turb_slope",       "turb", -865.68f, -5000.0f,   0.0f,   "NTU/V",2,"浊度斜率")          \
    X(TURB_OFFSET,      "turb_offset",      "turb", 3347.19f,  0.0f,  8000.0f,    "NTU", 2, "浊度截距")          \
    X(TURB_ALERT,       "turb_alert",       "turb",  20.0f,     0.0f,  3000.0f,   "NTU", 0, "浊度告警")          \
    X(LEVEL_MIN_PCT,    "level_min_pct",    "level", 30.0f,    0.0f,    100.0f,   "%",   0, "低于该水位启动补水") \
    X(LEVEL_STOP_PCT,   "level_stop_pct",   "level", 95.0f,    0.0f,    100.0f,   "%",   0, "高于该水位停止补水") \
    X(LEVEL_ALERT_PCT,  "level_alert_pct",  "level", 20.0f,    0.0f,    100.0f,   "%",   0, "低水位告警")        \
    X(LEVEL_MIN_MV,     "level_min_mv",     "level", 0.0f,     0.0f,   3300.0f,   "mV",  0, "水位0%对应电压")    \
    X(LEVEL_MAX_MV,     "level_max_mv",     "level", 3000.0f, 100.0f,   3300.0f,   "mV",  0, "水位100%对应电压")  \
    X(REFILL_MAX_S,     "refill_max_s",     "level", 600.0f,   10.0f,   1800.0f,   "s",   0, "单次补水最长时间")  \
    X(REFILL_COOLDOWN_S,"refill_cooldown_s","level", 3600.0f,  0.0f,   86400.0f,   "s",   0, "两次补水最小间隔")  \
    X(AUTO_REFILL,      "auto_refill",      "level", 1.0f,     0.0f,     1.0f,    "",    0, "补水自动使能")      \
    X(LIGHT_ON_H,       "light_on_h",       "light", 8.0f,     0.0f,     23.0f,   "h",   0, "开灯 时")          \
    X(LIGHT_ON_M,       "light_on_m",       "light", 0.0f,     0.0f,     59.0f,   "m",   0, "开灯 分")          \
    X(LIGHT_OFF_H,      "light_off_h",      "light", 18.0f,    0.0f,     23.0f,   "h",   0, "关灯 时")          \
    X(LIGHT_OFF_M,      "light_off_m",      "light", 0.0f,     0.0f,     59.0f,   "m",   0, "关灯 分")          \
    X(LIGHT_BRIGHTNESS, "light_brightness", "light", 100.0f,   0.0f,    100.0f,   "%",   0, "开灯亮度")          \
    X(AUTO_LIGHT,       "auto_light",       "light", 1.0f,     0.0f,     1.0f,    "",    0, "灯光定时使能")      \
    X(CO2_WITH_LIGHT,   "co2_with_light",   "light", 1.0f,     0.0f,     1.0f,    "",    0, "CO2随灯开关")       \
    X(AUTO_CO2,         "auto_co2",         "light", 1.0f,     0.0f,     1.0f,    "",    0, "CO2自动使能")       \
    X(CO2_ALERT,        "co2_alert",        "light", 1200.0f,  0.0f,  10000.0f,   "ppm", 0, "CO2告警")          \
    X(FEED1_H,          "feed1_h",          "feed",  8.0f,     0.0f,     23.0f,   "h",   0, "喂食1 时")         \
    X(FEED1_M,          "feed1_m",          "feed",  0.0f,     0.0f,     59.0f,   "m",   0, "喂食1 分")         \
    X(FEED2_H,          "feed2_h",          "feed",  17.0f,    0.0f,     23.0f,   "h",   0, "喂食2 时")         \
    X(FEED2_M,          "feed2_m",          "feed",  30.0f,    0.0f,     59.0f,   "m",   0, "喂食2 分")         \
    X(FEED_ANGLE,       "feed_angle",       "feed",  120.0f,   0.0f,    180.0f,   "deg", 0, "喂食角度")          \
    X(FEED_HOLD_MS,     "feed_hold_ms",     "feed",  1500.0f,  100.0f, 20000.0f,   "ms",  0, "喂食保持时间")      \
    X(AUTO_FEEDER,      "auto_feeder",      "feed",  1.0f,     0.0f,     1.0f,    "",    0, "定时喂食使能")      \
    X(UV_ON_H,          "uv_on_h",          "uv",    1.0f,     0.0f,     23.0f,   "h",   0, "紫外开 时")         \
    X(UV_ON_M,          "uv_on_m",          "uv",    30.0f,    0.0f,     59.0f,   "m",   0, "紫外开 分")         \
    X(UV_OFF_H,         "uv_off_h",         "uv",    3.0f,     0.0f,     23.0f,   "h",   0, "紫外关 时")         \
    X(UV_OFF_M,         "uv_off_m",         "uv",    0.0f,     0.0f,     59.0f,   "m",   0, "紫外关 分")         \
    X(AUTO_UV,          "auto_uv",          "uv",    1.0f,     0.0f,     1.0f,    "",    0, "紫外定时使能")      \
    X(PUMP_ALWAYS_ON,   "pump_always_on",   "pump",  1.0f,     0.0f,     1.0f,    "",    0, "循环泵常开")        \
    X(INV_HEATER,       "inv_heater",       "out",   0.0f,     0.0f,     1.0f,    "",    0, "加热输出反相")      \
    X(INV_PUMP,         "inv_pump",         "out",   0.0f,     0.0f,     1.0f,    "",    0, "循环泵输出反相")    \
    X(INV_REFILL,       "inv_refill",       "out",   0.0f,     0.0f,     1.0f,    "",    0, "补水泵输出反相")    \
    X(INV_CO2,          "inv_co2",          "out",   0.0f,     0.0f,     1.0f,    "",    0, "CO2输出反相")       \
    X(INV_UV,           "inv_uv",           "out",   0.0f,     0.0f,     1.0f,    "",    0, "紫外输出反相")      \
    X(INV_LIGHT,        "inv_light",        "out",   0.0f,     0.0f,     1.0f,    "",    0, "灯光输出反相")      \
    X(INV_AIR,          "inv_air",          "out",   0.0f,     0.0f,     1.0f,    "",    0, "气泵输出反相")      \
    X(LIGHT_LUX_SCALE,  "light_lux_scale",  "sensor", 1.0f,   0.1f,     10.0f,   "",    2, "光照 lux 校准倍率") \
    X(SERVO_REVERSE,    "servo_reverse",    "feed",   0.0f,   0.0f,     1.0f,    "",    0, "喂食舵机方向反向")

/* 参数索引枚举 */
typedef enum {
#define X_ENUM(id, name, group, def, minv, maxv, unit, dec, desc) PARAM_IDX_##id,
    PARAM_LIST(X_ENUM)
#undef X_ENUM
    PARAM_IDX_COUNT
} param_id_t;

/* 参数描述 */
typedef struct {
    const char *name;
    const char *group;
    float default_value;
    float min_value;
    float max_value;
    const char *unit;
    uint8_t decimals;
    const char *desc;
} param_desc_t;

/** 读取参数总量 */
int param_store_count(void);

/** 按下标取描述 */
const param_desc_t *param_store_desc(int index);

/** 按名字找下标，找不到返回 -1 */
int param_store_find(const char *name);

/** 初始化：载入默认值并从 NVS 恢复（NVS 无数据时保持默认） */
esp_err_t param_store_init(void);

/** 按索引/名字读取（越界或未知名字返回 fallback） */
float param_get(param_id_t id);
float param_get_f(const char *name, float fallback);
/** 便捷宏：P(PARAM_IDX_TEMP_TARGET) */
#define P(id) param_get((param_id_t)(id))

/** 按索引/名字写（自动 clamp 到 [min,max]） */
esp_err_t param_set(param_id_t id, float value);
esp_err_t param_set_f(const char *name, float value);

/** 是否有未保存修改 */
bool param_store_dirty(void);

/** 保存到 NVS / 从 NVS 重新载入 / 恢复默认（不自动保存） */
esp_err_t param_store_save(void);
esp_err_t param_store_load(void);
esp_err_t param_store_reset_defaults(void);

/** 整数便捷读取 */
uint32_t param_get_u32(param_id_t id);
int param_get_int(param_id_t id);
bool param_get_bool(param_id_t id);

#ifdef __cplusplus
}
#endif

#endif /* PARAM_STORE_H */
