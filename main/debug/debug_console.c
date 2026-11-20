#include "debug/debug_console.h"

#include "config/error_codes.h"
#include "config/pin_config.h"
#include "config/system_config.h"
#include "debug/debug_commands.h"
#include "debug/debug_protocol.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "rom/ets_sys.h"
#include "service/sd_logger.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "DEBUG_CON";

static bool s_running = false;
static SemaphoreHandle_t s_tx_lock = NULL;

/* ------------------------------------------------------------------ */
/* 发送                                                                */
/* ------------------------------------------------------------------ */

static void uart_send_raw(const char *text, size_t len) {
    if (!text || len == 0) {
        return;
    }
    if (s_tx_lock) {
        (void)xSemaphoreTake(s_tx_lock, pdMS_TO_TICKS(500));
    }
    (void)uart_write_bytes(DEBUG_UART_NUM, text, len);
    if (s_tx_lock) {
        xSemaphoreGive(s_tx_lock);
    }
}

/*
 * 让 ESP_LOG 也走 UART 驱动发送。
 *
 * 不这么做的话：日志由 ROM 的 printf 直接怼 UART 寄存器，而协议帧走
 * uart_write_bytes（驱动），两条路径互不知情 —— 日志还没写完，#READY 就插进来了，
 * 结果是 `I ��#READY,fw=...` 这种被污染的行，上位机只能整行丢弃。
 *
 * 更麻烦的是 ESP-IDF 会把一条日志拆成多次 vprintf 调用（先输出
 * "I (123) TAG: " 前缀，再输出正文），所以每次调用单独加锁也不够 ——
 * 前缀发出后照样会被抢占。这里按行缓冲，攒到 '\n' 才整行一次性写出。
 */
#define DEBUG_LOG_LINE_MAX 384
static SemaphoreHandle_t s_log_lock = NULL;
static char s_log_line[DEBUG_LOG_LINE_MAX];
static size_t s_log_len = 0;

