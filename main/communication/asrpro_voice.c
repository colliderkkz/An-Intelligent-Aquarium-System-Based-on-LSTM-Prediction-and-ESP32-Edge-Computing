#include "asrpro_voice.h"

#include "communication/voice_protocol.h"
#include "config/pin_config.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "ASRPRO_VOICE";

typedef struct {
    const char *phrase;
    uint8_t cmd;
} asr_phrase_map_t;

static asrpro_voice_cmd_handler_t s_handler = NULL;

static const asr_phrase_map_t s_phrase_map[] = {
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

static uint8_t match_phrase_to_cmd(const char *line) {
    if (!line || !line[0]) {
        return 0;
    }

    const bool mentions_uv = (strstr(line, "紫外") != NULL) || (strstr(line, "UV") != NULL);
    if (mentions_uv && strstr(line, "灯") != NULL) {
        if (strstr(line, "打开") != NULL) {
            return VOICE_CMD_UV_ON;
        }
        if (strstr(line, "关闭") != NULL) {
            return VOICE_CMD_UV_OFF;
        }
    }

    for (size_t i = 0; i < sizeof(s_phrase_map) / sizeof(s_phrase_map[0]); i++) {
        if (strcmp(line, s_phrase_map[i].phrase) == 0) {
            return s_phrase_map[i].cmd;
        }
    }

    return 0;
}

static void rstrip_newline(char *s) {
    if (!s) {
        return;
    }

    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t')) {
        s[n - 1] = '\0';
        n--;
    }
}

static void asrpro_voice_task(void *pvParameters) {
    (void)pvParameters;

    uint8_t rx[128];
    char line[128];
    size_t line_len = 0;

    while (1) {
        int n = uart_read_bytes(VOICE_UART_NUM, rx, sizeof(rx), pdMS_TO_TICKS(200));
        if (n <= 0) {
            continue;
        }

        for (int i = 0; i < n; i++) {
            char c = (char)rx[i];
            if (c == '\n') {
                line[line_len] = '\0';
                rstrip_newline(line);
                if (line[0] != '\0') {
                    uint8_t cmd = match_phrase_to_cmd(line);
                    if (cmd != 0 && s_handler) {
                        (void)s_handler(cmd, line);
                    }
                }
                line_len = 0;
            } else if (c != '\r') {
                if (line_len < sizeof(line) - 1) {
                    line[line_len++] = c;
                } else {
                    line_len = 0;
                }
            }
        }
    }
}

esp_err_t asrpro_voice_start(asrpro_voice_cmd_handler_t handler) {
    s_handler = handler;

    uart_config_t cfg = {
        .baud_rate = VOICE_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t ret = uart_driver_install(VOICE_UART_NUM, VOICE_UART_BUF_SIZE, 0, 0, NULL, 0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = uart_param_config(VOICE_UART_NUM, &cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = uart_set_pin(VOICE_UART_NUM, VOICE_UART_TX_PIN, VOICE_UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(ret));
        return ret;
    }

    BaseType_t ok = xTaskCreate(asrpro_voice_task, "asrpro_voice", 4096, NULL, 5, NULL);
    return ok == pdPASS ? ESP_OK : ESP_FAIL;
}
