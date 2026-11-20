#include "power_diag.h"

#include "esp_log.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <string.h>

static const char *TAG = "PWR_DIAG";

#define NS      "pwrdiag"
#define K_BOOT  "boot"
#define K_BO     "bo"
#define K_PO     "po"
#define K_LAST  "last"

static int s_reason = 0;
static char s_reason_txt[24] = "UNKNOWN";
static char s_last_txt[24] = "-";
static uint32_t s_boot = 0;
static uint32_t s_brownout = 0;
static uint32_t s_poweron = 0;

static const char *reason_to_text(int r) {
    switch (r) {
        case ESP_RST_UNKNOWN:   return "UNKNOWN";
        case ESP_RST_POWERON:   return "POWERON";
        case ESP_RST_EXT:       return "EXT_PIN";
        case ESP_RST_SW:        return "SOFTWARE";
        case ESP_RST_PANIC:     return "PANIC";
        case ESP_RST_INT_WDT:   return "INT_WDT";
        case ESP_RST_TASK_WDT:  return "TASK_WDT";
        case ESP_RST_WDT:       return "WDT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";
        case ESP_RST_SDIO:      return "SDIO";
        default:                return "OTHER";
    }
}

/* 旧版本 IDF 可能没有这些枚举，用数值兜底 */
static const char *reason_to_text_ext(int r) {
    if (r == 11) return "USB";
    if (r == 12) return "JTAG";
    if (r == 13) return "EFUSE";
    if (r == 14) return "PWR_GLITCH";
    if (r == 15) return "CPU_LOCKUP";
    return reason_to_text(r);
}

static uint32_t get_u32(nvs_handle_t h, const char *key) {
    uint32_t v = 0;
    if (nvs_get_u32(h, key, &v) != ESP_OK) {
        v = 0;
    }
    return v;
}

esp_err_t power_diag_init(void) {
    s_reason = (int)esp_reset_reason();
    snprintf(s_reason_txt, sizeof(s_reason_txt), "%s", reason_to_text_ext(s_reason));

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed: %s (counters unavailable)", esp_err_to_name(err));
        return err;
    }

    s_boot     = get_u32(h, K_BOOT) + 1;
    s_brownout = get_u32(h, K_BO);
    s_poweron  = get_u32(h, K_PO);

    /* 上一次的复位原因 */
    size_t len = sizeof(s_last_txt);
    if (nvs_get_str(h, K_LAST, s_last_txt, &len) != ESP_OK) {
        snprintf(s_last_txt, sizeof(s_last_txt), "-");
    }

    if (s_reason == ESP_RST_BROWNOUT) {
        s_brownout++;
    }
    if (s_reason == ESP_RST_POWERON) {
        s_poweron++;
    }

    (void)nvs_set_u32(h, K_BOOT, s_boot);
    (void)nvs_set_u32(h, K_BO, s_brownout);
    (void)nvs_set_u32(h, K_PO, s_poweron);
    (void)nvs_set_str(h, K_LAST, s_reason_txt);
    (void)nvs_commit(h);
    nvs_close(h);

    ESP_LOGW(TAG, "reset=%s(%d) boot#%lu brownout=%lu poweron=%lu prev=%s",
             s_reason_txt, s_reason,
             (unsigned long)s_boot, (unsigned long)s_brownout,
             (unsigned long)s_poweron, s_last_txt);

    if (s_reason == ESP_RST_BROWNOUT) {
        ESP_LOGE(TAG, "*** 上次是欠压复位：供电不足（USB 电流不够或线太长）***");
    }
    if (s_brownout >= 3) {
        ESP_LOGE(TAG, "*** 累计欠压 %lu 次，供电不稳定的可能性很高 ***",
                 (unsigned long)s_brownout);
    }
    return ESP_OK;
}

const char *power_diag_reset_reason_text(void) { return s_reason_txt; }
int power_diag_reset_reason_raw(void) { return s_reason; }
uint32_t power_diag_boot_count(void) { return s_boot; }
uint32_t power_diag_brownout_count(void) { return s_brownout; }
uint32_t power_diag_poweron_count(void) { return s_poweron; }
const char *power_diag_last_reason_text(void) { return s_last_txt; }
