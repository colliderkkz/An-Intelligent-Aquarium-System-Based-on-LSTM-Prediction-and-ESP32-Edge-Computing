#ifndef SENSOR_PH_SENSOR_H
#define SENSOR_PH_SENSOR_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t sensor_ph_init(void);
esp_err_t sensor_ph_read(float *ph_value);
float sensor_ph_get_last(void);

#ifdef __cplusplus
}
#endif

#endif // SENSOR_PH_SENSOR_H
