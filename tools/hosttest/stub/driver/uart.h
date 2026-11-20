#ifndef STUB_DRIVER_UART_H
#define STUB_DRIVER_UART_H

#include "esp_err.h"
#include "driver/gpio.h"
#include <stdint.h>
#include <stddef.h>

typedef int uart_port_t;
#define UART_NUM_0 0
#define UART_NUM_1 1
#define UART_NUM_2 2

#define UART_PIN_NO_CHANGE (-1)

typedef enum { UART_DATA_5_BITS = 0, UART_DATA_6_BITS, UART_DATA_7_BITS, UART_DATA_8_BITS } uart_word_length_t;
typedef enum { UART_PARITY_DISABLE = 0, UART_PARITY_EVEN = 2, UART_PARITY_ODD = 3 } uart_parity_t;
typedef enum { UART_STOP_BITS_1 = 1, UART_STOP_BITS_1_5 = 2, UART_STOP_BITS_2 = 3 } uart_stop_bits_t;
typedef enum { UART_HW_FLOWCTRL_DISABLE = 0, UART_HW_FLOWCTRL_RTS = 1, UART_HW_FLOWCTRL_CTS = 2 } uart_hw_flowcontrol_t;
typedef enum { UART_SCLK_DEFAULT = 0, UART_SCLK_APB = 1 } uart_sclk_t;

typedef struct {
    int baud_rate;
    uart_word_length_t data_bits;
    uart_parity_t parity;
    uart_stop_bits_t stop_bits;
    uart_hw_flowcontrol_t flow_ctrl;
    uint8_t rx_flow_ctrl_thresh;
    uart_sclk_t source_clk;
} uart_config_t;

esp_err_t uart_driver_install(uart_port_t uart_num, int rx_buffer_size, int tx_buffer_size,
                              int queue_size, void *uart_queue, int intr_alloc_flags);
esp_err_t uart_param_config(uart_port_t uart_num, const uart_config_t *uart_config);
esp_err_t uart_set_pin(uart_port_t uart_num, int tx_io_num, int rx_io_num, int rts_io_num, int cts_io_num);
int uart_write_bytes(uart_port_t uart_num, const void *src, size_t size);
int uart_read_bytes(uart_port_t uart_num, void *buf, uint32_t length, uint32_t ticks_to_wait);

#endif
