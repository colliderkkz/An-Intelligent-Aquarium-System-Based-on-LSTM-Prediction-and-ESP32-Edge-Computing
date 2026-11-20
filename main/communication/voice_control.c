#include "communication/voice_control.h"

#include "actuators/co2_valve.h"
#include "actuators/feeder.h"
#include "actuators/heater.h"
#include "actuators/light_actuator.h"
#include "actuators/pump.h"
#include "actuators/uv_lamp.h"
#include "actuators/water_pump.h"
#include "communication/uart_receiver.h"
#include "communication/voice_protocol.h"
#include "config/param_store.h"
#include "config/pin_config.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "sensors/light_sensor.h"
#include "sensors/ph_sensor.h"
#include "sensors/tds_sensor.h"
#include "sensors/temp_sensor.h"
#include "sensors/turbidity_sensor.h"
#include "sensors/water_level.h"
#include "service/control_mode.h"
#include "service/act_hold.h"
#include "service/sensor_hub.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "VOICE_CTRL";

typedef struct {
    const char *phrase;
    uint8_t cmd;
} voice_phrase_map_t;

static const voice_phrase_map_t s_phrase_map[] = {
    {"打开灯光", VOICE_CMD_LIGHT_ON},
    {"关闭灯光", VOICE_CMD_LIGHT_OFF},
    {"打开水泵", VOICE_CMD_PUMP_ON},
    {"关闭水泵", VOICE_CMD_PUMP_OFF},
    {"打开加热", VOICE_CMD_HEATER_ON},
    {"关闭加热", VOICE_CMD_HEATER_OFF},
    {"打开CO2", VOICE_CMD_CO2_ON},
    {"关闭CO2", VOICE_CMD_CO2_OFF},
    {"开始喂食", VOICE_CMD_FEED},
    {"当前温度", VOICE_CMD_QUERY_TEMP},
    {"当前pH", VOICE_CMD_QUERY_PH},
    {"当前TDS", VOICE_CMD_QUERY_TDS},
    {"鱼缸状态", VOICE_CMD_QUERY_ALL},
};

static bool s_started = false;

/* ---- 诊断用 ---- */
static volatile bool s_sniff = false;
static volatile uint32_t s_rx_frames = 0;
static volatile uint32_t s_unmapped = 0;
static volatile int s_baud = VOICE_UART_BAUD_RATE;
static char s_last_hex[80] = "-";
static char s_last_text[48] = "-";

static void voice_uart_write_line(const char *text) {
    if (!text) {
        return;
    }
    (void)uart_write_bytes(VOICE_UART_NUM, text, (size_t)strlen(text));
    (void)uart_write_bytes(VOICE_UART_NUM, "\r\n", 2);
    /* 没接喇叭时语音播报听不到，所以把回答也写进日志：
     * 日志会进 SD 卡、并通过 MQTT sdline 推到手机/上位机。 */
    ESP_LOGI(TAG, "SPEAK> %s", text);
}

static bool line_contains_uv_command(const char *line, bool *turn_on) {
    if (!line || !turn_on) {
        return false;
    }

    bool mentions_uv = (strstr(line, "紫外") != NULL) || (strstr(line, "UV") != NULL) ||
                       (strstr(line, "uv") != NULL);
    bool mentions_lamp = (strstr(line, "灯") != NULL) || (strstr(line, "lamp") != NULL);
    if (!mentions_uv || !mentions_lamp) {
        return false;
    }

    if (strstr(line, "打开") != NULL || strstr(line, "开") != NULL) {
        *turn_on = true;
        return true;
    }
    if (strstr(line, "关闭") != NULL || strstr(line, "关") != NULL) {
        *turn_on = false;
        return true;
    }
    return false;
}

