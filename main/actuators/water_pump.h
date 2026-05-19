#ifndef WATER_PUMP_H
#define WATER_PUMP_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t water_pump_init(void);
esp_err_t water_pump_set_state(bool on);
bool water_pump_get_state(void);

#ifdef __cplusplus
}
#endif

#endif // WATER_PUMP_H
