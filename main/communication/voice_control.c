#include "communication/voice_control.h"

#include "actuators/co2_valve.h"
#include "actuators/feeder.h"
#include "actuators/heater.h"
#include "actuators/light_actuator.h"
#include "actuators/pump.h"
#include "actuators/uv_lamp.h"
#include "communication/uart_receiver.h"
#include "communication/voice_protocol.h"
#include "config/pin_config.h"
#include "drivers/adc/adc_manager.h"
#include "drivers/adc/light_sensor.h"
#include "drivers/adc/ph_sensor_adc.h"
#include "drivers/adc/tds_sensor.h"
#include "drivers/adc/turbidity.h"
#include "drivers/adc/water_level.h"
#include "drivers/ds18b20.h"
#include "driver/uart.h"
#include "esp_log.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "VOICE_CTRL";

typedef struct {
    const char *token;
    uint8_t cmd;
} voice_token_map_t;

static const voice_token_map_t s_ascii_token_map[] = {
    {"LIGHT_ON", VOICE_CMD_LIGHT_ON},
    {"LIGHT_OFF", VOICE_CMD_LIGHT_OFF},
    {"PUMP_ON", VOICE_CMD_PUMP_ON},
    {"PUMP_OFF", VOICE_CMD_PUMP_OFF},
    {"HEATER_ON", VOICE_CMD_HEATER_ON},
    {"HEATER_OFF", VOICE_CMD_HEATER_OFF},
    {"CO2_ON", VOICE_CMD_CO2_ON},
    {"CO2_OFF", VOICE_CMD_CO2_OFF},
    {"FEED", VOICE_CMD_FEED},
    {"FEED_NOW", VOICE_CMD_FEED},
    {"UV_ON", VOICE_CMD_UV_ON},
    {"UV_OFF", VOICE_CMD_UV_OFF},
    {"TEMP", VOICE_CMD_QUERY_TEMP},
    {"QUERY_TEMP", VOICE_CMD_QUERY_TEMP},
    {"PH", VOICE_CMD_QUERY_PH},
    {"QUERY_PH", VOICE_CMD_QUERY_PH},
    {"TDS", VOICE_CMD_QUERY_TDS},
    {"QUERY_TDS", VOICE_CMD_QUERY_TDS},
    {"STATUS", VOICE_CMD_QUERY_ALL},
    {"QUERY_ALL", VOICE_CMD_QUERY_ALL},
};

static bool s_started = false;

static bool is_supported_cmd(uint8_t cmd) {
    return ((cmd >= VOICE_CMD_LIGHT_ON) && (cmd <= VOICE_CMD_UV_OFF)) ||
           ((cmd >= VOICE_CMD_QUERY_TEMP) && (cmd <= VOICE_CMD_QUERY_ALL));
}

static bool is_query_cmd(uint8_t cmd) {
    return (cmd >= VOICE_CMD_QUERY_TEMP) && (cmd <= VOICE_CMD_QUERY_ALL);
}

static void voice_uart_write_line(const char *text) {
    if (!text) {
        return;
    }

    (void)uart_write_bytes(VOICE_UART_NUM, text, (size_t)strlen(text));
    (void)uart_write_bytes(VOICE_UART_NUM, "\r\n", 2);
}

static void normalize_ascii_command(const char *line, char *out, size_t out_size) {
    if (!line || !out || out_size == 0) {
        return;
    }

    size_t j = 0;
    for (size_t i = 0; line[i] != '\0' && j < out_size - 1; i++) {
        unsigned char c = (unsigned char)line[i];
        if (c == ' ' || c == '\t' || c == '-' || c == ':' || c == '=') {
            c = '_';
        }
        if (c >= 0x20 && c <= 0x7E) {
            out[j++] = (char)toupper(c);
        }
    }
    out[j] = '\0';
}

static bool parse_prefixed_number(const char *line, uint8_t *cmd) {
    if (!line || !cmd) {
        return false;
    }

    while (*line == ' ' || *line == '\t') {
        line++;
    }

    if ((line[0] == 'C' || line[0] == 'c') &&
        (line[1] == 'M' || line[1] == 'm') &&
        (line[2] == 'D' || line[2] == 'd')) {
        line += 3;
        while (*line == ' ' || *line == '\t' || *line == ':' || *line == '=') {
            line++;
        }
    }

    int base = 10;
    if (line[0] == '0' && (line[1] == 'x' || line[1] == 'X')) {
        base = 16;
    }

    char *end = NULL;
    long value = strtol(line, &end, base);
    while (end && (*end == ' ' || *end == '\t')) {
        end++;
    }

    if (!end || end == line || *end != '\0' || value <= 0 || value > 0xFF) {
        return false;
    }

    *cmd = (uint8_t)value;
    return true;
}

