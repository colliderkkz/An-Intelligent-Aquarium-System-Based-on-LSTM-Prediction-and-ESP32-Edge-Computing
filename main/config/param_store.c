#include "config/param_store.h"

#include "config/error_codes.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <math.h>
#include <string.h>

static const char *TAG = "PARAM";

#define PARAM_NVS_NAMESPACE   "fishpz"
#define PARAM_NVS_KEY         "params"
#define PARAM_BLOB_MAGIC      0x46504B50u   /* 'FPKP' */
/* 版本 2：新增 inv_air（气泵极性），参数数量由 55 变 56。
 * 旧 NVS 数据会因 count/version 不匹配而自动作废并回落到默认值。 */
#define PARAM_BLOB_VERSION    2u

/* ---------- 描述表（X-Macro 自动生成，与枚举一一对应） ---------- */
static const param_desc_t s_desc[PARAM_IDX_COUNT] = {
#define X_DESC(id, name, group, def, minv, maxv, unit, dec, desc) \
    { name, group, (float)(def), (float)(minv), (float)(maxv), unit, dec, desc },
    PARAM_LIST(X_DESC)
#undef X_DESC
};

/* ---------- 当前值 ---------- */
static float s_values[PARAM_IDX_COUNT];
static bool s_initialized = false;
static bool s_dirty = false;

/* ---------- NVS 序列化结构 ---------- */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    uint32_t checksum;
    float values[PARAM_IDX_COUNT];
} param_blob_t;

static uint32_t xor_float_bits(uint32_t seed, float v) {
    uint32_t bits = 0;
    memcpy(&bits, &v, sizeof(bits));
    /* 混合位模式，避免同值异位导致校验漏检 */
    return (seed ^ (bits + 0x9E3779B9u + (seed << 6) + (seed >> 2)));
}

static uint32_t blob_checksum(const float *values, size_t count) {
    uint32_t seed = 0x811C9DC5u;
    for (size_t i = 0; i < count; i++) {
        seed = xor_float_bits(seed, values[i]);
    }
    return seed;
}

static float clamp_to_desc(int idx, float value) {
    const param_desc_t *d = &s_desc[idx];
    if (isnan(value)) {
        return d->default_value;
    }
    if (value < d->min_value) {
        return d->min_value;
    }
    if (value > d->max_value) {
        return d->max_value;
    }
    return value;
}

static void load_defaults(void) {
    for (int i = 0; i < PARAM_IDX_COUNT; i++) {
        s_values[i] = s_desc[i].default_value;
    }
}

int param_store_count(void) {
    return (int)PARAM_IDX_COUNT;
}

const param_desc_t *param_store_desc(int index) {
    if (index < 0 || index >= PARAM_IDX_COUNT) {
        return NULL;
    }
    return &s_desc[index];
}

