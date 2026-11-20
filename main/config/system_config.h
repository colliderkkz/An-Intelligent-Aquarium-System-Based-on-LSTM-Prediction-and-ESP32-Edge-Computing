//系统参数：阈值、周期、任务栈、控制参数、告警配置
#ifndef SYSTEM_CONFIG_H
#define SYSTEM_CONFIG_H

#include <stdint.h>
#include <stdbool.h>

// ==================== 系统版本 ====================
#define SYSTEM_VERSION_MAJOR          1
#define SYSTEM_VERSION_MINOR          1
#define SYSTEM_VERSION_PATCH          1
#define SYSTEM_NAME                    "fish_gundam_panzer"

// ==================== 任务配置 ====================
#define TASK_STACK_SENSOR              4096
// 控制任务里有 ESP_LOGI 打印浮点（内部 vsnprintf 会吃 1KB 左右栈），
// 旧值 3072 偏紧，容易在高频日志时栈溢出
#define TASK_STACK_CONTROL             4096
#define TASK_STACK_COMM                4096
#define TASK_STACK_LED                 2048

#define TASK_PRIORITY_SENSOR           5
#define TASK_PRIORITY_CONTROL          4
#define TASK_PRIORITY_COMM             3
#define TASK_PRIORITY_LED              2

#define TASK_CYCLE_SENSOR              2000
#define TASK_CYCLE_CONTROL             5000
#define TASK_CYCLE_LOG                 60000
#define TASK_CYCLE_HEARTBEAT           1000

// ==================== 传感器硬件校准 ====================
// pH传感器（模拟输入 GPIO1）
#define PH_ADC_VOLTAGE_0_PH            0.0f            // 0V = pH 0
#define PH_ADC_VOLTAGE_14_PH           3.3f            // 3.3V = pH 14
#define PH_TEMP_COEFFICIENT            0.003f          // 温度补偿系数

// TDS传感器（模拟输入 GPIO2）
#define TDS_TEMP_COEFFICIENT           0.02f           // 温度补偿系数

// 浊度计（模拟输入 GPIO3）
#define TURBIDITY_VOLTAGE_CLEAR        2.5f            // 清水电压
#define TURBIDITY_VOLTAGE_MURKY        1.0f            // 浑浊水电压

// ==================== 用户可调阈值（默认值，运行时以 param_store 为准） ====================
// 温度阈值(℃)
#define TEMP_MIN                       22.0f
#define TEMP_MAX                       30.0f
#define TEMP_ALERT_MIN                 18.0f
#define TEMP_ALERT_MAX                 34.0f
#define TEMP_TARGET                    26.0f
#define HEATER_HYSTERESIS              1.0f    // 加热回差温度

// pH阈值
#define PH_MIN                         6.5f
#define PH_MAX                         7.5f
#define PH_ALERT_MIN                   6.0f
#define PH_ALERT_MAX                   8.0f
#define PH_TARGET                      7.0f

// TDS阈值(ppm)
#define TDS_GOOD_MAX                   300
#define TDS_WARNING_MAX                400
#define TDS_ALERT_MAX                  500

// 浊度阈值(NTU)
#define TURBIDITY_GOOD_MAX             5
#define TURBIDITY_WARNING_MAX          10
#define TURBIDITY_ALERT_MAX            20

// 水位阈值(%)
#define WATER_LEVEL_ALERT_MIN          20.0f
#define WATER_LEVEL_AUTO_REFILL        30.0f
#define WATER_LEVEL_MAX                95.0f

// CO₂阈值(ppm)
#define CO2_TARGET_MIN                 400
#define CO2_TARGET_MAX                 800
#define CO2_ALERT_MAX                  1200

// ==================== 控制参数 ====================
#define HEATER_MAX_ON_TIME             3600
#define PUMP_CYCLE_ON_TIME             300
#define PUMP_CYCLE_INTERVAL            3600
#define REFILL_MAX_DURATION            600
#define REFILL_COOLDOWN                3600
#define CO2_DAY_MODE                   1
#define CO2_NIGHT_MODE                 0

// ==================== 传感器数据有效性 ====================
// 超过该时间没有成功采样，则认为数据过期，联锁动作（如加热、补水）必须停止
#define SENSOR_STALE_TIMEOUT_MS        (30 * 1000)
// 采样任务周期（ms）
#define SENSOR_HUB_PERIOD_MS           1000
// 温度转换时间（12bit = 750ms），采样任务用它做节流
#define SENSOR_TEMP_PERIOD_MS          2000
#define SENSOR_CO2_PERIOD_MS           1000

// ==================== 数据记录 ====================
#define SD_LOG_ENABLED                 1
#define SD_LOG_INTERVAL                60000
#define SD_LOG_FILE_MAX_SIZE           (1024 * 1024)
#define SD_LOG_FILE_MAX_COUNT          10

