#include "communication/uart_receiver.h"

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "UART_RX";

typedef struct {
    uart_receiver_config_t cfg;
} uart_receiver_ctx_t;

static void rstrip_line(char *line) {
    if (!line) {
        return;
    }

    size_t len = strlen(line);
    while (len > 0) {
        char c = line[len - 1];
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t') {
            line[len - 1] = '\0';
            len--;
        } else {
            break;
        }
    }
}

static void uart_receiver_task(void *pvParameters) {
    uart_receiver_ctx_t *ctx = (uart_receiver_ctx_t *)pvParameters;
    uint8_t *rx_buf = (uint8_t *)malloc((size_t)ctx->cfg.rx_buffer_size);
    char *line_buf = (char *)malloc(ctx->cfg.line_buffer_size);
    size_t line_len = 0;

    if (!rx_buf || !line_buf) {
        ESP_LOGE(TAG, "Failed to allocate UART receiver buffers");
        free(rx_buf);
        free(line_buf);
        free(ctx);
        vTaskDelete(NULL);
        return;
    }

    /* 空闲超时：有些模块（如 ASRPRO）发的是裸字节、不带换行，
     * 如果只认 '\n' 就永远收不到。这里超过 80ms 没有新数据就把
     * 已缓冲的内容当作一帧交出去。 */
    int64_t last_rx_us = esp_timer_get_time();

    while (1) {
        int n = uart_read_bytes(ctx->cfg.uart_num, rx_buf, (uint32_t)ctx->cfg.rx_buffer_size, pdMS_TO_TICKS(200));
        if (n <= 0) {
            if (line_len > 0 && (esp_timer_get_time() - last_rx_us) > 80000) {
                line_buf[line_len] = '\0';
                rstrip_line(line_buf);
                if (line_buf[0] != '\0' && ctx->cfg.callback) {
                    ctx->cfg.callback(line_buf, ctx->cfg.user_ctx);
                }
                line_len = 0;
            }
            continue;
        }
        last_rx_us = esp_timer_get_time();

        for (int i = 0; i < n; i++) {
            char c = (char)rx_buf[i];
            if (c == '\n') {
                line_buf[line_len] = '\0';
                rstrip_line(line_buf);
                if (line_buf[0] != '\0' && ctx->cfg.callback) {
                    ctx->cfg.callback(line_buf, ctx->cfg.user_ctx);
                }
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
