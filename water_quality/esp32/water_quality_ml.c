/**
 * @file water_quality_ml.c
 * @brief 水质 1D-CNN 推理服务实现
 *
 * 设计要点：
 *  - 采样与推理都在本模块自己的任务里做（栈 16KB：wq_cnn_predict 的中间激活
 *    约 11KB 全部在栈上，复用控制/通信任务会直接栈溢出）；
 *  - 只有 4 路探头都新鲜时才会往窗口里塞样本，缺数据就是缺数据，
 *    绝不用旧值/默认值凑一个"看起来正常"的结论；
 *  - 结果快照用 portMUX 保护，MQTT/串口读侧无锁、不阻塞；
 *  - ML,FEED 可以手工灌样本，用于探头坏了的情况下验证整条链路。
 */
#include "ml/water_quality_ml.h"

#include "config/error_codes.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ml/wq_cnn.h"
#include "service/sensor_hub.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "WQ_ML";

/* ---- 可调参数 ---- */
/* 采样节拍：24 × step = 窗口时长。默认 2 min → 48 min 窗口。
 * 训练集是 24 × 30 min（12 h），节拍不一致是模型跨到实机后最大的
 * 域偏移来源，现场可用 ML,STEP,<ms> 调大（例如 1800000 = 30 min）。 */
#define WQ_STEP_DEFAULT_MS   120000u
#define WQ_STEP_MIN_MS       10000u
#define WQ_STEP_MAX_MS       3600000u
/* 周期推理间隔 */
#define WQ_INFER_PERIOD_MS   60000u
/* 单路样本的"新鲜"判据：传感器采样周期是 1~2s，15s 已经足够宽松 */
#define WQ_SENSOR_FRESH_MS   15000u
/* 连续多少个节拍没有新样本就认为数据断了 */
#define WQ_STALE_STEPS       3u

#define WQ_TASK_STACK        16384
#define WQ_TASK_PRIORITY     2

typedef struct {
    wq_result_t r;
    uint32_t seq;  /* 每次推理尝试 +1，供 ML,PREDICT 等待 */
} wq_shared_t;

static wq_shared_t s_shared;
static portMUX_TYPE s_shared_mux = portMUX_INITIALIZER_UNLOCKED;

/* 滑动窗口环形缓冲：s_win[s_head] 是下一次写入位置（满时也是最旧样本） */
static float s_win[WQ_WINDOW][4];
static int s_head = 0;
static int s_count = 0;
static uint32_t s_step_ms = WQ_STEP_DEFAULT_MS;
static uint32_t s_last_push_ms = 0;  /* 写侧持 s_lock；读侧只做 32 位原子读 */
static uint32_t s_last_infer_ms = 0;
static bool s_has_pushed = false;
static bool s_started = false;
static bool s_enabled = true;
/* 最近一次有效值，仅用于 ML,STATUS 里展示"探头现在读到多少" */
static float s_feat_last[4] = {0.0f, 7.0f, 0.0f, 0.0f};

static SemaphoreHandle_t s_lock = NULL;
static TaskHandle_t s_task = NULL;

static const char *const s_class_name[WQ_CLASS_COUNT] = {"Normal", "Caution", "Warning", "Severe"};
static const char *const s_class_label[WQ_CLASS_COUNT] = {"正常", "注意", "警告", "严重"};

/* 模型 4 通道 ↔ sensor_hub 通道，顺序必须与 wq_cnn.h 一致 */
static const sensor_id_t s_win_ids[4] = {SH_TEMP, SH_PH, SH_TDS, SH_TURBIDITY};

static inline uint32_t now_ms(void) {
    return (uint32_t)((uint64_t)esp_timer_get_time() / 1000u);
}

/* ------------------------------------------------------------------ */
/* 快照                                                                */
/* ------------------------------------------------------------------ */

static void shared_read(wq_shared_t *out) {
    taskENTER_CRITICAL(&s_shared_mux);
    *out = s_shared;
    taskEXIT_CRITICAL(&s_shared_mux);
}

