#include "history.h"
#include "esp_timer.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "service/sd_logger.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "HISTORY";

/* 冷启动时读文件末尾这么多字节做历史（约 800 行） */
#define TAIL_BYTES      (48 * 1024)
/* RAM 曲线缓存：600 点，按 20 秒一个点算覆盖 3.3 小时 */
#define HIST_MAX        600
/* 实时采样最小间隔（秒） */
#define SAMPLE_MIN_S    20
/* 一次从 SD 里最多捞多少个点 */
#define SEED_MAX        160

/*
 * 之所以要缓存：这块板子的 SD 卡读 48KB 要 9 秒（实测 open=90ms
 * seek=330ms read=9410ms，约 5KB/s），每次手机问一次曲线都同步读一遍，
 * 表现就是"曲线半天不更新"。现在只在冷启动读一次，之后从 RAM 直接拼 JSON。
 *
 * 时间基准统一用"开机秒"（int32）。SD 里比开机更早的采样是负数，
 * 这样实时点和历史点能放进同一个数组里排序、也不用管两边时钟是否一致。
 */
typedef struct {
    int32_t ts;
    float temp, ph, level, light;
} hist_pt_t;

static hist_pt_t s_cache[HIST_MAX];
static int s_cache_n = 0;
static bool s_seed_tried = false;
static int s_seed_fail = 0;
static SemaphoreHandle_t s_lock = NULL;

/* SD 尾部流式解析用的固定缓冲（不占堆，避免 48KB malloc 失败） */
static hist_pt_t s_stage[SEED_MAX];
static uint32_t s_stage_sec[SEED_MAX];
static uint32_t s_raw[SEED_MAX];

static int32_t uptime_s(void) {
    return (int32_t)(esp_timer_get_time() / 1000000);
}

static void cache_push(int32_t ts, float temp, float ph, float level, float light) {
    if (s_cache_n >= HIST_MAX) {
        /* 满了丢掉最旧的一个 */
        memmove(&s_cache[0], &s_cache[1], sizeof(hist_pt_t) * (size_t)(HIST_MAX - 1));
        s_cache_n = HIST_MAX - 1;
    }
    hist_pt_t *d = &s_cache[s_cache_n++];
    d->ts = ts;
    d->temp = temp;
    d->ph = ph;
    d->level = level;
    d->light = light;
}

void history_note_sample(float temp, float ph, float level, float light) {
    if (temp < 0 && ph < 0 && level < 0 && light < 0) {
        return;                         /* 全无效，不记 */
    }
    /* 冷启动播种时会 memmove 整个缓存，这里必须让路。
     * 用 0 超时的 try-lock：宁可丢一个采样点，也绝不能把 2 秒上报任务卡住。 */
    if (s_lock != NULL && xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return;
    }
    const int32_t now = uptime_s();
    if (s_cache_n > 0) {
        const int32_t d = now - s_cache[s_cache_n - 1].ts;
        if (d >= 0 && d < SAMPLE_MIN_S) {
            /* 同一个 20 秒窗口：只刷新数值，不新增点 */
            s_cache[s_cache_n - 1].temp = temp;
            s_cache[s_cache_n - 1].ph = ph;
            s_cache[s_cache_n - 1].level = level;
            s_cache[s_cache_n - 1].light = light;
            if (s_lock != NULL) {
                (void)xSemaphoreGive(s_lock);
            }
            return;
        }
    }
    cache_push(now, temp, ph, level, light);
    if (s_lock != NULL) {
        (void)xSemaphoreGive(s_lock);
    }
}

/* ---------------- 解析 SD 尾部 ---------------- */

/* 解析一行 CSV：行首 "YYYY-MM-DD HH:MM:SS"，后面 temp,ph,tds,turb,level,light,co2,mask */
static bool parse_row(const char *line, hist_pt_t *out, uint32_t *raw_sec) {
    *raw_sec = 0;

    /* 不转 Epoch（时区/闰年都麻烦），只折算成单调递增的秒数，
     * 最后取"与最新一条的差值"，时间轴就和 SD 卡里一致。 */
    {
        unsigned yy = 0, mo = 0, dd = 0, hh = 0, mi = 0, ss = 0;
        int used = 0;
        if (sscanf(line, "%4u-%2u-%2u %2u:%2u:%2u%n",
                   &yy, &mo, &dd, &hh, &mi, &ss, &used) == 6 && used > 0) {
            *raw_sec = ((((mo * 31u) + dd) * 24u + hh) * 3600u) + (mi * 60u) + ss;
        }
        (void)yy;
    }

    const char *p = strchr(line, ',');
    if (!p) {
        return false;
    }
    float temp = -1, ph = -1, tds = -1, turb = -1, lvl = -1, lit = -1;
    int co2 = 0;
    char mask[16] = {0};

    int n = sscanf(p + 1, "%f,%f,%f,%f,%f,%f,%d,%15s",
                   &temp, &ph, &tds, &turb, &lvl, &lit, &co2, mask);
    if (n < 6) {
        return false;
    }

    /* mask = "0100010"：temp,ph,tds,turb,level,light,co2 */
    const bool v_temp = (mask[0] == '1');
    const bool v_ph   = (mask[1] == '1');
    const bool v_lvl  = (mask[4] == '1');
    const bool v_lit  = (mask[5] == '1');

    out->temp  = v_temp ? temp : -1.0f;
    out->ph    = v_ph   ? ph   : -1.0f;
    out->level = v_lvl  ? lvl  : -1.0f;
    out->light = v_lit  ? lit  : -1.0f;
    return true;
}

