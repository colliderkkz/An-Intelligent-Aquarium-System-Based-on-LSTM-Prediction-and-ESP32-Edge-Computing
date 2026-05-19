#include "sensors/co2_sensor.h"

#include "config/pin_config.h"
#include "drivers/sgp30.h"
#include <stdbool.h>

static bool s_initialized = false;
static i2c_bus_handle_t s_bus = NULL;
static sensor_co2_data_t s_last_data = {0};

esp_err_t sensor_co2_init(i2c_bus_handle_t bus) {
    if (!bus) {
        return ESP_ERR_INVALID_ARG;
    }

    s_bus = bus;
    esp_err_t ret = sgp30_init(s_bus, SGP30_I2C_ADDR);
    if (ret == ESP_OK) {
        s_initialized = true;
    }
    return ret;
}

esp_err_t sensor_co2_read(sensor_co2_data_t *data) {
    if (!data) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized || !s_bus) {
        return ESP_ERR_INVALID_STATE;
    }

    sgp30_data_t raw = {0};
    esp_err_t ret = sgp30_read_data(s_bus, SGP30_I2C_ADDR, &raw);
    if (ret != ESP_OK) {
        return ret;
    }

    s_last_data.co2_eq_ppm = raw.co2_eq;
    s_last_data.tvoc_ppb = raw.tvoc;
    *data = s_last_data;
    return ESP_OK;
}

sensor_co2_data_t sensor_co2_get_last(void) {
    return s_last_data;
}
