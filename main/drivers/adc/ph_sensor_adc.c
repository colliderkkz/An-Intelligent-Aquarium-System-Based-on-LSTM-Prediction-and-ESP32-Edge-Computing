#include "ph_sensor_adc.h"

#include "adc_manager.h"
#include "config/param_store.h"
#include "config/pin_config.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

static const char *TAG = "PH_SENSOR";

/* 两次中值采样之间的间隔 */
#define PH_SAMPLE_INTERVAL_US 500

/* ================= pH 时间滤波 =================
 * 电极本身的噪声（±0.1~0.3）靠"单次 10 采样取中值"压不平，
 * 这里再加一层跨样本滤波，三层叠加：
 *   ① 单次 10 点中值（原有，抗尖峰）
 *   ② 最近 3 次的中值（抗偶发跳点）
 *   ③ EMA（α=0.35，2 秒一个样本 → 约 10 秒时间常数，抹平抖动）
 * 真实变化（例如校准液切换）不会被拖住：
 *   - 连续 2 次偏差都 > 0.6 就认为是真变化，直接跟过去；
 *   - 单次尖峰只做小幅跟随，不改变读数。
 */
#define PH_FILT_WIN        3
#define PH_FILT_ALPHA      0.35f
#define PH_FILT_STEP_PH    0.60f   /* 单步跳变判定阈值（pH） */
#define PH_FILT_JUMP_N     2       /* 连续几次才认定是真变化 */

static float s_hist[PH_FILT_WIN];
static int   s_hist_n = 0;
static int   s_hist_idx = 0;
static float s_ema = 0.0f;
static bool  s_ema_valid = false;
static int   s_jump_cnt = 0;

static void ph_filter_reset(void) {
    s_hist_n = 0;
    s_hist_idx = 0;
    s_ema_valid = false;
    s_jump_cnt = 0;
}

static float median_small(float *v, int n) {
    float t[PH_FILT_WIN];
    for (int i = 0; i < n && i < PH_FILT_WIN; i++) {
        t[i] = v[i];
    }
    for (int i = 0; i < n; i++) {
        for (int k = i + 1; k < n; k++) {
            if (t[i] > t[k]) {
                float tmp = t[i];
                t[i] = t[k];
                t[k] = tmp;
            }
        }
    }
    return (n % 2) ? t[n / 2] : (t[n / 2 - 1] + t[n / 2]) * 0.5f;
}

/* 输入：一次测量得到的 pH（已做单次中值）。输出：滤波后的 pH */
static float ph_filter_apply(float x) {
    if (!s_ema_valid) {
        /* 上电/传感器恢复后的第一次：直接采纳，避免从 0 慢慢爬 */
        s_ema = x;
        s_ema_valid = true;
        s_hist[0] = x;
        s_hist_n = 1;
        s_hist_idx = 1 % PH_FILT_WIN;
        s_jump_cnt = 0;
        return x;
    }

    s_hist[s_hist_idx] = x;
    s_hist_idx = (s_hist_idx + 1) % PH_FILT_WIN;
    if (s_hist_n < PH_FILT_WIN) {
        s_hist_n++;
    }
    float med = median_small(s_hist, s_hist_n);

    const float dev = med - s_ema;
    if (dev > PH_FILT_STEP_PH || dev < -PH_FILT_STEP_PH) {
        /* 可能是真变化（换溶液/校准），连续 N 次才认 */
        if (++s_jump_cnt >= PH_FILT_JUMP_N) {
            s_ema = med;
            s_jump_cnt = 0;
        } else {
            /* 先小幅跟随，别把一次尖峰写进读数 */
            s_ema += (dev > 0 ? 1.0f : -1.0f) * (PH_FILT_STEP_PH * 0.25f);
            s_ema = 0.5f * s_ema + 0.5f * med;   /* 轻微拉回，避免漂 */
        }
    } else {
        s_jump_cnt = 0;
        s_ema += PH_FILT_ALPHA * dev;
    }
    return s_ema;
}

static void sort_ascending(int *arr, int len) {
    for (int i = 0; i < len; i++) {
        for (int k = i + 1; k < len; k++) {
            if (arr[i] > arr[k]) {
                int tmp = arr[i];
                arr[i] = arr[k];
                arr[k] = tmp;
            }
        }
    }
}