uint8_t voice_control_match_command(const char *line) {
    if (!line || !line[0]) {
        return 0;
    }

    bool turn_on = false;
    if (line_contains_uv_command(line, &turn_on)) {
        return turn_on ? VOICE_CMD_UV_ON : VOICE_CMD_UV_OFF;
    }

    for (size_t i = 0; i < sizeof(s_phrase_map) / sizeof(s_phrase_map[0]); i++) {
        if (strcmp(line, s_phrase_map[i].phrase) == 0) {
            return s_phrase_map[i].cmd;
        }
    }

    /* 容忍 ASRPRO 输出带标点/空格：退化为包含匹配 */
    for (size_t i = 0; i < sizeof(s_phrase_map) / sizeof(s_phrase_map[0]); i++) {
        if (strstr(line, s_phrase_map[i].phrase) != NULL) {
            return s_phrase_map[i].cmd;
        }
    }

    return 0;
}

/* 把原始字节记成 hex，方便检测时看清模块到底发了什么 */
static void record_frame(const char *line) {
    size_t n = strlen(line);
    char hex[80];
    size_t p = 0;
    for (size_t i = 0; i < n && i < 16 && p + 4 < sizeof(hex); i++) {
        p += (size_t)snprintf(hex + p, sizeof(hex) - p, "%02X ", (unsigned char)line[i]);
    }
    if (p == 0) {
        snprintf(hex, sizeof(hex), "(empty)");
    } else {
        hex[p ? p - 1 : 0] = '\0';
    }
    strncpy(s_last_hex, hex, sizeof(s_last_hex) - 1);
    s_last_hex[sizeof(s_last_hex) - 1] = '\0';

    char clean[48];
    size_t k = 0;
    for (size_t i = 0; i < n && k < sizeof(clean) - 1; i++) {
        unsigned char c = (unsigned char)line[i];
        clean[k++] = (c >= 0x20 && c < 0x7F) || c >= 0x80 ? (char)c : '.';
    }
    clean[k] = '\0';
    strncpy(s_last_text, clean, sizeof(s_last_text) - 1);
    s_last_text[sizeof(s_last_text) - 1] = '\0';
}

static void voice_receiver_callback(const char *line, void *user_ctx) {
    (void)user_ctx;
    if (!line || !line[0]) {
        return;
    }

    s_rx_frames++;
    record_frame(line);

    if (s_sniff) {
        ESP_LOGW(TAG, "RX %uB hex=[%s] text=%s", (unsigned)strlen(line), s_last_hex, s_last_text);
    }

    /* 情况 A：ASRPRO 直接发裸字节命令 0x01~0x13 */
    if (strlen(line) == 1) {
        uint8_t b = (uint8_t)(unsigned char)line[0];
        if (b >= VOICE_CMD_LIGHT_ON && b <= VOICE_CMD_QUERY_ALL) {
            ESP_LOGI(TAG, "Voice byte cmd 0x%02X", b);
            (void)voice_control_execute_command(b, line);
            return;
        }
    }

    /* 情况 B：ASRPRO 发中文短语文本 */
    uint8_t cmd = voice_control_match_command(line);
    if (cmd == 0) {
        s_unmapped++;
        ESP_LOGI(TAG, "Unmapped voice phrase: %s (hex %s)", s_last_text, s_last_hex);
        return;
    }

    ESP_LOGI(TAG, "Voice phrase: %s -> 0x%02X", s_last_text, cmd);
    (void)voice_control_execute_command(cmd, line);
}

esp_err_t voice_control_init(void) {
    /* 传感器/ADC 由 sensor_hub 统一初始化，这里不再重复配置硬件 */
    return ESP_OK;
}