// ==================== 告警配置 ====================
#define ALERT_ENABLE_TEMP              1
#define ALERT_ENABLE_PH                1
#define ALERT_ENABLE_TDS               1
#define ALERT_ENABLE_WATER_LEVEL       1
#define ALERT_ENABLE_CO2               1
#define ALERT_RETRY_COUNT              3

// ==================== 定时控制 ====================
// 灯光定时（20W LED）
#define LIGHT_ON_HOUR                  8                // 开灯时间(8:00)
#define LIGHT_ON_MINUTE                0
#define LIGHT_OFF_HOUR                 18               // 关灯时间(18:00)
#define LIGHT_OFF_MINUTE               0
#define LIGHT_MORNING_BRIGHTNESS       100              // 早晨亮度(%)
#define LIGHT_DAY_BRIGHTNESS           100              // 白天亮度(%)
#define LIGHT_EVENING_BRIGHTNESS       100              // 傍晚亮度(%)

// 喂食定时（每天2次）
#define FEEDING_TIME_1_HOUR            8        // 第一次喂食(8:00)
#define FEEDING_TIME_1_MINUTE          0        // 分钟
#define FEEDING_TIME_2_HOUR            17       // 第二次喂食(17:30)
#define FEEDING_TIME_2_MINUTE          30       // 分钟
#define FEEDER_ANGLE                   120
#define FEEDER_RETURN_DELAY_MS         1500

// 紫外灯定时（凌晨 01:30 到 03:00）
#define UV_LAMP_ON_HOUR                1
#define UV_LAMP_ON_MINUTE              30
#define UV_LAMP_OFF_HOUR               3
#define UV_LAMP_OFF_MINUTE             0

// ==================== WiFi配置 ====================
#ifdef ENABLE_WIFI
    #define WIFI_SSID                  ""
    #define WIFI_PASSWORD              ""
    #define WIFI_MAX_RETRY             5
    #define WIFI_CONNECT_TIMEOUT       10000
#endif

// ==================== MQTT配置 ====================
#ifdef ENABLE_MQTT
    #define MQTT_BROKER_URL            "mqtt://192.168.1.100"
    #define MQTT_PORT                  1883
    #define MQTT_CLIENT_ID             "FishTank1"
    #define MQTT_KEEPALIVE             60
    #define MQTT_TOPIC_PUB             "fish_tank/data"
    #define MQTT_TOPIC_SUB             "fish_tank/control"
#endif

// ==================== 调试配置 ====================
#define DEBUG_LEVEL                    3

#if DEBUG_LEVEL >= 3
    #define LOG_I(tag, fmt, ...)       printf("[I][%s] " fmt "\n", tag, ##__VA_ARGS__)
#else
    #define LOG_I(tag, fmt, ...)
#endif

#if DEBUG_LEVEL >= 2
    #define LOG_W(tag, fmt, ...)       printf("[W][%s] " fmt "\n", tag, ##__VA_ARGS__)
#else
    #define LOG_W(tag, fmt, ...)
#endif

#if DEBUG_LEVEL >= 1
    #define LOG_E(tag, fmt, ...)       printf("[E][%s] " fmt "\n", tag, ##__VA_ARGS__)
#else
    #define LOG_E(tag, fmt, ...)
#endif

// ==================== 上位机调试默认值 ====================
#define DEBUG_MANUAL_TIMEOUT_S         120     // 手动模式默认超时，超时自动回到 AUTO
#define DEBUG_STREAM_PERIOD_MS         1000    // 遥测默认周期
#define DEBUG_LINE_MAX                 192     // 单条命令行最大长度
#define DEBUG_MAX_FIELDS               12      // 一条命令最多字段数
#define DEBUG_REPLY_MAX                768     // 单条回复最大长度

// ==================== 系统状态 ====================
typedef enum {
    SYSTEM_STATE_INIT = 0,
    SYSTEM_STATE_RUNNING,
    SYSTEM_STATE_ERROR,
    SYSTEM_STATE_LOW_POWER,
    SYSTEM_STATE_OTA_UPDATE
} SystemState_t;

typedef enum {
    ERROR_NONE = 0,
    ERROR_I2C_BUS,
    ERROR_SD_CARD,
    ERROR_TEMP_SENSOR,
    ERROR_PH_SENSOR,
    ERROR_WATER_LEVEL,
    ERROR_WIFI,
    ERROR_MQTT
} ErrorCode_t;

// ==================== 默认配置 ====================
#define DEFAULT_TEMP_TARGET            26.0f
#define DEFAULT_PH_TARGET              7.0f
#define DEFAULT_LIGHT_BRIGHTNESS       100
#define DEFAULT_AUTO_HEATER            1
#define DEFAULT_AUTO_PUMP              1
#define DEFAULT_AUTO_LIGHT             1
#define DEFAULT_AUTO_FEEDER            1

// ==================== 系统保护 ====================
#define WATCHDOG_TIMEOUT_MS            30000
#define WATCHDOG_ENABLE                1
#define BATTERY_LOW_THRESHOLD          3.3f
#define BATTERY_SHUTDOWN_THRESHOLD     3.0f

#endif // SYSTEM_CONFIG_H
