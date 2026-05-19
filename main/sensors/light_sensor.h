#ifndef SENSOR_LIGHT_SENSOR_H
#define SENSOR_LIGHT_SENSOR_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t sensor_light_init(void);
esp_err_t sensor_light_read_percent(float *light_percent);
float sensor_light_get_last_percent(void);

#ifdef __cplusplus
}
#endif

#endif // SENSOR_LIGHT_SENSOR_H