static void format_all_status(char *buf, size_t len) {
    const sensor_sample_t *t = sensor_hub_get(SH_TEMP);
    const sensor_sample_t *ph = sensor_hub_get(SH_PH);
    const sensor_sample_t *tds = sensor_hub_get(SH_TDS);
    const sensor_sample_t *lv = sensor_hub_get(SH_LEVEL);
    const sensor_sample_t *li = sensor_hub_get(SH_LIGHT);
    const sensor_sample_t *tu = sensor_hub_get(SH_TURBIDITY);

    snprintf(buf, len, "T%.1fC PH%.1f TDS%.0f L%.0f%% LX%.0f%% TU%.0f",
             t && t->valid ? (double)t->value : -1.0,
             ph && ph->valid ? (double)ph->value : -1.0,
             tds && tds->valid ? (double)tds->value : -1.0,
             lv && lv->valid ? (double)lv->value : -1.0,
             li && li->valid ? (double)li->value : -1.0,
             tu && tu->valid ? (double)tu->value : -1.0);
}

/** 手动保持时长，和手机 App 保持一致 */
#define VOICE_MANUAL_HOLD_S 120

/** 语音命令 -> 执行器 id（用于 act_hold_set），查询类返回 NULL */
static const char *voice_act_name(uint8_t cmd_id) {
    switch (cmd_id) {
        case VOICE_CMD_LIGHT_ON:
        case VOICE_CMD_LIGHT_OFF:
            return "light";
        case VOICE_CMD_PUMP_ON:
        case VOICE_CMD_PUMP_OFF:
            return "pump";
        case VOICE_CMD_HEATER_ON:
        case VOICE_CMD_HEATER_OFF:
            return "heater";
        case VOICE_CMD_CO2_ON:
        case VOICE_CMD_CO2_OFF:
            return "co2";
        case VOICE_CMD_UV_ON:
        case VOICE_CMD_UV_OFF:
            return "uv";
        default:
            return NULL;
    }
}

esp_err_t voice_control_execute_command(uint8_t cmd_id, const char *raw_text) {
    (void)raw_text;

    /* 除了“查询”以外，语音动作都视为手动干预：暂停自动控制一小段时间，
     * 否则自动策略会在下一秒钟把用户的操作改回去。 */
    switch (cmd_id) {
        case VOICE_CMD_QUERY_TEMP:
        case VOICE_CMD_QUERY_PH:
        case VOICE_CMD_QUERY_TDS:
        case VOICE_CMD_QUERY_ALL:
            break;
        default: {
            /* 原来只调 control_mode_enter_manual()：它只暂停"计划"，
             * 而温度/补水/常开泵这些自动逻辑照样会在 2 秒内把用户刚关掉的
             * 设备打开 —— 表现就是"语音关不掉"。这里补上执行器级保持，
             * 和手机 App 的操作走同一套策略。 */
            control_mode_enter_manual(VOICE_MANUAL_HOLD_S);
            const char *an = voice_act_name(cmd_id);
            if (an) {
                act_hold_set(an, VOICE_MANUAL_HOLD_S);
            }
            break;
        }
    }

    ESP_LOGI(TAG, "EXEC cmd=0x%02X (%s)", cmd_id, raw_text ? raw_text : "");

    switch (cmd_id) {
        case VOICE_CMD_LIGHT_ON:
            return light_actuator_set_brightness(100.0f);
        case VOICE_CMD_LIGHT_OFF:
            return light_actuator_set_brightness(0.0f);
        case VOICE_CMD_PUMP_ON:
            return pump_set_state(true);
        case VOICE_CMD_PUMP_OFF: {
            /* 循环泵有"常开"策略，会周期性拉回满档：语音关泵要一起关掉它 */
            esp_err_t r = pump_set_state(false);
            if (r == ESP_OK) {
                (void)param_set(PARAM_IDX_PUMP_ALWAYS_ON, 0.0f);
                (void)param_store_save();   /* 语音是低频操作，直接落盘即可 */
            }
            return r;
        }
        case VOICE_CMD_HEATER_ON:
            return heater_set_state(true);
        case VOICE_CMD_HEATER_OFF:
            return heater_set_state(false);
        case VOICE_CMD_CO2_ON:
            return co2_valve_set_state(true);
        case VOICE_CMD_CO2_OFF:
            return co2_valve_set_state(false);
        case VOICE_CMD_FEED:
            return feeder_feed_now();
        case VOICE_CMD_UV_ON:
            return uv_lamp_set_state(true);
        case VOICE_CMD_UV_OFF:
            return uv_lamp_set_state(false);

        case VOICE_CMD_QUERY_TEMP: {
            float temp_c = 25.0f;
            esp_err_t ret = sensor_temp_read_c(&temp_c);
            if (ret == ESP_OK) {
                char buf[48];
                snprintf(buf, sizeof(buf), "TEMP %.1fC", (double)temp_c);
                voice_uart_write_line(buf);
            } else {
                voice_uart_write_line("TEMP READ FAILED");
            }
            return ret;
        }

        case VOICE_CMD_QUERY_PH: {
            float ph = 0.0f;
            esp_err_t ret = sensor_ph_read(&ph);
            if (ret == ESP_OK) {
                char buf[48];
                snprintf(buf, sizeof(buf), "PH %.1f", (double)ph);
                voice_uart_write_line(buf);
            } else {
                voice_uart_write_line("PH READ FAILED");
            }
            return ret;
        }

        case VOICE_CMD_QUERY_TDS: {
            float tds = 0.0f;
            esp_err_t ret = sensor_tds_read_ppm(&tds);
            if (ret == ESP_OK) {
                char buf[48];
                snprintf(buf, sizeof(buf), "TDS %.0fppm", (double)tds);
                voice_uart_write_line(buf);
            } else {
                voice_uart_write_line("TDS READ FAILED");
            }
            return ret;
        }

        case VOICE_CMD_QUERY_ALL: {
            /* 用缓存值回答，避免语音查询要等 3 秒 */
            char buf[128];
            format_all_status(buf, sizeof(buf));
            voice_uart_write_line(buf);
            return ESP_OK;
        }

        default:
            return ESP_ERR_NOT_SUPPORTED;
    }
}