int param_store_find(const char *name) {
    if (!name) {
        return -1;
    }
    for (int i = 0; i < PARAM_IDX_COUNT; i++) {
        if (strcmp(s_desc[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

esp_err_t param_store_init(void) {
    load_defaults();
    s_dirty = false;
    s_initialized = true;

    esp_err_t ret = param_store_load();
    if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "NVS load failed (%s), keep defaults", esp_err_to_name(ret));
    }
    ESP_LOGI(TAG, "param store ready, %d params", (int)PARAM_IDX_COUNT);
    return ESP_OK;
}

float param_get(param_id_t id) {
    if ((int)id < 0 || (int)id >= PARAM_IDX_COUNT) {
        return 0.0f;
    }
    return s_values[id];
}

float param_get_f(const char *name, float fallback) {
    int idx = param_store_find(name);
    if (idx < 0) {
        return fallback;
    }
    return s_values[idx];
}

uint32_t param_get_u32(param_id_t id) {
    float v = param_get(id);
    if (v < 0.0f) {
        return 0;
    }
    return (uint32_t)(v + 0.5f);
}

int param_get_int(param_id_t id) {
    return (int)param_get_u32(id);
}

bool param_get_bool(param_id_t id) {
    return param_get(id) >= 0.5f;
}

esp_err_t param_set(param_id_t id, float value) {
    if ((int)id < 0 || (int)id >= PARAM_IDX_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    float clamped = clamp_to_desc((int)id, value);
    if (clamped != s_values[id]) {
        s_values[id] = clamped;
        s_dirty = true;
    }
    return ESP_OK;
}

esp_err_t param_set_f(const char *name, float value) {
    int idx = param_store_find(name);
    if (idx < 0) {
        return APP_ERR_UNKNOWN_ID;
    }
    return param_set((param_id_t)idx, value);
}

bool param_store_dirty(void) {
    return s_dirty;
}

esp_err_t param_store_save(void) {
    if (!s_initialized) {
        return APP_ERR_INVALID_STATE;
    }

    param_blob_t blob;
    memset(&blob, 0, sizeof(blob));
    blob.magic = PARAM_BLOB_MAGIC;
    blob.version = PARAM_BLOB_VERSION;
    blob.count = (uint16_t)PARAM_IDX_COUNT;
    for (int i = 0; i < PARAM_IDX_COUNT; i++) {
        blob.values[i] = s_values[i];
    }
    blob.checksum = blob_checksum(blob.values, PARAM_IDX_COUNT);

    nvs_handle_t h;
    esp_err_t ret = nvs_open(PARAM_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(ret));
        return APP_ERR_NVS;
    }

    ret = nvs_set_blob(h, PARAM_NVS_KEY, &blob, sizeof(blob));
    if (ret == ESP_OK) {
        ret = nvs_commit(h);
    }
    nvs_close(h);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs save failed: %s", esp_err_to_name(ret));
        return APP_ERR_NVS;
    }

    s_dirty = false;
    ESP_LOGI(TAG, "params saved to NVS");
    return ESP_OK;
}

esp_err_t param_store_load(void) {
    if (!s_initialized) {
        return APP_ERR_INVALID_STATE;
    }

    nvs_handle_t h;
    esp_err_t ret = nvs_open(PARAM_NVS_NAMESPACE, NVS_READONLY, &h);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (ret != ESP_OK) {
        return APP_ERR_NVS;
    }

    param_blob_t blob;
    size_t len = sizeof(blob);
    ret = nvs_get_blob(h, PARAM_NVS_KEY, &blob, &len);
    nvs_close(h);

    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (ret != ESP_OK) {
        return APP_ERR_NVS;
    }
    if (len != sizeof(blob) || blob.magic != PARAM_BLOB_MAGIC ||
        blob.count != (uint16_t)PARAM_IDX_COUNT || blob.version != PARAM_BLOB_VERSION) {
        ESP_LOGW(TAG, "NVS blob mismatch (len=%u magic=0x%08lx count=%u ver=%u), use defaults",
                 (unsigned)len, (unsigned long)blob.magic, (unsigned)blob.count,
                 (unsigned)blob.version);
        return APP_ERR_NVS_BLOB;
    }
    if (blob_checksum(blob.values, PARAM_IDX_COUNT) != blob.checksum) {
        ESP_LOGW(TAG, "NVS blob checksum mismatch, use defaults");
        return APP_ERR_NVS_CRC;
    }

    for (int i = 0; i < PARAM_IDX_COUNT; i++) {
        s_values[i] = clamp_to_desc(i, blob.values[i]);
    }
    s_dirty = false;
    ESP_LOGI(TAG, "params restored from NVS");
    return ESP_OK;
}

esp_err_t param_store_reset_defaults(void) {
    if (!s_initialized) {
        return APP_ERR_INVALID_STATE;
    }
    load_defaults();
    s_dirty = true;
    ESP_LOGI(TAG, "params reset to defaults (not saved yet)");
    return ESP_OK;
}