static void shared_publish(const wq_result_t *r) {
    taskENTER_CRITICAL(&s_shared_mux);
    s_shared.r = *r;
    s_shared.seq++;
    taskEXIT_CRITICAL(&s_shared_mux);
}

/* 窗口填充进度要能实时反映在 ML,STATUS / ML,FEED 的回复里，
 * 否则灌了 10 帧还显示 n=0（下一次推理才会更新）。 */
static void shared_set_progress(int n) {
    taskENTER_CRITICAL(&s_shared_mux);
    s_shared.r.samples = (uint8_t)n;
    s_shared.r.window_full = (n >= WQ_WINDOW);
    taskEXIT_CRITICAL(&s_shared_mux);
}

/* 读侧：拷贝快照 + 根据"现在"推导状态（数据断了要能看出来） */
static void fill_result(wq_result_t *out) {
    wq_shared_t snap;
    shared_read(&snap);

    *out = snap.r;
    out->enabled = s_enabled;
    out->step_s = (float)s_step_ms / 1000.0f;
    out->window_span_s = (out->samples > 1) ? (float)(out->samples - 1) * out->step_s : 0.0f;

    if (!s_started) {
        snprintf(out->status, sizeof(out->status), "INIT");
        return;
    }
    if (!s_enabled) {
        snprintf(out->status, sizeof(out->status), "OFF");
        return;
    }
    if (out->samples < WQ_WINDOW) {
        snprintf(out->status, sizeof(out->status), "WARMUP");
        return;
    }
    if (s_has_pushed) {
        const uint32_t age = now_ms() - s_last_push_ms;
        if (age > WQ_STALE_STEPS * s_step_ms) {
            snprintf(out->status, sizeof(out->status), "STALE");
            return;
        }
    }
    if (!out->ok) {
        snprintf(out->status, sizeof(out->status), "NO_DATA");
        return;
    }
    snprintf(out->status, sizeof(out->status), "OK");
}

/* ------------------------------------------------------------------ */
/* 窗口                                                                */
/* ------------------------------------------------------------------ */

/* 调用方必须持有 s_lock */
static void push_locked(const float v[4]) {
    memcpy(s_win[s_head], v, sizeof(s_win[s_head]));
    s_head = (s_head + 1) % WQ_WINDOW;
    if (s_count < WQ_WINDOW) {
        s_count++;
    }
    s_has_pushed = true;
    s_last_push_ms = now_ms();
    memcpy(s_feat_last, v, sizeof(s_feat_last));
}

/* 调用方必须持有 s_lock；把环形缓冲按"最旧 → 最新"摊平 */
static void linearize_locked(float out[WQ_WINDOW][4]) {
    for (int i = 0; i < WQ_WINDOW; i++) {
        const int idx = (s_head + i) % WQ_WINDOW;  /* 满窗口时 s_head 即最旧 */
        memcpy(out[i], s_win[idx], sizeof(out[i]));
    }
}

/* ------------------------------------------------------------------ */
/* 采样                                                                */
/* ------------------------------------------------------------------ */

static bool sample_is_usable(sensor_id_t id, float *value) {
    const sensor_sample_t *s = sensor_hub_get(id);
    if (!s || s->err != ESP_OK || !s->valid) {
        return false;
    }
    if (!sensor_hub_is_fresh(id, WQ_SENSOR_FRESH_MS)) {
        return false;
    }
    *value = s->value;
    return true;
}

/* 4 路都新鲜才入窗；返回 true 表示这一拍采到了完整样本 */
static bool push_from_sensors(void) {
    const sensor_id_t ids[4] = {SH_TEMP, SH_PH, SH_TDS, SH_TURBIDITY};
    float v[4];

    for (int i = 0; i < 4; i++) {
        if (!sample_is_usable(ids[i], &v[i])) {
            return false;
        }
    }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        return false;
    }
    push_locked(v);
    const int n = s_count;
    xSemaphoreGive(s_lock);
    shared_set_progress(n);
    return true;
}

