#ifndef UV_LAMP_H
#define UV_LAMP_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t uv_lamp_init(void);
esp_err_t uv_lamp_set_state(bool on);
bool uv_lamp_get_state(void);

#ifdef __cplusplus
}
#endif

#endif // UV_LAMP_H