static bool parse_hex_byte_sequence(const char *line, uint8_t *bytes, size_t max_bytes, size_t *out_len) {
    if (!line || !bytes || max_bytes == 0 || !out_len) {
        return false;
    }

    size_t count = 0;
    const char *p = line;
    while (*p != '\0') {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '\0') {
            break;
        }

        if (!isxdigit((unsigned char)p[0]) || !isxdigit((unsigned char)p[1])) {
            return false;
        }
        if (count >= max_bytes) {
            return false;
        }

        char token[3] = {p[0], p[1], '\0'};
        bytes[count++] = (uint8_t)strtoul(token, NULL, 16);
        p += 2;

        while (*p == ' ' || *p == '\t') {
            p++;
        }
    }

    *out_len = count;
    return count > 0;
}

static uint8_t match_asrpro_hex_frame(const char *line) {
    uint8_t bytes[16];
    size_t len = 0;
    if (!parse_hex_byte_sequence(line, bytes, sizeof(bytes), &len)) {
        return 0;
    }

    if (len == 4 && bytes[0] == 0xFF && bytes[1] == 0x00 && bytes[3] == 0x01) {
        return is_supported_cmd(bytes[2]) ? bytes[2] : 0;
    }

    if (len == 5 && bytes[0] == 0xFF && bytes[1] == 0x00 && bytes[2] == 0x00 && bytes[3] == 0x00 && bytes[4] == 0xFF) {
        return 0xFF;
    }

    return 0;
}

static uint8_t match_binary_command(const char *line) {
    if (!line) {
        return 0;
    }

    if (strlen(line) != 1) {
        return 0;
    }

    uint8_t cmd = (uint8_t)line[0];
    return is_supported_cmd(cmd) ? cmd : 0;
}

static bool is_wake_token(const char *line) {
    char normalized[64] = {0};
    normalize_ascii_command(line, normalized, sizeof(normalized));
    return (strcmp(normalized, "SMART_BUTLER") == 0) ||
           (strcmp(normalized, "VOICE_BUTLER") == 0) ||
           (strcmp(normalized, "WAKE") == 0);
}

static void send_ack(uint8_t cmd_id) {
    char buf[16];
    snprintf(buf, sizeof(buf), "ACK%02X", cmd_id);
    voice_uart_write_line(buf);
}

static void send_error(uint8_t cmd_id) {
    char buf[16];
    snprintf(buf, sizeof(buf), "ERR%02X", cmd_id);
    voice_uart_write_line(buf);
}

