#ifndef SENSOR_TEMP_SENSOR_H
#define SENSOR_TEMP_SENSOR_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t sensor_temp_init(void);
esp_err_t sensor_temp_read_c(float *temperature_c);
float sensor_temp_get_last_c(void);

#ifdef __cplusplus
}
#endif

#endif // SENSOR_TEMP_SENSOR_H
