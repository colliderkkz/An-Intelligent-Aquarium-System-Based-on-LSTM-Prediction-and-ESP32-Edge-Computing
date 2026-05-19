#ifndef LIGHT_SENSOR_H
#define LIGHT_SENSOR_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t light_sensor_init(void);
esp_err_t light_sensor_read(float *percent);

#ifdef __cplusplus
}
#endif

#endif // LIGHT_SENSOR_H
