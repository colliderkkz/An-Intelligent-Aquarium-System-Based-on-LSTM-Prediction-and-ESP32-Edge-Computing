#include "mhz19b.h"

#include "esp_log.h"

static const char *TAG = "MHZ19B";

esp_err_t mhz19b_init(void) {
    ESP_LOGW(TAG, "MH-Z19B is not populated on this hardware revision; use SGP30 on the shared I2C bus");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t mhz19b_read(mhz19b_data_t *data) {
    (void)data;
    ESP_LOGW(TAG, "MH-Z19B read requested on unsupported hardware");
    return ESP_ERR_NOT_SUPPORTED;
}
