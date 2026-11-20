#include "sd_logger.h"

#include "config/pin_config.h"
#include "debug/debug_console.h"
#include "drivers/sd_card_spi.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "service/sensor_hub.h"
#include "service/time_service.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "SD_LOG";

#define SD_LOG_QUEUE_LEN   24
#define SD_LOG_LINE_MAX    192
#define SD_TASK_PERIOD_MS  1000
#define SD_RETRY_MS        5000

typedef struct {
    char text[SD_LOG_LINE_MAX];
} sd_log_item_t;

static QueueHandle_t s_queue = NULL;
static TaskHandle_t s_task = NULL;
static volatile bool s_run = false;
static volatile bool s_active = false;
static volatile uint32_t s_rows = 0;
static volatile uint32_t s_dropped = 0;
/* 实时推送：每写入一行就发一帧 #SDLINE 给上位机（默认关，SD,STREAM,ON 打开） */
/* 默认开启：板子写卡的同时把每行同步到 MQTT，NAS 归档和手机端才能持续拿到数据。
 * 上位机/手机都可以用 SD,STREAM / {"cmd":"stream"} 单独关掉。 */
static volatile bool s_stream = true;
/* 行监听者（MQTT 注册），用于把每一行同步到手机/NAS */
static sd_line_cb_t s_line_cb = NULL;
static volatile uint32_t s_streamed_csv = 0;
static volatile uint32_t s_streamed_log = 0;
static uint32_t s_last_mount_try = 0;
/* 写失败标志：拔卡瞬间 fwrite/fflush 会报错，用它触发卸载+重挂载 */
static volatile bool s_write_failed = false;
/* 目录项同步：fclose 才会把文件名/大小写进 FAT 目录项，fflush 不会。
 * 所以每 10 秒关一次再开一次，保证随时拔卡读卡器都能看到完整数据。 */
static uint32_t s_last_sync_ms = 0;
static volatile uint32_t s_data_size = 0;
static volatile uint32_t s_log_size = 0;
#define SD_SYNC_PERIOD_MS 10000u

static FILE *s_data_fp = NULL;
static FILE *s_log_fp = NULL;
static int s_cur_y = -1, s_cur_m = -1, s_cur_d = -1;
static char s_data_path[96];
static char s_log_path[96];

void sd_logger_set_line_cb(sd_line_cb_t cb) {
    s_line_cb = cb;
}

void sd_logger_set_stream(bool on) {
    s_stream = on;
    s_streamed_csv = 0;
    s_streamed_log = 0;
    ESP_LOGI(TAG, "streaming %s", on ? "ON" : "OFF");
}

bool sd_logger_is_streaming(void) {
    return s_stream;
}

uint32_t sd_logger_streamed_csv(void) {
    return s_streamed_csv;
}

uint32_t sd_logger_streamed_log(void) {
    return s_streamed_log;
}

uint32_t sd_logger_data_size(void) {
    return s_data_size;
}

uint32_t sd_logger_log_size(void) {
    return s_log_size;
}

bool sd_logger_is_active(void) {
    return s_active;
}

uint32_t sd_logger_rows(void) {
    return s_rows;
}

uint32_t sd_logger_dropped(void) {
    return s_dropped;
}

const char *sd_logger_data_path(void) {
    return s_data_path;
}

const char *sd_logger_log_path(void) {
    return s_log_path;
}

void sd_logger_push_log(const char *line, size_t len) {
    if (!s_queue || !line || len == 0) {
        return;
    }
    sd_log_item_t item;
    size_t n = len < (sizeof(item.text) - 1) ? len : (sizeof(item.text) - 1);
    memcpy(item.text, line, n);
    /* 去掉尾部换行，落盘时统一加时间前缀 */
    while (n > 0 && (item.text[n - 1] == '\n' || item.text[n - 1] == '\r')) {
        --n;
    }
    if (n == 0) {
        return;
    }
    item.text[n] = '\0';
    if (xQueueSend(s_queue, &item, 0) != pdTRUE) {
        ++s_dropped;
    }
}