esp_err_t ph_sensor_init(void) {
    return adc_manager_config(PH_ADC_UNIT, PH_ADC_CHANNEL);
}

esp_err_t ph_sensor_convert_checked(int mv, float temperature, float *out_ph) {
    const float voltage = (float)mv / 1000.0f;
    float ph = P(PARAM_IDX_PH_SLOPE) * voltage + P(PARAM_IDX_PH_OFFSET);

    /* 温度补偿：电极斜率随温度变化，按 pH/℃ 线性修正（默认 0，即不补偿） */
    const float temp_comp = P(PARAM_IDX_PH_TEMP_COMP);
    if (temp_comp != 0.0f) {
        ph += temp_comp * (temperature - 25.0f);
    }

    /* 先原样带出去，方便日志里看到真实换算值 */
    if (out_ph) {
        *out_ph = ph;
    }

    /* 超出 pH 0~14 说明探头断路/短路/没接好。
     * 以前这里是"钳位成 14.0 然后照常返回 OK"，导致上位机把假值当真实数据记录。 */
    if (ph > 14.0f || ph < 0.0f) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

float ph_sensor_convert_mv(int mv, float temperature) {
    float ph = 0.0f;
    (void)ph_sensor_convert_checked(mv, temperature, &ph);

    if (ph > 14.0f) {
        ph = 14.0f;
    }
    if (ph < 0.0f) {
        ph = 0.0f;
    }
    return ph;
}

esp_err_t ph_sensor_read_raw(float temperature, float *ph, int *raw, int *mv) {
    int samples[PH_MEDIAN_SAMPLE_COUNT] = {0};

    for (int i = 0; i < PH_MEDIAN_SAMPLE_COUNT; i++) {
        int raw_val = 0;
        esp_err_t ret = adc_manager_read(PH_ADC_UNIT, PH_ADC_CHANNEL, &raw_val, NULL);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "pH ADC read failed: %s", esp_err_to_name(ret));
            return ret;
        }
        samples[i] = raw_val;
        esp_rom_delay_us(PH_SAMPLE_INTERVAL_US);
    }

    sort_ascending(samples, PH_MEDIAN_SAMPLE_COUNT);
    /* 偶数个样本取中间两个的平均，避免取低中位数带来的系统性偏低 */
    const int mid = PH_MEDIAN_SAMPLE_COUNT / 2;
    const int median_raw = (samples[mid - 1] + samples[mid]) / 2;

    int mv_value = 0;
    esp_err_t ret = adc_manager_raw_to_mv(PH_ADC_UNIT, median_raw, &mv_value);
    if (ret != ESP_OK) {
        return ret;
    }

    /* 引脚悬空/对地短路时 ADC 会读到接近 0（或接近电源轨）。
     * 真实 pH 前置放大器的输出在 0.4~3.1V，超出这个窗口按"传感器未接入"处理，
     * 否则会被换算成 pH 14.0 这种假值。 */
    if (median_raw < ADC_RAIL_LOW_RAW || median_raw > ADC_RAIL_HIGH_RAW) {
        ESP_LOGD(TAG, "pH sensor not connected? raw=%d mv=%d", median_raw, mv_value);
        ph_filter_reset();
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (ph) {
        float v = 0.0f;
        esp_err_t cv = ph_sensor_convert_checked(mv_value, temperature, &v);
        if (cv != ESP_OK) {
            ESP_LOGD(TAG, "pH out of range: raw=%d mv=%d -> %.2f (probe not connected?)",
                     median_raw, mv_value, (double)v);
            ph_filter_reset();
            return cv;
        }
        /* 跨样本滤波后再给 2 位小数：
         * 以前只保留 1 位，噪声被量化成 0.1 的台阶，看起来更"跳"。 */
        float f = ph_filter_apply(v);
        *ph = ((float)((int)(f * 100.0f + 0.5f))) / 100.0f;
    }
    if (raw) {
        *raw = median_raw;
    }
    if (mv) {
        *mv = mv_value;
    }
    return ESP_OK;
}

esp_err_t ph_sensor_read(float temperature, float *ph_value) {
    if (!ph_value) {
        return ESP_ERR_INVALID_ARG;
    }
    return ph_sensor_read_raw(temperature, ph_value, NULL, NULL);
}
