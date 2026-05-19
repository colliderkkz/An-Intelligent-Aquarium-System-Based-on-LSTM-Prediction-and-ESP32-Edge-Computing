#ifndef PUMP_H
#define PUMP_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t pump_init(void);
esp_err_t pump_set_state(bool on);
bool pump_get_state(void);

#ifdef __cplusplus
}
#endif

#endif // PUMP_H
