#ifndef CO2_VALVE_H
#define CO2_VALVE_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t co2_valve_init(void);
esp_err_t co2_valve_set_state(bool on);
bool co2_valve_get_state(void);

#ifdef __cplusplus
}
#endif

#endif // CO2_VALVE_H
