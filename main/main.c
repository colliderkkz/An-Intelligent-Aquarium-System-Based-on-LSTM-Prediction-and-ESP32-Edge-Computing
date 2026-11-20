/**
 * @file main.c
 * @brief 智能鱼缸过滤系统 —— 系统入口
 *
 * 启动顺序（顺序很重要，上位机调试命令任何时候都可能到来）：
 *   1. NVS + 参数表（阈值/标定可从 NVS 恢复）
 *   2. I2C 总线 + RTC/时钟服务
 *   3. 所有执行器初始化（先全部置为安全状态）
 *   4. 传感器采集中枢 sensor_hub（统一采样 + 缓存）
 *   5. 调试控制台（UART0，上位机可以开始点动/读数）
 *   6. 语音控制（UART2）
 *   7. 自动控制任务（温控/补水/灯光/喂食/紫外）
 *
 * 所有自动控制任务都会检查 control_mode_is_manual()：一旦上位机或语音做过
 * 手动操作，自动策略会暂停，超时后自动恢复，避免“抢引脚”。
 */
#include <stdio.h>
#include <string.h>

#include "actuators/air_pump.h"
#include "actuators/netlea_lamp.h"
#include "actuators/pump.h"
#include "bus/i2c_bus.h"
#include "communication/voice_control.h"
#include "config/error_codes.h"
#include "config/param_store.h"
#include "config/pin_config.h"
#include "config/system_config.h"
#include "control/feeding_control.h"
#include "control/light_control.h"
#include "control/temp_control.h"
#include "control/uv_control.h"
#include "control/water_control.h"
#include "debug/debug_console.h"
#include "drivers/pcf8563.h"
#include "drivers/sd_card_spi.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "service/control_mode.h"
#include "net/mqtt_app.h"
#include "net/sntp_sync.h"
#include "net/wifi_manager.h"
#include "service/sd_logger.h"
#include "service/schedule.h"
#include "service/sensor_hub.h"
#include "service/time_service.h"
#include "service/power_diag.h"

static const char *TAG = "MAIN";

/* 温控/补水的判断周期（数据来自 sensor_hub 缓存，本身很快） */
#define CONTROL_PERIOD_MS      2000
/* 灯光/紫外/喂食的定时判断周期（需要分钟级精度） */
#define SCHEDULE_PERIOD_MS     1000

/* 是否在启动时挂载 SD 卡（挂载失败不影响系统运行） */
#define SD_CARD_MOUNT_ON_BOOT  1

static i2c_bus_handle_t g_i2c_bus = NULL;

/* 各控制模块初始化结果：某个模块失败时只跳过它，不影响其它调度 */
static bool s_light_ok = false;
static bool s_uv_ok = false;
static bool s_feed_ok = false;
static bool s_temp_ok = false;
static bool s_water_ok = false;

/* ------------------------------------------------------------------ */
/* 定时调度任务：灯光 / CO2 / 紫外 / 喂食                                */
/* ------------------------------------------------------------------ */
static void schedule_task(void *pvParameters) {
    (void)pvParameters;

    while (1) {
        pcf8563_time_t now = {0};
        time_service_now(&now);

        /* 定时计划优先：启用了计划的执行器由它接管 */
        schedule_update(&now);

        if (s_light_ok) {
            esp_err_t ret = light_control_update(&now);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "light control: %s", app_err_name(ret));
            }
        }

        if (s_uv_ok) {
            esp_err_t ret = uv_control_update(&now);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "uv control: %s", app_err_name(ret));
            }
        }

        /* 喂食会阻塞数秒（舵机保持 + 回位），放在本任务最后 */
        if (s_feed_ok) {
            esp_err_t ret = feeding_control_update(&now);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "feeding control: %s", app_err_name(ret));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(SCHEDULE_PERIOD_MS));
    }
}

