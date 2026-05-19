#ifndef SENSOR_WATER_LEVEL_H
#define SENSOR_WATER_LEVEL_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t sensor_water_level_init(void);
esp_err_t sensor_water_level_read_percent(float *level_percent);
esp_err_t sensor_water_level_read_voltage(float *voltage_v);
float sensor_water_level_get_last_percent(void);

#ifdef __cplusplus
}
#endif

#endif // SENSOR_WATER_LEVEL_H
