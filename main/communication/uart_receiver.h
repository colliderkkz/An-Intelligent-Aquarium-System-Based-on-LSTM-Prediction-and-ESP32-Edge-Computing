#ifndef UART_RECEIVER_H
#define UART_RECEIVER_H

#include "driver/uart.h"
#include "esp_err.h"
#include "hal/gpio_types.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*uart_line_callback_t)(const char *line, void *user_ctx);

typedef struct {
    uart_port_t uart_num;
    gpio_num_t tx_pin;
    gpio_num_t rx_pin;
    int baud_rate;
    int rx_buffer_size;
    size_t line_buffer_size;
    uart_line_callback_t callback;
    void *user_ctx;
    const char *task_name;
    int task_stack_size;
    int task_priority;
} uart_receiver_config_t;

esp_err_t uart_receiver_start(const uart_receiver_config_t *config);

#ifdef __cplusplus
}
#endif

#endif // UART_RECEIVER_H