/* ------------------------------------------------------------------ */
/* 自动控制任务：温控 / 补水 / 循环泵策略                                */
/* ------------------------------------------------------------------ */
static void control_task(void *pvParameters) {
    (void)pvParameters;

    bool stale_reported = false;

    while (1) {
        if (s_temp_ok) {
            esp_err_t ret = temp_control_update();
            if (ret == APP_ERR_SENSOR_STALE) {
                if (!stale_reported) {
                    ESP_LOGE(TAG, "temperature data stale, heater interlocked OFF");
                    stale_reported = true;
                }
            } else if (ret != ESP_OK) {
                ESP_LOGW(TAG, "temp control: %s", app_err_name(ret));
            } else {
                stale_reported = false;
            }
        }

        if (s_water_ok) {
            esp_err_t ret = water_control_update();
            /* 限流：没接水位探头时每轮都是 SENSOR_STALE，不节流的话
             * 每 2 秒刷一条，SD 日志文件很快被它灌满。只在"状态变化"时打一条。 */
            static int last_water_err = 0;
            const int cur = (ret == ESP_OK) ? 0 : (int)ret;
            if (cur != last_water_err) {
                last_water_err = cur;
                if (cur == 0) {
                    ESP_LOGI(TAG, "water control recovered");
                } else if (ret != ESP_ERR_INVALID_STATE) {
                    ESP_LOGW(TAG, "water control: %s (will stay quiet until it changes)",
                             app_err_name(ret));
                }
            }
        }

        /* 循环过滤泵：默认常开。
         * BUGFIX：如果给循环泵配了定时计划，这里就不能再抢 —— 否则
         * 计划在窗口外把它关掉，2 秒后又被拉回开，计划的"关"永远失效。 */
        int pump_sched = -1;
        const bool pump_scheduled =
            schedule_find("pump", &pump_sched) && schedule_is_active(pump_sched);
        if (!control_mode_is_manual() && control_mode_auto_enabled() &&
            !pump_scheduled &&
            param_get_bool(PARAM_IDX_PUMP_ALWAYS_ON) && !pump_get_state()) {
            (void)pump_set_state(true);
        }

        vTaskDelay(pdMS_TO_TICKS(CONTROL_PERIOD_MS));
    }
}

