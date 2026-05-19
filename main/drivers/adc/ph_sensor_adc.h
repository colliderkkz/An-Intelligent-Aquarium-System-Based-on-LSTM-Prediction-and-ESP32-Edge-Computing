#ifndef PH_SENSOR_ADC_H
#define PH_SENSOR_ADC_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ph_sensor_init(void);
esp_err_t ph_sensor_read(float temperature, float *ph_value);

#ifdef __cplusplus
}
#endif

#endif // PH_SENSOR_ADC_H
