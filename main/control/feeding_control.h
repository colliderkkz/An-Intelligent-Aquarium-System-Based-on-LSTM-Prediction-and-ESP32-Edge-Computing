#ifndef FEEDING_CONTROL_H
#define FEEDING_CONTROL_H

#include "drivers/pcf8563.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t feeding_control_init(void);
esp_err_t feeding_control_update(const pcf8563_time_t *now);

#ifdef __cplusplus
}
#endif

#endif // FEEDING_CONTROL_H