/**
 * @brief 把 SD 尾部解析成按时间升序的点，ts 以"开机秒"为基准
 * @return 点数（0 表示没数据）
 */
static int parse_sd_tail(hist_pt_t *out, int max) {
    if (max <= 0 || !sd_logger_is_active()) {
        return 0;
    }
    const char *path = sd_logger_data_path();
    if (!path || !path[0]) {
        return 0;
    }

    const uint32_t t_open0 = (uint32_t)(esp_timer_get_time() / 1000);
    FILE *f = fopen(path, "r");
    if (!f) {
        ESP_LOGW(TAG, "open %s failed", path);
        return 0;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return 0;
    }
    const long size = ftell(f);
    if (size <= 0) {
        fclose(f);
        return 0;
    }

    const long start = (size > TAIL_BYTES) ? (size - TAIL_BYTES) : 0;
    const size_t len = (size_t)(size - start);

    /*
     * 这里绝对不能 malloc 48KB：实测开机后最大连续堆只有 ~30KB，
     * malloc(49152) 直接失败 —— 这就是"曲线永远只有实时点"的真正原因
     * （日志里 "no mem for 49152 bytes"）。
     * 改成流式 fgets + 固定环形缓冲，堆占用为 0。
     *
     * 平均一行约 60 字节，据此估算取点步长，保证点数不超过 max。
     */
    long est_rows = (long)(len / 60u);
    if (est_rows < 2) {
        est_rows = 2;
    }
    int step = (int)(est_rows / max);
    if (step < 1) {
        step = 1;
    }

    if (fseek(f, start, SEEK_SET) != 0) {
        fclose(f);
        return 0;
    }

    int n = 0, wr = 0, lineno = 0;
    char line[192];
    while (fgets(line, sizeof(line), f) != NULL) {
        lineno++;
        if (lineno == 1 && start > 0) {
            continue;                       /* 第一行是被截断的半行 */
        }
        if (step > 1 && (lineno % step) != 0) {
            continue;
        }
        hist_pt_t pt;
        uint32_t sec = 0;
        if (!parse_row(line, &pt, &sec)) {
            continue;
        }
        s_stage[wr] = pt;
        s_stage_sec[wr] = sec;
        wr = (wr + 1) % max;
        if (n < max) {
            n++;
        }
    }
    fclose(f);

    const uint32_t t_open = (uint32_t)(esp_timer_get_time() / 1000) - t_open0;
    if (t_open > 2000u) {
        ESP_LOGW(TAG, "SD read slow: %ums, %d lines", (unsigned)t_open, lineno);
    }
    if (n < 2) {
        return 0;
    }

    /* 环形缓冲展开成"旧 -> 新" */
    int idx = 0;
    const int first = (n == max) ? wr : 0;
    for (int i = 0; i < n; i++) {
        const int si = (first + i) % max;
        out[idx] = s_stage[si];
        s_raw[idx] = s_stage_sec[si];
        idx++;
    }

    /* out[idx-1] 是最新的一条；其它点按"与它差多少秒"折算成开机秒 */
    const uint32_t base_raw = s_raw[idx - 1];
    const int32_t base_ts = uptime_s();
    for (int i = 0; i < idx; i++) {
        uint32_t ago;
        if (base_raw != 0 && s_raw[i] != 0 && base_raw >= s_raw[i]) {
            ago = base_raw - s_raw[i];
        } else {
            ago = (uint32_t)(idx - 1 - i) * (uint32_t)step;   /* 没时间戳就按行估 */
        }
        if (ago > 30u * 86400u) {
            ago = 30u * 86400u;                      /* 防止脏数据把轴拉爆 */
        }
        out[i].ts = base_ts - (int32_t)ago;
    }

    ESP_LOGI(TAG, "seeded %d points (step %d, %d lines, %ums)",
             idx, step, lineno, (unsigned)t_open);
    return idx;
}