/* 进协议帧的内容里有逗号/等号/换行，直接塞会破坏 key=value 解析，
 * 所以统一 base64。这里只要编码，不需要解码。 */
static const char B64_TAB[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t b64_enc(const uint8_t *in, size_t len, char *out, size_t out_len) {
    if (!in || !out || out_len == 0) {
        return 0;
    }
    const size_t need = ((len + 2) / 3) * 4 + 1;
    if (out_len < need) {
        return 0;
    }
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        const uint32_t b0 = in[i];
        const uint32_t b1 = (i + 1 < len) ? in[i + 1] : 0;
        const uint32_t b2 = (i + 2 < len) ? in[i + 2] : 0;
        const uint32_t v = (b0 << 16) | (b1 << 8) | b2;
        out[o++] = B64_TAB[(v >> 18) & 0x3F];
        out[o++] = B64_TAB[(v >> 12) & 0x3F];
        out[o++] = (i + 1 < len) ? B64_TAB[(v >> 6) & 0x3F] : '=';
        out[o++] = (i + 2 < len) ? B64_TAB[v & 0x3F] : '=';
    }
    out[o] = '\0';
    return o;
}

/* 推送一行给上位机 */
static void emit_line(const char *kind, uint32_t seq, const char *text, size_t len) {
    if (!s_stream || !text || len == 0) {
        return;
    }
    /* 去掉末尾换行，避免上位机每行多一个空行 */
    while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r')) {
        --len;
    }
    /* 只有 SD 任务会调用 emit_line，用静态缓冲省 640 字节栈 */
    static char b64[640];
    if (b64_enc((const uint8_t *)text, len, b64, sizeof(b64)) == 0) {
        return;
    }
    debug_console_notify("SDLINE,kind=%s,seq=%lu,text=%s",
                         kind, (unsigned long)seq, b64);

    /* 同时喂给监听者（MQTT -> 手机/NAS） */
    if (s_line_cb) {
        s_line_cb(kind, text, len);
    }
}

static void close_files(void) {
    if (s_data_fp) {
        fflush(s_data_fp);
        fclose(s_data_fp);
        s_data_fp = NULL;
    }
    if (s_log_fp) {
        fflush(s_log_fp);
        fclose(s_log_fp);
        s_log_fp = NULL;
    }
    s_active = false;
    s_cur_y = s_cur_m = s_cur_d = -1;
    s_data_path[0] = '\0';
    s_log_path[0] = '\0';
}

static bool open_files_for(const pcf8563_time_t *now) {
    /* 直接写挂载根目录。
     * 原本想分 /sdcard/data 和 /sdcard/log 两个子目录，但实测这个挂载方式下
     * mkdir() 建目录会失败（fopen 随之失败，SD_LOG 一直 active=0）。
     * 文件名加前缀区分两类内容，既清晰又不用建目录。 */
    /* 长文件名：sdkconfig 里已启用 CONFIG_FATFS_LFN_HEAP=y。
     * 注意：如果沿用旧的 sdkconfig（LNFS=NONE），fopen 会返回 EINVAL(22)
     * 且 SD_LOG 一直 active=0 —— 那种情况下要删掉 sdkconfig 重新全量构建。 */
    snprintf(s_data_path, sizeof(s_data_path), "%s/sensors_%04u-%02u-%02u.csv",
             SD_CARD_MOUNT_POINT,
             (unsigned)now->year, (unsigned)now->month, (unsigned)now->day);
    snprintf(s_log_path, sizeof(s_log_path), "%s/board_%04u-%02u-%02u.log",
             SD_CARD_MOUNT_POINT,
             (unsigned)now->year, (unsigned)now->month, (unsigned)now->day);

    struct stat st;
    const bool data_new = (stat(s_data_path, &st) != 0);

    s_data_fp = fopen(s_data_path, "a");
    if (!s_data_fp) {
        ESP_LOGE(TAG, "open %s failed (errno=%d)", s_data_path, errno);
        return false;
    }
    s_log_fp = fopen(s_log_path, "a");
    if (!s_log_fp) {
        ESP_LOGE(TAG, "open %s failed (errno=%d)", s_log_path, errno);
        fclose(s_data_fp);
        s_data_fp = NULL;
        return false;
    }

    if (data_new) {
        fputs("time,temp_c,ph,tds_ppm,turb_ntu,level_pct,light_pct,co2_ppm,valid\n", s_data_fp);
        fflush(s_data_fp);
    }

    s_cur_y = now->year;
    s_cur_m = now->month;
    s_cur_d = now->day;
    s_active = true;
    s_rows = 0;
    ESP_LOGI(TAG, "recording -> %s", s_data_path);
    return true;
}