/* 更新"展示用"的最近读数（窗口不因缺数据而推进） */
static void refresh_feat(void) {
    float v;

    for (int i = 0; i < 4; i++) {
        const sensor_sample_t *s = sensor_hub_get(s_win_ids[i]);
        const bool ok = (s && s->err == ESP_OK && s->valid);
        if (sample_is_usable(s_win_ids[i], &v)) {
            s_feat_last[i] = v;
        }
        taskENTER_CRITICAL(&s_shared_mux);
        s_shared.r.feat_valid[i] = ok;
        s_shared.r.feat[i] = s_feat_last[i];
        taskEXIT_CRITICAL(&s_shared_mux);
    }
}

/* ------------------------------------------------------------------ */
/* 推理                                                                */
/* ------------------------------------------------------------------ */

static void run_inference(uint32_t now) {
    float win[WQ_WINDOW][4];
    int n;

    memset(win, 0, sizeof(win));

    xSemaphoreTake(s_lock, portMAX_DELAY);
    n = s_count;
    if (n == WQ_WINDOW) {
        linearize_locked(win);
    }
    xSemaphoreGive(s_lock);

    wq_result_t r;
    memset(&r, 0, sizeof(r));
    r.cls = -1;
    r.samples = (uint8_t)n;
    r.window_full = (n == WQ_WINDOW);
    r.window_span_s = (float)((n > 1) ? (n - 1) * s_step_ms : 0) / 1000.0f;
    r.updated_ms = now;
    for (int i = 0; i < 4; i++) {
        const sensor_sample_t *s = sensor_hub_get(s_win_ids[i]);
        r.feat_valid[i] = (s && s->err == ESP_OK && s->valid);
        r.feat[i] = s_feat_last[i];
    }

    if (n < WQ_WINDOW) {
        /* 窗口没满：不硬跑模型，明确告诉调用方还差多少 */
        snprintf(r.status, sizeof(r.status), "WARMUP");
        shared_publish(&r);
        s_last_infer_ms = now;
        return;
    }

    const int64_t t0 = esp_timer_get_time();
    const int cls = wq_cnn_predict(win, r.prob);
    const int64_t dt = esp_timer_get_time() - t0;

    r.ok = true;
    r.cls = cls;
    r.infer_us = (uint32_t)dt;
    snprintf(r.status, sizeof(r.status), "OK");
    shared_publish(&r);
    s_last_infer_ms = now;

    ESP_LOGI(TAG, "wq=%d(%s) p=%.2f/%.2f/%.2f/%.2f samples=%d span=%.0fs infer=%lldus",
             cls, s_class_name[cls], r.prob[0], r.prob[1], r.prob[2], r.prob[3], n,
             (double)r.window_span_s, (long long)dt);
}

/* ------------------------------------------------------------------ */
/* 任务                                                                */
/* ------------------------------------------------------------------ */

