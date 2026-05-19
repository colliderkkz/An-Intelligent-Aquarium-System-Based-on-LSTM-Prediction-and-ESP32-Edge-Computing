#ifndef TDS_SENSOR_H
#define TDS_SENSOR_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t tds_sensor_init(void);
esp_err_t tds_sensor_read(float temperature, float *tds_value);

#ifdef __cplusplus
}
#endif

#endif // TDS_SENSOR_H