static void write_sensor_row(const pcf8563_time_t *now) {
    if (!s_data_fp) {
        return;
    }

    const sensor_sample_t *t = sensor_hub_get(SH_TEMP);
    const sensor_sample_t *p = sensor_hub_get(SH_PH);
    const sensor_sample_t *tds = sensor_hub_get(SH_TDS);
    const sensor_sample_t *tur = sensor_hub_get(SH_TURBIDITY);
    const sensor_sample_t *lvl = sensor_hub_get(SH_LEVEL);
    const sensor_sample_t *lit = sensor_hub_get(SH_LIGHT);
    const sensor_sample_t *co2 = sensor_hub_get(SH_CO2);

    char valid[8];
    const sensor_sample_t *all[SH_COUNT] = { t, p, tds, tur, lvl, lit, co2 };
    for (int i = 0; i < SH_COUNT && i < 7; ++i) {
        valid[i] = (all[i] && all[i]->err == ESP_OK) ? '1' : '0';
    }
    valid[7] = '\0';

    char row[256];
    int n = snprintf(row, sizeof(row),
                     "%04u-%02u-%02u %02u:%02u:%02u,%.2f,%.2f,%.0f,%.1f,%.1f,%.1f,%.0f,%s\n",
                     (unsigned)now->year, (unsigned)now->month, (unsigned)now->day,
                     (unsigned)now->hour, (unsigned)now->minute, (unsigned)now->second,
                     (double)t->value, (double)p->value, (double)tds->value,
                     (double)tur->value, (double)lvl->value, (double)lit->value,
                     (double)co2->value, valid);
    if (n > 0) {
        const size_t wrote = fwrite(row, 1, (size_t)n, s_data_fp);
        if (wrote != (size_t)n) {
            s_write_failed = true;
            return;
        }
        ++s_rows;
        ++s_streamed_csv;
        emit_line("csv", s_rows, row, (size_t)n);
    }
}

static void write_pending_logs(void) {
    if (!s_log_fp || !s_queue) {
        /* 没在记录时也要把队列排空，避免越积越多的旧日志在开卡时涌出来 */
        sd_log_item_t drop;
        while (xQueueReceive(s_queue, &drop, 0) == pdTRUE) {
        }
        return;
    }
    sd_log_item_t item;
    pcf8563_time_t now = {0};
    time_service_now(&now);
    while (xQueueReceive(s_queue, &item, 0) == pdTRUE) {
        char line[SD_LOG_LINE_MAX + 32];
        int n = snprintf(line, sizeof(line), "%04u-%02u-%02u %02u:%02u:%02u %s\n",
                         (unsigned)now.year, (unsigned)now.month, (unsigned)now.day,
                         (unsigned)now.hour, (unsigned)now.minute, (unsigned)now.second,
                         item.text);
        if (n > 0) {
            const size_t wrote = fwrite(line, 1, (size_t)n, s_log_fp);
            if (wrote != (size_t)n) {
                s_write_failed = true;
                break;
            }
            ++s_streamed_log;
            emit_line("log", s_streamed_log, line, (size_t)n);
        }
    }
}

