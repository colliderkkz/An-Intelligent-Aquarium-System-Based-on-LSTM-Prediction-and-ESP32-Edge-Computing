#include "communication/uart_receiver.h"

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "UART_RX";

typedef struct {
    uart_receiver_config_t cfg;
} uart_receiver_ctx_t;

static void rstrip_line(char *line, size_t *len) {
    if (!line || !len) {
        return;
    }

    while (*len > 0) {
        char c = line[*len - 1];
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t') {
            line[*len - 1] = '\0';
            (*len)--;
        } else {
            break;
        }
    }
}

static bool should_emit_as_hex(const uint8_t *buf, size_t len) {
    if (!buf || len == 0) {
        return false;
    }

    for (size_t i = 0; i < len; i++) {
        uint8_t c = buf[i];
        if (c == 0 || (c < 0x20 && c != '\r' && c != '\n' && c != '\t') || c > 0x7E) {
            return true;
        }
    }

    return false;
}

static void emit_callback_line(uart_receiver_ctx_t *ctx, uint8_t *line_buf, size_t line_len) {
    if (!ctx || !line_buf || line_len == 0 || !ctx->cfg.callback) {
        return;
    }

    if (should_emit_as_hex(line_buf, line_len)) {
        size_t out_len = 0;
        char *hex_buf = (char *)malloc(line_len * 3);
        if (!hex_buf) {
            ESP_LOGE(TAG, "Failed to allocate hex line buffer");
            return;
        }

        for (size_t i = 0; i < line_len; i++) {
            int n = snprintf(hex_buf + out_len, line_len * 3 - out_len,
                             (i + 1 < line_len) ? "%02X " : "%02X",
                             line_buf[i]);
            if (n <= 0) {
                free(hex_buf);
                return;
            }
            out_len += (size_t)n;
        }

        ctx->cfg.callback(hex_buf, ctx->cfg.user_ctx);
        free(hex_buf);
        return;
    }

    line_buf[line_len] = '\0';
    char *text_line = (char *)line_buf;
    rstrip_line(text_line, &line_len);
    if (line_len > 0) {
        ctx->cfg.callback(text_line, ctx->cfg.user_ctx);
    }
}

static void uart_receiver_task(void *pvParameters) {
    uart_receiver_ctx_t *ctx = (uart_receiver_ctx_t *)pvParameters;
    uint8_t *rx_buf = (uint8_t *)malloc((size_t)ctx->cfg.rx_buffer_size);
    uint8_t *line_buf = (uint8_t *)malloc(ctx->cfg.line_buffer_size + 1U);
    size_t line_len = 0;

    if (!rx_buf || !line_buf) {
        ESP_LOGE(TAG, "Failed to allocate UART receiver buffers");
        free(rx_buf);
        free(line_buf);
        free(ctx);
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        int n = uart_read_bytes(ctx->cfg.uart_num, rx_buf, (uint32_t)ctx->cfg.rx_buffer_size, pdMS_TO_TICKS(200));
        if (n <= 0) {
            if (line_len > 0) {
                emit_callback_line(ctx, line_buf, line_len);
                line_len = 0;
            }
            continue;
        }

        for (int i = 0; i < n; i++) {
            uint8_t c = rx_buf[i];
            if (c == '\n') {
                emit_callback_line(ctx, line_buf, line_len);
                line_len = 0;
            } else if (c != '\r') {
                if (line_len < ctx->cfg.line_buffer_size - 1) {
                    line_buf[line_len++] = c;
                } else {
                    line_len = 0;
                }
            }
        }
    }
}

esp_err_t uart_receiver_start(const uart_receiver_config_t *config) {
    if (!config || !config->callback || config->line_buffer_size < 8 || config->rx_buffer_size < 16) {
        return ESP_ERR_INVALID_ARG;
    }

    uart_config_t uart_cfg = {
        .baud_rate = config->baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t ret = uart_driver_install(config->uart_num, config->rx_buffer_size, 0, 0, NULL, 0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    ret = uart_param_config(config->uart_num, &uart_cfg);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = uart_set_pin(config->uart_num, config->tx_pin, config->rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) {
        return ret;
    }

    uart_receiver_ctx_t *ctx = (uart_receiver_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx) {
        return ESP_ERR_NO_MEM;
    }
    ctx->cfg = *config;

    BaseType_t ok = xTaskCreate(
        uart_receiver_task,
        config->task_name ? config->task_name : "uart_receiver",
        (uint32_t)(config->task_stack_size > 0 ? config->task_stack_size : 4096),
        ctx,
        (UBaseType_t)(config->task_priority > 0 ? config->task_priority : 5),
        NULL);

    if (ok != pdPASS) {
        free(ctx);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "UART receiver started on UART%d", config->uart_num);
    return ESP_OK;
}
