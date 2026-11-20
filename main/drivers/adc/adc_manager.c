#include "adc_manager.h"

#include "config/pin_config.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdbool.h>
#include <string.h>

static const char *TAG = "ADC_MANAGER";

/* 采样间隔（微秒）：留一点建立时间，又不会拖慢整体周期 */
#define ADC_SAMPLE_INTERVAL_US 150

typedef struct {
    adc_oneshot_unit_handle_t oneshot;
    adc_cali_handle_t cali;
    bool cali_enabled;
    bool unit_ready;
    uint32_t configured_mask;      /* 已配置的通道掩码 */
} adc_unit_ctx_t;

/* 下标 0 -> ADC1，1 -> ADC2 */
static adc_unit_ctx_t s_units[2];
static bool s_adc_initialized = false;

/* 旧的默认参考电压，校准不可用时使用 */
#define ADC_FALLBACK_VREF_MV   3300
#define ADC_FULL_SCALE         4095

static int unit_index(adc_unit_t unit) {
    return (unit == ADC_UNIT_2) ? 1 : 0;
}

static esp_err_t adc_unit_calibrate(adc_unit_ctx_t *ctx, adc_unit_t unit) {
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cfg = {
        .unit_id = unit,
        .atten = ADC_ATTEN,
        .bitwidth = ADC_WIDTH,
    };
    esp_err_t ret = adc_cali_create_scheme_curve_fitting(&cfg, &ctx->cali);
    if (ret == ESP_OK) {
        ctx->cali_enabled = true;
    }
    return ret;
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cfg = {
        .unit_id = unit,
        .atten = ADC_ATTEN,
        .bitwidth = ADC_WIDTH,
    };
    esp_err_t ret = adc_cali_create_scheme_line_fitting(&cfg, &ctx->cali);
    if (ret == ESP_OK) {
        ctx->cali_enabled = true;
    }
    return ret;
#else
    (void)ctx;
    (void)unit;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static esp_err_t adc_unit_setup(adc_unit_t unit) {
    adc_unit_ctx_t *ctx = &s_units[unit_index(unit)];
    if (ctx->unit_ready) {
        return ESP_OK;
    }

    /* 注意：不同 IDF 版本的 adc_oneshot_unit_init_cfg_t 字段略有差异，
     * 这里只用 unit_id/ulp_mode（其余字段保持 0 = 默认时钟源）。 */
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = unit,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };

    esp_err_t ret = adc_oneshot_new_unit(&unit_cfg, &ctx->oneshot);
    if (ret != ESP_OK) {
        /* ADC2 在 WiFi 运行期间可能被占用，这里只记录不致命 */
        ESP_LOGW(TAG, "ADC unit %d init failed: %s", (int)unit + 1, esp_err_to_name(ret));
        return ret;
    }

    ret = adc_unit_calibrate(ctx, unit);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "ADC%d calibration enabled", (int)unit + 1);
    } else {
        ctx->cali_enabled = false;
        ESP_LOGW(TAG, "ADC%d calibration unavailable (%s), fallback to linear",
                 (int)unit + 1, esp_err_to_name(ret));
    }

    ctx->configured_mask = 0;
    ctx->unit_ready = true;
    return ESP_OK;
}

esp_err_t adc_manager_init(void) {
    if (s_adc_initialized) {
        return ESP_OK;
    }
    memset(s_units, 0, sizeof(s_units));

    esp_err_t ret1 = adc_unit_setup(ADC_UNIT_SENSOR);
    if (ret1 != ESP_OK) {
        ESP_LOGE(TAG, "ADC1 init failed: %s", esp_err_to_name(ret1));
    }

#if LIGHT_SENSOR_ON_ADC2
    esp_err_t ret2 = adc_unit_setup(ADC_UNIT_LIGHT_SENSOR);
    if (ret2 != ESP_OK) {
        ESP_LOGW(TAG, "ADC2 init failed: %s (light sensor will be unavailable)",
                 esp_err_to_name(ret2));
    }
#else
    (void)adc_unit_setup;
#endif

    s_adc_initialized = (ret1 == ESP_OK);
    return s_adc_initialized ? ESP_OK : ret1;
}

bool adc_manager_ready(void) {
    return s_adc_initialized;
}

esp_err_t adc_manager_config(adc_unit_t unit, adc_channel_t channel) {
    esp_err_t ret = adc_manager_init();
    if (ret != ESP_OK) {
        return ret;
    }

    adc_unit_ctx_t *ctx = &s_units[unit_index(unit)];
    if (!ctx->unit_ready) {
        ret = adc_unit_setup(unit);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    if ((int)channel < 0 || (int)channel >= 10) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ctx->configured_mask & (1u << (int)channel)) {
        return ESP_OK;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN,
        .bitwidth = ADC_WIDTH,
    };
    ret = adc_oneshot_config_channel(ctx->oneshot, channel, &chan_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC%d channel %d config failed: %s",
                 (int)unit + 1, (int)channel, esp_err_to_name(ret));
        return ret;
    }
    ctx->configured_mask |= (1u << (int)channel);
    return ESP_OK;
}

esp_err_t adc_manager_raw_to_mv(adc_unit_t unit, int raw, int *mv_out) {
    if (!mv_out) {
        return ESP_ERR_INVALID_ARG;
    }
    adc_unit_ctx_t *ctx = &s_units[unit_index(unit)];
    if (ctx->cali_enabled) {
        return adc_cali_raw_to_voltage(ctx->cali, raw, mv_out);
    }
    *mv_out = (raw * ADC_FALLBACK_VREF_MV) / ADC_FULL_SCALE;
    return ESP_OK;
}

esp_err_t adc_manager_read(adc_unit_t unit, adc_channel_t channel, int *raw_out, int *mv_out) {
    if (!raw_out && !mv_out) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = adc_manager_config(unit, channel);
    if (ret != ESP_OK) {
        return ret;
    }

    adc_unit_ctx_t *ctx = &s_units[unit_index(unit)];
    int raw_sum = 0;
    for (int i = 0; i < ADC_SAMPLE_COUNT; i++) {
        int raw = 0;
        ret = adc_oneshot_read(ctx->oneshot, channel, &raw);
        if (ret != ESP_OK) {
            return ret;
        }
        raw_sum += raw;
        /* BUGFIX: 旧代码用 vTaskDelay(pdMS_TO_TICKS(1)) 做采样间隔，
         * 在 100Hz tick 下等于 10ms，单次 ADC 读取要 80ms，pH 的 10 次
         * 中值滤波更是要 800ms。这里改成微秒级忙等。 */
        esp_rom_delay_us(ADC_SAMPLE_INTERVAL_US);
    }

    int raw_avg = raw_sum / ADC_SAMPLE_COUNT;
    if (raw_out) {
        *raw_out = raw_avg;
    }
    if (mv_out) {
        ret = adc_manager_raw_to_mv(unit, raw_avg, mv_out);
    }
    return ret;
}
