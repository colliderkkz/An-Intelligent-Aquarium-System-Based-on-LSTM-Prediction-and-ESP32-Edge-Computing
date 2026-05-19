#ifndef TEMP_CONTROL_H
#define TEMP_CONTROL_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t temp_control_init(void);
esp_err_t temp_control_update(void);
float temp_control_get_last_temperature_c(void);
bool temp_control_is_heating(void);

#ifdef __cplusplus
}
#endif

#endif // TEMP_CONTROL_H