/**
 * @brief 关一次再开一次，把 FAT 目录项刷到卡上
 *
 * 关键：FatFs 的 f_sync（fflush 会调）只保证数据簇落盘，**目录项里的文件大小
 * 往往要等 f_close 才更新**。表现就是板子说写了 N 行，但卡插电脑看不到文件、
 * 或者只有 0 字节。每 10 秒做一次 close+reopen 就能让读卡器随时看到完整数据。
 */
static void sync_files(void) {
    if (!s_active || !s_data_path[0]) {
        return;
    }

    if (s_data_fp) {
        (void)fflush(s_data_fp);
        (void)fclose(s_data_fp);
        s_data_fp = NULL;
    }
    if (s_log_fp) {
        (void)fflush(s_log_fp);
        (void)fclose(s_log_fp);
        s_log_fp = NULL;
    }

    /* 顺便记下真实文件大小（在 SD 任务里 stat 是安全的，不会被 VFS 锁卡住） */
    struct stat st;
    if (stat(s_data_path, &st) == 0) {
        s_data_size = (uint32_t)st.st_size;
    }
    if (stat(s_log_path, &st) == 0) {
        s_log_size = (uint32_t)st.st_size;
    }

    s_data_fp = fopen(s_data_path, "a");
    s_log_fp = fopen(s_log_path, "a");
    if (!s_data_fp || !s_log_fp) {
        ESP_LOGW(TAG, "reopen during sync failed, will remount");
        if (s_data_fp) {
            fclose(s_data_fp);
            s_data_fp = NULL;
        }
        if (s_log_fp) {
            fclose(s_log_fp);
            s_log_fp = NULL;
        }
        s_active = false;
        (void)sd_card_deinit(SD_CARD_MOUNT_POINT);
        return;
    }
    s_active = true;
}

/**
 * @brief 检查卡是否还在
 *
 * 热插拔的关键：sd_card_is_mounted() 只会被 init/deinit 改，卡被拔掉它仍是 true。
 * 真正的信号是写失败（fwrite 短写 / fflush 非 0 / ferror）。
 * 一旦发现，就关文件 + 卸载，主循环里那个"每 10 秒重试挂载"的逻辑会重新把它挂上，
 * 卡插回来就能自动恢复记录。
 */
static void check_card_health(void) {
    bool bad = s_write_failed;
    s_write_failed = false;

    /* 这里原来每秒 fflush 两个文件。FFatFs 的 f_sync 在这张卡上要几十毫秒，
     * 每秒两次就是持续占着 VFS 锁 —— 曲线读历史、MQTT 落盘全被它拖慢。
     * ferror() 已经能反映之前 fwrite 的失败（拔卡就是这个表现），
     * 真正的 fflush 交给 10 秒一次的 sync_files()。 */
    if (!bad && s_data_fp && ferror(s_data_fp)) {
        bad = true;
    }
    if (!bad && s_log_fp && ferror(s_log_fp)) {
        bad = true;
    }

    if (!bad) {
        return;
    }

    ESP_LOGW(TAG, "SD write error -> card likely removed, unmounting to allow re-plug");
    close_files();
    (void)sd_card_deinit(SD_CARD_MOUNT_POINT);
}

