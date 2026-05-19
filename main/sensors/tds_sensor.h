#ifndef SENSOR_TDS_SENSOR_H
#define SENSOR_TDS_SENSOR_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t sensor_tds_init(void);
esp_err_t sensor_tds_read_ppm(float *tds_ppm);
float sensor_tds_get_last_ppm(void);

#ifdef __cplusplus
}
#endif

#endif // SENSOR_TDS_SENSOR_H
