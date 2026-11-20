#include "act_hold.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "ACT_HOLD";

#define ACT_N   7

static const char *const s_names[ACT_N] = {
    "light", "air", "pump", "uv", "co2", "heater", "refill"
};

static TickType_t s_until[ACT_N];

static int find(const char *name) {
    if (!name || !name[0]) {
        return -1;
    }
    for (int i = 0; i < ACT_N; i++) {
        if (strcmp(s_names[i], name) == 0) {
            return i;
        }
    }
    return -1;
}

void act_hold_set(const char *name, uint32_t seconds) {
    int i = find(name);
    if (i < 0 || seconds == 0) {
        return;
    }
    s_until[i] = xTaskGetTickCount() + pdMS_TO_TICKS(seconds * 1000u);
    ESP_LOGI(TAG, "%s hold %us", s_names[i], (unsigned)seconds);
}

bool act_hold_active(const char *name) {
    int i = find(name);
    if (i < 0) {
        return false;
    }
    return (int32_t)(xTaskGetTickCount() - s_until[i]) < 0;
}

uint32_t act_hold_left_s(const char *name) {
    int i = find(name);
    if (i < 0 || !act_hold_active(name)) {
        return 0;
    }
    TickType_t left = s_until[i] - xTaskGetTickCount();
    return (uint32_t)((left * portTICK_PERIOD_MS) / 1000u);
}

uint32_t act_hold_max_left_s(void) {
    uint32_t mx = 0;
    for (int i = 0; i < ACT_N; i++) {
        uint32_t v = act_hold_left_s(s_names[i]);
        if (v > mx) {
            mx = v;
        }
    }
    return mx;
}

void act_hold_names(char *buf, uint32_t len) {
    if (!buf || len == 0) {
        return;
    }
    buf[0] = '\0';
    uint32_t off = 0;
    for (int i = 0; i < ACT_N; i++) {
        if (!act_hold_active(s_names[i])) {
            continue;
        }
        int n = snprintf(buf + off, len - off, "%s%s", off ? "," : "", s_names[i]);
        if (n < 0 || (uint32_t)(off + (uint32_t)n) >= len - 1u) {
            break;
        }
        off += (uint32_t)n;
    }
}

void act_hold_clear_all(void) {
    for (int i = 0; i < ACT_N; i++) {
        s_until[i] = 0;
    }
}
