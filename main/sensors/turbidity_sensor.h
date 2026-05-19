#ifndef SENSOR_TURBIDITY_SENSOR_H
#define SENSOR_TURBIDITY_SENSOR_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t sensor_turbidity_init(void);
esp_err_t sensor_turbidity_read_ntu(float *ntu);
float sensor_turbidity_get_last_ntu(void);

#ifdef __cplusplus
}
#endif

#endif // SENSOR_TURBIDITY_SENSOR_H