static int debug_log_vprintf(const char *fmt, va_list ap) {
    char buf[256];
    va_list copy;
    va_copy(copy, ap);
    int n = vsnprintf(buf, sizeof(buf), fmt, copy);
    va_end(copy);
    if (n <= 0) {
        return n;
    }

    if (xPortInIsrContext() || !s_log_lock) {
        ets_printf("%s", buf);
        return n;
    }

    const size_t len = ((size_t)n < sizeof(buf)) ? (size_t)n : (sizeof(buf) - 1);
    if (xSemaphoreTake(s_log_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        /* 拿不到锁就丢弃这行日志。
         * BUGFIX: 之前这里 ets_printf 直写 UART，会跳过行缓冲容易和
         * 正在发送的半行日志交叉，导致 #READY 变成 "I (600) DEBU??#READY"。
         * 宁可丢一行日志，也不能弄脏协议帧。 */
        return n;
    }
    for (size_t i = 0; i < len; ++i) {
        if (s_log_len < sizeof(s_log_line) - 1) {
            s_log_line[s_log_len++] = buf[i];
        }
        const bool flush = (buf[i] == '\n');
        if (flush || s_log_len >= sizeof(s_log_line) - 1) {
            if (s_log_len > 0) {
                uart_send_raw(s_log_line, s_log_len);       /* 内部拿 TX 锁 */
                sd_logger_push_log(s_log_line, s_log_len);  /* 同时落盘到 SD 卡 */
                s_log_len = 0;
            }
        }
    }
    xSemaphoreGive(s_log_lock);
    return n;
}

/* 把日志里还没凑成整行的半截先冲出去，保证接下来的协议帧独占一行 */
static void log_flush_partial_locked(void) {
    if (s_log_len > 0) {
        uart_send_raw(s_log_line, s_log_len);
        s_log_len = 0;
    }
}

/* dbg_io_t.send_line 的实现：line 已带 '#'，这里补 CRLF */
/*
 * 发送一整个协议帧。
 * 两个保证：
 *   1. 前面先补 CRLF —— 即使日志刚好写到一半，本帧也能独立成行，
 *      上位机不会因为行首粘了 "I " 前缀而整帧丢弃；
 *   2. 与日志共用 s_log_lock，先把半行日志冲掉再发帧，两者不会交错。
 */
static void send_frame(const char *line) {
    if (!line) {
        return;
    }
    char out[DEBUG_REPLY_MAX + 8];
    int n = snprintf(out, sizeof(out), "\r\n%s\r\n", line);
    if (n < 0) {
        return;
    }
    if ((size_t)n >= sizeof(out)) {
        n = (int)sizeof(out) - 1;
    }
    const size_t len = (size_t)n;

    if (s_log_lock && !xPortInIsrContext()) {
        if (xSemaphoreTake(s_log_lock, pdMS_TO_TICKS(500)) == pdTRUE) {
            log_flush_partial_locked();
            uart_send_raw(out, len);
            xSemaphoreGive(s_log_lock);
            return;
        }
    }
    uart_send_raw(out, len);
}

static void console_send_line(void *ctx, const char *line) {
    (void)ctx;
    send_frame(line);
}

static void console_send_err(void *ctx, const char *code, const char *msg) {
    (void)ctx;
    char out[128];
    int n = snprintf(out, sizeof(out), "!%s,%s", code ? code : "ERR", msg ? msg : "");
    if (n < 0) {
        return;
    }
    if ((size_t)n >= sizeof(out)) {
        n = (int)sizeof(out) - 1;
    }
    send_frame(out);
}

void debug_console_notify(const char *fmt, ...) {
    if (!fmt) {
        return;
    }
    char body[DEBUG_REPLY_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    char line[DEBUG_REPLY_MAX + 2];
    int n = snprintf(line, sizeof(line), "#%s", body);
    if (n <= 0) {
        return;
    }
    send_frame(line);
}

/* ------------------------------------------------------------------ */
/* 中断检测                                                            */
/* ------------------------------------------------------------------ */

bool debug_console_abort_requested(void) {
    /* 只"窥视"接收缓冲里有没有字节，绝不把它读走。
     *
     * BUGFIX: 原来用 uart_read_bytes(...,1,0) 把字节消费掉了 ——
     * TEST,ACTUATORS 跑到一半时上位机定时发的 SD,STATUS 会被吃掉首字母，
     * 剩下 "D,STATUS" 变成未知命令；用户敲的 MODE,GET 也只剩 "ODE,GET"。
     * 改成查缓冲长度后，那个字节原封不动留给真正的命令解析器。 */
    size_t pending = 0;
    if (uart_get_buffered_data_len(DEBUG_UART_NUM, &pending) != ESP_OK) {
        return false;
    }
    return pending > 0;
}

/* ------------------------------------------------------------------ */
/* 接收任务                                                            */
/* ------------------------------------------------------------------ */

static void handle_line(const char *line, const dbg_io_t *io) {
    static dbg_fields_t fields;
    char buf[DBG_LINE_MAX];
    size_t len = strlen(line);
    if (len >= sizeof(buf)) {
        len = sizeof(buf) - 1;
    }
    memcpy(buf, line, len);
    buf[len] = '\0';

    dbg_fields_split(buf, &fields);
    if (fields.count == 0 || !fields.field[0] || fields.field[0][0] == '\0') {
        return;
    }

    esp_err_t ret = debug_commands_dispatch(&fields, io);
    if (ret != ESP_OK) {
        /* 处理函数通常已经发过错误行；这里只做日志，避免重复回复 */
        ESP_LOGD(TAG, "command '%s' -> %s", fields.field[0], app_err_name(ret));
    }
}

static void debug_console_task(void *pvParameters) {
    (void)pvParameters;

    static dbg_rx_t rx;
    dbg_rx_reset(&rx);

    const dbg_io_t io = {
        .send_line = console_send_line,
        .send_err = console_send_err,
        .ctx = NULL,
    };

    uint8_t chunk[128];
    uint32_t last_stream_ms = 0;
    char tel[DEBUG_REPLY_MAX];

    debug_commands_set_abort_check(debug_console_abort_requested);

    /* 上电问候：上位机据此判断“设备已就绪”，而不是靠猜 */
    debug_console_notify("READY,fw=%s,ver=%s,proto=%d,board=%s",
                         SYSTEM_NAME, FIRMWARE_VERSION_STRING,
                         DEBUG_PROTOCOL_VERSION, BOARD_NAME);

    while (1) {
        int n = uart_read_bytes(DEBUG_UART_NUM, chunk, sizeof(chunk), pdMS_TO_TICKS(20));
        if (n > 0) {
            for (int i = 0; i < n; i++) {
                dbg_rx_result_t res = dbg_rx_push(&rx, (char)chunk[i]);
                if (res == DBG_RX_LINE) {
                    handle_line(rx.buf, &io);
                } else if (res == DBG_RX_OVERFLOW) {
                    console_send_err(NULL, app_err_name(APP_ERR_OVERFLOW), "line too long");
                }
            }
        }

        if (debug_commands_stream_enabled()) {
            uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
            if ((uint32_t)(now - last_stream_ms) >= debug_commands_stream_period_ms()) {
                last_stream_ms = now;
                debug_commands_build_telemetry(tel, sizeof(tel));
                console_send_line(NULL, tel);
            }
        }
    }
}

esp_err_t debug_console_start(i2c_bus_handle_t bus) {
    if (s_running) {
        return ESP_OK;
    }

    esp_err_t ret = debug_commands_init(bus);
    if (ret != ESP_OK) {
        return ret;
    }

    if (s_tx_lock == NULL) {
        s_tx_lock = xSemaphoreCreateMutex();
        if (s_tx_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_log_lock == NULL) {
        s_log_lock = xSemaphoreCreateMutex();
        if (s_log_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    s_log_len = 0;

    uart_config_t cfg = {
        .baud_rate = DEBUG_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    /* UART0 可能已被 IDF 控制台安装过驱动，INVALID_STATE 视为正常。
     *
     * tx_buffer 必须给：原来传 0，uart_write_bytes 会一直阻塞到 FIFO 发完，
     * 115200 下一行 60 字节约 5ms、一行 300 字节约 26ms —— 谁打日志谁卡住，
     * 包括 MQTT 事件任务（命令回执延迟里就有这一块）。给 4KB 环形缓冲后
     * 写入变成本地 memcpy，实际发送交给 UART 中断。 */
    ret = uart_driver_install(DEBUG_UART_NUM, DEBUG_UART_RX_BUF_SIZE,
                              DEBUG_UART_TX_BUF_SIZE, 0, NULL, 0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "uart%d tx buffer %d (install=%s)", DEBUG_UART_NUM,
             DEBUG_UART_TX_BUF_SIZE, esp_err_to_name(ret));

    ret = uart_param_config(DEBUG_UART_NUM, &cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = uart_set_pin(DEBUG_UART_NUM, DEBUG_UART_TX_PIN, DEBUG_UART_RX_PIN,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "uart_set_pin failed: %s (keep default pins)", esp_err_to_name(ret));
    }

    /* 必须在创建任务之前接管日志输出：任务一启动就会发 #READY，
     * 如果那时日志还走 ROM printf，两者就会交错误码。 */
    esp_log_set_vprintf(debug_log_vprintf);

    BaseType_t ok = xTaskCreate(debug_console_task, "dbg_console", DEBUG_UART_TASK_STACK, NULL,
                                DEBUG_UART_TASK_PRIORITY, NULL);
    if (ok != pdPASS) {
        return ESP_FAIL;
    }

    s_running = true;
    ESP_LOGI(TAG, "debug console ready on UART%d @%d (protocol v%d)",
             (int)DEBUG_UART_NUM, DEBUG_UART_BAUD_RATE, DEBUG_PROTOCOL_VERSION);
    return ESP_OK;
}

bool debug_console_is_running(void) {
    return s_running;
}