/* 冷启动把 SD 历史塞进缓存；首次成功之前允许重试几次 */
static void cache_seed(void) {
    if (s_seed_tried || s_seed_fail >= 3) {
        return;
    }

    /* 必须 static：SEED_MAX 个点有 2.4KB，放栈上会把 6KB 的 hist 任务栈撑爆
     * （之前就是这里栈溢出，表现为"一发 hist 板子就重启"） */
    static hist_pt_t tmp[SEED_MAX];
    const int n = parse_sd_tail(tmp, SEED_MAX);
    if (n <= 0) {
        s_seed_fail++;                  /* 没卡/还没数据：下次再试 */
        return;
    }
    s_seed_tried = true;
    int room = HIST_MAX - s_cache_n;
    if (room <= 0) {
        ESP_LOGW(TAG, "seed skipped: cache full (%d)", s_cache_n);
        return;
    }
    const int first = (n > room) ? (n - room) : 0;   /* 只保留最近的 room 个 */
    const int cnt = n - first;
    /* 历史点比实时点都旧，整体插到前面 */
    if (s_cache_n > 0) {
        memmove(&s_cache[cnt], &s_cache[0], sizeof(hist_pt_t) * (size_t)s_cache_n);
    }
    memcpy(&s_cache[0], &tmp[first], sizeof(hist_pt_t) * (size_t)cnt);
    s_cache_n += cnt;
    ESP_LOGI(TAG, "seed ok: +%d points, cache=%d, oldest ago=%lds",
             cnt, s_cache_n, (long)(uptime_s() - s_cache[0].ts));
}

/* ---------------- 对外：拼 JSON ---------------- */

esp_err_t history_build_json(char *out, size_t cap, int want) {
    if (!out || cap < 256 || want <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (want > 160) {
        want = 160;
    }

    /* 预热任务和手机请求可能同时进来，缓存是共享的，串行化一下。
     * 不能 portMAX_DELAY：冷启动预热要读 9 秒 SD，手机这边的 hist 请求
     * 会被一直挂着，表现就是"点了没反应"。1.5 秒拿不到就直接回 busy。 */
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    bool locked = (s_lock == NULL) ||
                  (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1500)) == pdTRUE);
    if (!locked) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;
    /* 注意：不能用"缓存里点数不够"当条件 —— 实时点在开机 40 秒内就会攒够 2 个，
     * 那时 SD 历史就永远读不进来了。这里只按"没成功过"判断。 */
    if (!s_seed_tried) {
        cache_seed();                   /* 冷启动才有这一步，约 9 秒 */
    }
    if (s_cache_n < 2) {
        ret = ESP_ERR_NOT_FOUND;
        goto done;
    }

    /* 从最新往回按步长挑，保证最后一点一定是最新采样 */
    int step = s_cache_n / want;
    if (step < 1) {
        step = 1;
    }
    int pick[161];
    int cnt = 0;
    for (int i = s_cache_n - 1; i >= 0 && cnt < want; i -= step) {
        pick[cnt++] = i;
    }

    const int32_t now = uptime_s();
    size_t w = 0;
    {
        char head[64];
        int hn = snprintf(head, sizeof(head), "{\"type\":\"hist\",\"step\":0,\"n\":%d,\"v\":[", cnt);
        if (hn < 0 || (size_t)hn >= sizeof(head) || (size_t)hn >= cap - 8) {
            ret = ESP_ERR_NO_MEM;
            goto done;
        }
        memcpy(out, head, (size_t)hn);
        w = (size_t)hn;
    }

    int written = 0;
    for (int j = cnt - 1; j >= 0; j--) {          /* pick 是"新->旧"，反过来输出 */
        const hist_pt_t *pt = &s_cache[pick[j]];
        int32_t ago = now - pt->ts;
        if (ago < 0) {
            ago = 0;
        }
        if (ago > 999999) {
            ago = 999999;
        }
        char item[64];
        int n2 = snprintf(item, sizeof(item), "%s[%d,%.1f,%.2f,%.1f,%.0f]",
                          written ? "," : "", (int)ago,
                          (double)pt->temp, (double)pt->ph,
                          (double)pt->level, (double)pt->light);
        if (n2 < 0 || (size_t)n2 >= sizeof(item)) {
            break;
        }
        if (w + (size_t)n2 >= cap - 8) {
            break;                     /* 放不下就整条不写，保证 JSON 合法 */
        }
        memcpy(out + w, item, (size_t)n2);
        w += (size_t)n2;
        written++;
    }
    snprintf(out + w, cap - w, "]}");

    /* 回填真实条数 */
    {
        char head[64];
        int hn = snprintf(head, sizeof(head), "{\"type\":\"hist\",\"step\":0,\"n\":%d,\"v\":[", written);
        if (hn > 0) {
            memcpy(out, head, (size_t)hn);
        }
    }

done:
    if (s_lock != NULL && locked) {
        (void)xSemaphoreGive(s_lock);
    }
    return ret;
}

