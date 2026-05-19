#ifndef DS18B20_H
#define DS18B20_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ds18b20_init(void);
esp_err_t ds18b20_read_temperature_c(float *celsius);

#ifdef __cplusplus
}
#endif

#endif // DS18B20_H
