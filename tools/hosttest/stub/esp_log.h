#ifndef STUB_ESP_LOG_H
#define STUB_ESP_LOG_H

#include "esp_err.h"
#include <stdarg.h>

typedef enum {
    ESP_LOG_NONE = 0,
    ESP_LOG_ERROR,
    ESP_LOG_WARN,
    ESP_LOG_INFO,
    ESP_LOG_DEBUG,
    ESP_LOG_VERBOSE,
} esp_log_level_t;

typedef int (*vprintf_like_t)(const char *fmt, va_list args);

void esp_log_level_set(const char *tag, esp_log_level_t level);
void esp_log_write(esp_log_level_t level, const char *tag, const char *fmt, ...);
vprintf_like_t esp_log_set_vprintf(vprintf_like_t func);

#define ESP_LOGE(tag, fmt, ...) esp_log_write(ESP_LOG_ERROR, tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) esp_log_write(ESP_LOG_WARN, tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) esp_log_write(ESP_LOG_INFO, tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) esp_log_write(ESP_LOG_DEBUG, tag, fmt, ##__VA_ARGS__)
#define ESP_LOGV(tag, fmt, ...) esp_log_write(ESP_LOG_VERBOSE, tag, fmt, ##__VA_ARGS__)

#endif