static void wq_task(void *arg) {
    (void)arg;

    for (;;) {
        const uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        const uint32_t now = now_ms();

        refresh_feat();

        const bool due = (!s_has_pushed) || ((uint32_t)(now - s_last_push_ms) >= s_step_ms);
        if (due) {
            (void)push_from_sensors();
        }

        if (notified > 0) {
            run_inference(now);
        } else if (s_enabled && (uint32_t)(now - s_last_infer_ms) >= WQ_INFER_PERIOD_MS) {
            run_inference(now);
        }
    }
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */

esp_err_t water_quality_init(void) {
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    memset(s_win, 0, sizeof(s_win));
    s_head = 0;
    s_count = 0;
    s_step_ms = WQ_STEP_DEFAULT_MS;
    s_has_pushed = false;
    s_enabled = true;
    s_last_infer_ms = 0;

    memset(&s_shared, 0, sizeof(s_shared));
    s_shared.r.cls = -1;
    snprintf(s_shared.r.status, sizeof(s_shared.r.status), "INIT");
    s_shared.r.step_s = (float)s_step_ms / 1000.0f;

    ESP_LOGI(TAG, "model ready: %d ch x %d steps, step=%lus, classes=%s/%s/%s/%s",
             WQ_IN_CHANNELS, WQ_WINDOW, (unsigned long)(s_step_ms / 1000u),
             s_class_name[0], s_class_name[1], s_class_name[2], s_class_name[3]);
    return ESP_OK;
}

esp_err_t water_quality_start(void) {
    if (s_lock == NULL) {
        return APP_ERR_INVALID_STATE;
    }
    if (s_started) {
        return ESP_OK;
    }
    s_started = true;

    BaseType_t ok = xTaskCreate(wq_task, "wq_ml", WQ_TASK_STACK, NULL, WQ_TASK_PRIORITY, &s_task);
    if (ok != pdPASS) {
        s_started = false;
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool water_quality_get(wq_result_t *out) {
    if (!out) {
        return false;
    }
    fill_result(out);
    return s_started;
}

esp_err_t water_quality_predict(wq_result_t *out, uint32_t timeout_ms) {
    if (!s_started || !s_task) {
        return APP_ERR_INVALID_STATE;
    }

    wq_shared_t snap;
    shared_read(&snap);
    const uint32_t seq0 = snap.seq;

    xTaskNotifyGive(s_task);

    const uint32_t deadline = now_ms() + timeout_ms;
    for (;;) {
        shared_read(&snap);
        if (snap.seq != seq0) {
            break;
        }
        if ((int32_t)(now_ms() - deadline) >= 0) {
            fill_result(out);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    fill_result(out);
    return ESP_OK;
}

esp_err_t water_quality_feed(float temp_c, float ph, float tds_ppm, float turb_ntu) {
    if (s_lock == NULL) {
        return APP_ERR_INVALID_STATE;
    }
    /* 工程量范围粗检：明显是标定错误或单位不对的样本不该进模型 */
    if (temp_c < -5.0f || temp_c > 45.0f || ph < 0.0f || ph > 14.0f ||
        tds_ppm < 0.0f || tds_ppm > 5000.0f || turb_ntu < 0.0f || turb_ntu > 3000.0f) {
        return ESP_ERR_INVALID_ARG;
    }

    const float v[4] = {temp_c, ph, tds_ppm, turb_ntu};
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    push_locked(v);
    const int n = s_count;
    xSemaphoreGive(s_lock);
    shared_set_progress(n);
    return ESP_OK;
}

void water_quality_reset(void) {
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(s_win, 0, sizeof(s_win));
    s_head = 0;
    s_count = 0;
    s_has_pushed = false;
    xSemaphoreGive(s_lock);

    wq_result_t r;
    memset(&r, 0, sizeof(r));
    r.cls = -1;
    snprintf(r.status, sizeof(r.status), "WARMUP");
    shared_publish(&r);
    ESP_LOGI(TAG, "window cleared");
}

void water_quality_set_step_ms(uint32_t step_ms) {
    if (step_ms < WQ_STEP_MIN_MS) {
        step_ms = WQ_STEP_MIN_MS;
    } else if (step_ms > WQ_STEP_MAX_MS) {
        step_ms = WQ_STEP_MAX_MS;
    }
    s_step_ms = step_ms;
    ESP_LOGI(TAG, "step -> %lus (window = %lus)", (unsigned long)(step_ms / 1000u),
             (unsigned long)((step_ms / 1000u) * WQ_WINDOW));
}

uint32_t water_quality_step_ms(void) {
    return s_step_ms;
}

void water_quality_set_enabled(bool enabled) {
    s_enabled = enabled;
    ESP_LOGI(TAG, "inference %s", enabled ? "enabled" : "disabled");
}

bool water_quality_enabled(void) {
    return s_enabled;
}

const char *water_quality_class_name(int cls) {
    if (cls < 0 || cls >= WQ_CLASS_COUNT) {
        return "?";
    }
    return s_class_name[cls];
}

const char *water_quality_class_label(int cls) {
    if (cls < 0 || cls >= WQ_CLASS_COUNT) {
        return "?";
    }
    return s_class_label[cls];
}