static void sd_logger_task(void *arg) {
    (void)arg;
    TickType_t last = xTaskGetTickCount();

    while (s_run) {
        const TickType_t now_tick = xTaskGetTickCount();
        const uint32_t elapsed_ms = (uint32_t)((now_tick - last) * portTICK_PERIOD_MS);

        if (!sd_card_is_mounted()) {
            if (s_active) {
                close_files();
                ESP_LOGW(TAG, "SD card removed, recording stopped");
            }
            (void)s_write_failed;
            /* 尝试挂载（失败不影响主控循环）。
             * 从 10 秒缩短到 3 秒：插卡后能更快开始记录。
             * 没卡时一次失败要 ~1 秒（sdmmc 超时），3 秒间隔不会把任务压死。 */
            const uint32_t now_t = (uint32_t)xTaskGetTickCount();
            if ((uint32_t)(now_t - s_last_mount_try) >= pdMS_TO_TICKS(3000)) {
                s_last_mount_try = now_t;
                esp_err_t mr = sd_card_init(SD_CARD_MOUNT_POINT);
                if (mr == ESP_OK) {
                    ESP_LOGI(TAG, "SD card mounted, recording will start");
                } else {
                    ESP_LOGW(TAG, "SD mount retry: %s", esp_err_to_name(mr));
                }
            }
            vTaskDelay(pdMS_TO_TICKS(SD_RETRY_MS));
            last = xTaskGetTickCount();
            continue;
        }

        pcf8563_time_t now = {0};
        time_service_now(&now);

        /* 跨天 or 首次打开：换文件 */
        if (s_active && (now.year != s_cur_y || now.month != s_cur_m || now.day != s_cur_d)) {
            close_files();
        }
        if (!s_active && !open_files_for(&now)) {
            vTaskDelay(pdMS_TO_TICKS(SD_RETRY_MS));
            last = xTaskGetTickCount();
            continue;
        }

        write_pending_logs();

        if (elapsed_ms >= SD_TASK_PERIOD_MS) {
            write_sensor_row(&now);
            check_card_health();   /* 顺手检测卡是否被拔掉 */
            /* 按固定节拍推进，而不是 last = now_tick。
             * 后者会让每次循环的抖动累积成漂移，整秒被"吃掉"就少一行。 */
            last += pdMS_TO_TICKS(SD_TASK_PERIOD_MS);
            if ((int32_t)(now_tick - last) > (int32_t)pdMS_TO_TICKS(3000)) {
                /* 落后超过 3 秒说明卡/同步严重阻塞，重新对齐，别疯狂补写 */
                last = now_tick;
            }
        }

        /* 每 10 秒刷一次目录项，保证随时拔卡都能读全 */
        {
            const uint32_t now_t2 = (uint32_t)xTaskGetTickCount();
            if ((uint32_t)(now_t2 - s_last_sync_ms) >= pdMS_TO_TICKS(SD_SYNC_PERIOD_MS)) {
                s_last_sync_ms = now_t2;
                sync_files();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }

    close_files();
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t sd_logger_start(void) {
    if (s_task) {
        return ESP_OK;
    }
    if (!s_queue) {
        s_queue = xQueueCreate(SD_LOG_QUEUE_LEN, sizeof(sd_log_item_t));
        if (!s_queue) {
            return ESP_ERR_NO_MEM;
        }
    }
    s_run = true;
    /* 栈必须够大：sd_card_init -> esp_vfs_fat_sdspi_mount 这条路径很吃栈，
     * 4096 会在挂载成功那一刻栈溢出，任务直接挂掉（表现是 mounted=1 但 active 永远 0）。 */
    BaseType_t ok = xTaskCreate(sd_logger_task, "sd_logger", 8192, NULL, 3, &s_task);
    if (ok != pdPASS) {
        s_run = false;
        s_task = NULL;
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "logger task started (mounted=%d)", (int)sd_card_is_mounted());
    return ESP_OK;
}

esp_err_t sd_logger_stop(void) {
    if (!s_task) {
        return ESP_OK;
    }
    s_run = false;
    /* 等任务自己收尾 */
    for (int i = 0; i < 50 && s_task; ++i) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return ESP_OK;
}

esp_err_t sd_logger_flush(void) {
    if (s_data_fp) {
        fflush(s_data_fp);
    }
    if (s_log_fp) {
        fflush(s_log_fp);
    }
    return ESP_OK;
}
