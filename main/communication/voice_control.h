#ifndef VOICE_CONTROL_H
#define VOICE_CONTROL_H

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t voice_control_init(void);
esp_err_t voice_control_start(void);
esp_err_t voice_control_execute_command(uint8_t cmd_id, const char *raw_text);
uint8_t voice_control_match_command(const char *line);

#ifdef __cplusplus
}
#endif

#endif // VOICE_CONTROL_H
