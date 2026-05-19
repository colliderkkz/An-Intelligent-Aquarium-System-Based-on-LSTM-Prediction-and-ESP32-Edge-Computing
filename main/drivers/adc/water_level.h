#ifndef WATER_LEVEL_H
#define WATER_LEVEL_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t water_level_init(void);
esp_err_t water_level_read_voltage(float *voltage_v);
esp_err_t water_level_read(float *percent);

#ifdef __cplusplus
}
#endif

#endif // WATER_LEVEL_H