static int clamp_int(int value, int min_value, int max_value) {
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

static void send_temp_frame(float temp_c) {
    int temp10 = (int)(temp_c * 10.0f + (temp_c >= 0 ? 0.5f : -0.5f));
    temp10 = clamp_int(temp10, -999, 9999);
    char buf[24];
    snprintf(buf, sizeof(buf), "QT%+04d", temp10);
    voice_uart_write_line(buf);
}

static void send_ph_frame(float ph) {
    int ph10 = (int)(ph * 10.0f + 0.5f);
    ph10 = clamp_int(ph10, 0, 999);
    char buf[24];
    snprintf(buf, sizeof(buf), "QP%03d", ph10);
    voice_uart_write_line(buf);
}

static void send_tds_frame(float tds) {
    int tds_i = (int)(tds + 0.5f);
    tds_i = clamp_int(tds_i, 0, 9999);
    char buf[24];
    snprintf(buf, sizeof(buf), "QD%04d", tds_i);
    voice_uart_write_line(buf);
}

static void send_status_frame(float temp_c, float ph, float tds, float level, float light, float turbidity_ntu) {
    int temp10 = clamp_int((int)(temp_c * 10.0f + (temp_c >= 0 ? 0.5f : -0.5f)), -999, 9999);
    int ph10 = clamp_int((int)(ph * 10.0f + 0.5f), 0, 999);
    int tds_i = clamp_int((int)(tds + 0.5f), 0, 9999);
    int level_i = clamp_int((int)(level + 0.5f), 0, 100);
    int light_i = clamp_int((int)(light + 0.5f), 0, 100);
    int tur_i = clamp_int((int)(turbidity_ntu + 0.5f), 0, 9999);

    char buf[64];
    snprintf(buf, sizeof(buf), "QS%+04d%03d%04d%03d%03d%04d",
             temp10, ph10, tds_i, level_i, light_i, tur_i);
    voice_uart_write_line(buf);
}

uint8_t voice_control_match_command(const char *line) {
    if (!line || !line[0]) {
        return 0;
    }

    uint8_t asr_frame_cmd = match_asrpro_hex_frame(line);
    if (asr_frame_cmd != 0 && asr_frame_cmd != 0xFF) {
        return asr_frame_cmd;
    }

    uint8_t cmd = match_binary_command(line);
    if (cmd != 0) {
        return cmd;
    }

    if (parse_prefixed_number(line, &cmd) && is_supported_cmd(cmd)) {
        return cmd;
    }

    char normalized[64] = {0};
    normalize_ascii_command(line, normalized, sizeof(normalized));

    for (size_t i = 0; i < sizeof(s_ascii_token_map) / sizeof(s_ascii_token_map[0]); i++) {
        if (strcmp(normalized, s_ascii_token_map[i].token) == 0) {
            return s_ascii_token_map[i].cmd;
        }
    }

    return 0;
}

static void voice_receiver_callback(const char *line, void *user_ctx) {
    (void)user_ctx;

    if (is_wake_token(line) || match_asrpro_hex_frame(line) == 0xFF) {
        ESP_LOGI(TAG, "Wake token detected: %s", line);
        voice_uart_write_line("WAKEACK");
        return;
    }

    uint8_t cmd = voice_control_match_command(line);
    if (cmd == 0) {
        ESP_LOGI(TAG, "Unmapped voice input: %s", line);
        voice_uart_write_line("ERR00");
        return;
    }

    ESP_LOGI(TAG, "Voice input: %s -> 0x%02X", line, cmd);
    esp_err_t ret = voice_control_execute_command(cmd, line);
    if (ret == ESP_OK) {
        if (!is_query_cmd(cmd)) {
            send_ack(cmd);
        }
    } else {
        ESP_LOGW(TAG, "Voice command 0x%02X failed: %s", cmd, esp_err_to_name(ret));
        send_error(cmd);
    }
}

esp_err_t voice_control_init(void) {
    (void)adc_manager_init();
    (void)ph_sensor_init();
    (void)tds_sensor_init();
    (void)light_sensor_init();
    (void)turbidity_init();
    (void)water_level_init();
    return ESP_OK;
}

esp_err_t voice_control_execute_command(uint8_t cmd_id, const char *raw_text) {
    (void)raw_text;

    switch (cmd_id) {
        case VOICE_CMD_LIGHT_ON:
            return light_actuator_set_brightness(100.0f);
        case VOICE_CMD_LIGHT_OFF:
            return light_actuator_set_brightness(0.0f);
        case VOICE_CMD_PUMP_ON:
            return pump_set_state(true);
        case VOICE_CMD_PUMP_OFF:
            return pump_set_state(false);
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
            float temp_c = 0.0f;
            esp_err_t ret = ds18b20_read_temperature_c(&temp_c);
            if (ret == ESP_OK) {
                send_temp_frame(temp_c);
            } else {
                voice_uart_write_line("ERTMP");
            }
            return ret;
        }
        case VOICE_CMD_QUERY_PH: {
            float temp_c = 25.0f;
            float ph = 0.0f;
            (void)ds18b20_read_temperature_c(&temp_c);
            esp_err_t ret = ph_sensor_read(temp_c, &ph);
            if (ret == ESP_OK) {
                send_ph_frame(ph);
            } else {
                voice_uart_write_line("ERPH0");
            }
            return ret;
        }
        case VOICE_CMD_QUERY_TDS: {
            float temp_c = 25.0f;
            float tds = 0.0f;
            (void)ds18b20_read_temperature_c(&temp_c);
            esp_err_t ret = tds_sensor_read(temp_c, &tds);
            if (ret == ESP_OK) {
                send_tds_frame(tds);
            } else {
                voice_uart_write_line("ERTDS");
            }
            return ret;
        }
        case VOICE_CMD_QUERY_ALL: {
            float temp_c = 25.0f;
            float ph = 0.0f;
            float tds = 0.0f;
            float level = 0.0f;
            float light = 0.0f;
            float turbidity_ntu = 0.0f;

            (void)ds18b20_read_temperature_c(&temp_c);
            (void)ph_sensor_read(temp_c, &ph);
            (void)tds_sensor_read(temp_c, &tds);
            (void)water_level_read(&level);
            (void)light_sensor_read(&light);
            (void)turbidity_read_compensated(temp_c, &turbidity_ntu);

            send_status_frame(temp_c, ph, tds, level, light, turbidity_ntu);
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