/* ------------------------------------------------------------------ */
/* 启动                                                               */
/* ------------------------------------------------------------------ */
static esp_err_t init_nvs(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase (%s), erasing...", esp_err_to_name(ret));
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

static void init_i2c(void) {
    i2c_bus_config_t i2c_cfg = {
        .port = I2C_MASTER_PORT,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .clk_speed_hz = I2C_MASTER_FREQ_HZ,
        .pullup_enable = true,
        .timeout_ms = I2C_MASTER_TIMEOUT_MS,
    };
    g_i2c_bus = i2c_bus_init(&i2c_cfg);
    if (!g_i2c_bus) {
        ESP_LOGE(TAG, "I2C bus init failed, RTC/CO2 unavailable");
        return;
    }

    esp_err_t ret = pcf8563_init(g_i2c_bus, PCF8563_I2C_ADDR);
    if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "RTC lost power (VL=1): use RTC,SET to calibrate on the host tool");
    } else if (ret != ESP_OK) {
        ESP_LOGW(TAG, "PCF8563 init failed: %s", esp_err_to_name(ret));
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "==============================================");
    ESP_LOGI(TAG, " %s v%d.%d.%d", SYSTEM_NAME,
             SYSTEM_VERSION_MAJOR, SYSTEM_VERSION_MINOR, SYSTEM_VERSION_PATCH);
    ESP_LOGI(TAG, " board: %s", BOARD_NAME);
    ESP_LOGI(TAG, "==============================================");

    esp_err_t ret = init_nvs();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s (parameters will not persist)", esp_err_to_name(ret));
    }

    /* 记录本次复位原因：拔插电源后可以据此判断是"正常上电"还是"欠压复位" */
    (void)power_diag_init();

    (void)param_store_init();
    control_mode_init();
    (void)schedule_init();

    init_i2c();
    (void)time_service_init(g_i2c_bus);

    /* ---- 执行器：全部先回到安全状态 ---- */
    s_light_ok = (light_control_init() == ESP_OK);
    s_temp_ok = (temp_control_init() == ESP_OK);
    s_feed_ok = (feeding_control_init() == ESP_OK);
    s_water_ok = (water_control_init() == ESP_OK);
    s_uv_ok = (uv_control_init() == ESP_OK);
    esp_err_t pump_ret = pump_init();

    /* 气泵：目前只做上位机/手动点动，不参与自动策略（没有溶氧传感器） */
    if (air_pump_init() != ESP_OK) {
        ESP_LOGW(TAG, "air pump init failed or disabled (AIR_PUMP_ENABLED=%d)", AIR_PUMP_ENABLED);
    }

    if (pump_ret == ESP_OK) {
        (void)pump_set_state(param_get_bool(PARAM_IDX_PUMP_ALWAYS_ON));
    } else {
        ESP_LOGW(TAG, "circulation pump init failed");
    }

    /* ---- 尼特利 BLE 灯：与板载 PWM 灯二选一 ----
     * 必须放在 light_control_init() 之后：切换光源时要能直接驱动 LEDC。
     * 初始化失败也不影响原有功能，会自动退回板载灯/模拟器。 */
    if (netlea_lamp_init() != ESP_OK) {
        ESP_LOGW(TAG, "netlea lamp init failed (board PWM still works)");
    }

    /* ---- 传感器采集中枢 ---- */
    ret = sensor_hub_init(g_i2c_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "sensor hub init failed: %s", esp_err_to_name(ret));
    }
    if (sensor_hub_start() != ESP_OK) {
        ESP_LOGE(TAG, "sensor hub task start failed");
    }

    /* ---- 上位机调试控制台（尽早启动，方便现场排障）---- */
    ret = debug_console_start(g_i2c_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "debug console start failed: %s", esp_err_to_name(ret));
    }

    /* ---- SD 卡（可选）----
     * 不在此处同步挂载：sdspi 对某些卡/接线会长时间阻塞，
     * 会把 app_main 卡死、上位机连上也没响应。
     * 现在由 sd_logger 任务在后台重试挂载。 */
    if (sd_logger_start() != ESP_OK) {
        ESP_LOGW(TAG, "sd logger task start failed");
    }

    /* 掉电诊断摘要：必须放在 sd_logger 启动之后打，
     * 否则这条日志进不了 SD 卡和 NAS，拔插电源后就查不到历史了。 */
    ESP_LOGW(TAG, "POWER: boot#%lu reset=%s brownout=%lu poweron=%lu prev=%s",
             (unsigned long)power_diag_boot_count(),
             power_diag_reset_reason_text(),
             (unsigned long)power_diag_brownout_count(),
             (unsigned long)power_diag_poweron_count(),
             power_diag_last_reason_text());

    /* ---- 网络：WiFi + MQTT 远程访问（可选，不影响本地功能）----
     * WiFi 连接放到后台任务里做：连不上时 net_connect() 最坏要试 20 秒，
     * 放在 app_main 里会把后面的语音/自动控制任务全部拖住（以前就是这样：
     * 没网时加热控制要等半分钟才开始跑）。 */
    if (net_init() == ESP_OK) {
        if (net_has_config()) {
            (void)net_connect_async();
        } else {
            ESP_LOGW(TAG, "wifi not configured; use NET,SET,... on the host tool");
        }
        (void)mqtt_app_start();
        (void)sntp_sync_start();
    } else {
        ESP_LOGW(TAG, "network init failed, remote access disabled");
    }

    /* ---- 语音 ---- */
    if (voice_control_init() == ESP_OK && voice_control_start() == ESP_OK) {
        ESP_LOGI(TAG, "voice control started on UART%d", (int)VOICE_UART_NUM);
    } else {
        ESP_LOGW(TAG, "voice control unavailable");
    }

    /* ---- 自动控制任务 ---- */
    if (s_light_ok || s_uv_ok || s_feed_ok) {
        xTaskCreate(schedule_task, "schedule", TASK_STACK_CONTROL, NULL,
                    TASK_PRIORITY_CONTROL, NULL);
    } else {
        ESP_LOGW(TAG, "schedule task not started (light/uv/feed all failed)");
    }

    if (s_temp_ok || s_water_ok) {
        xTaskCreate(control_task, "control", TASK_STACK_CONTROL, NULL,
                    TASK_PRIORITY_CONTROL, NULL);
    } else {
        ESP_LOGW(TAG, "control task not started (temp/water both failed)");
    }

    ESP_LOGI(TAG, "system ready. connect the USB-UART port at %d baud and run the host tool",
             DEBUG_UART_BAUD_RATE);
}