esp_err_t voice_control_start(void) {
    if (s_started) {
        return ESP_OK;
    }

    /* 语音会用到水位的读数（查询全部状态），确保 ADC 已被 sensor_hub 初始化 */
    (void)sensor_water_level_init();

    uart_receiver_config_t cfg = {
        .uart_num = VOICE_UART_NUM,
        .tx_pin = VOICE_UART_TX_PIN,
        .rx_pin = VOICE_UART_RX_PIN,
        .baud_rate = VOICE_UART_BAUD_RATE,
        .rx_buffer_size = VOICE_UART_BUF_SIZE,
        .line_buffer_size = 128,
        .callback = voice_receiver_callback,
        .user_ctx = NULL,
        .task_name = "voice_uart_rx",
        .task_stack_size = 4096,
        .task_priority = 5,
    };

    esp_err_t ret = uart_receiver_start(&cfg);
    if (ret == ESP_OK) {
        s_started = true;
    }
    return ret;
}

/* ==================== 诊断接口 ==================== */

void voice_control_set_sniff(bool on) { s_sniff = on; }
bool voice_control_sniff_on(void) { return s_sniff; }
uint32_t voice_control_rx_frames(void) { return s_rx_frames; }
uint32_t voice_control_unmapped(void) { return s_unmapped; }
const char *voice_control_last_hex(void) { return s_last_hex; }
const char *voice_control_last_text(void) { return s_last_text; }
bool voice_control_is_started(void) { return s_started; }
int voice_control_baud(void) { return s_baud; }

esp_err_t voice_control_send(const char *text) {
    if (!text || !s_started) {
        return ESP_ERR_INVALID_ARG;
    }
    voice_uart_write_line(text);
    ESP_LOGI(TAG, "TX -> %s", text);
    return ESP_OK;
}

esp_err_t voice_control_set_baud(int baud) {
    if (baud < 1200 || baud > 921600) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t r = uart_set_baudrate(VOICE_UART_NUM, (uint32_t)baud);
    if (r == ESP_OK) {
        s_baud = baud;
        ESP_LOGI(TAG, "baud -> %d", baud);
    }
    return r;
}
