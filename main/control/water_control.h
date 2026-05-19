#ifndef WATER_CONTROL_H
#define WATER_CONTROL_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t water_control_init(void);
esp_err_t water_control_update(void);
bool water_control_is_refilling(void);
float water_control_get_last_level_percent(void);

#ifdef __cplusplus
}
#endif

#endif // WATER_CONTROL_H
