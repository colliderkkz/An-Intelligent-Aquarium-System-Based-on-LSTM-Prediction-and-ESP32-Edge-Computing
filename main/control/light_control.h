#ifndef LIGHT_CONTROL_H
#define LIGHT_CONTROL_H

#include "drivers/pcf8563.h"
#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t light_control_init(void);
esp_err_t light_control_update(const pcf8563_time_t *now);
bool light_control_is_light_on(void);

#ifdef __cplusplus
}
#endif

#endif // LIGHT_CONTROL_H
