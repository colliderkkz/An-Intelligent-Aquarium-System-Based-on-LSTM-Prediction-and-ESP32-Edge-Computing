#ifndef SENSOR_CO2_SENSOR_H
#define SENSOR_CO2_SENSOR_H

#include "bus/i2c_bus.h"
#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t co2_eq_ppm;
    uint16_t tvoc_ppb;
} sensor_co2_data_t;

esp_err_t sensor_co2_init(i2c_bus_handle_t bus);
esp_err_t sensor_co2_read(sensor_co2_data_t *data);
sensor_co2_data_t sensor_co2_get_last(void);

#ifdef __cplusplus
}
#endif

#endif // SENSOR_CO2_SENSOR_H
