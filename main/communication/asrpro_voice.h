#ifndef ASRPRO_VOICE_H
#define ASRPRO_VOICE_H

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef esp_err_t (*asrpro_voice_cmd_handler_t)(uint8_t cmd_id, const char *raw_text);

esp_err_t asrpro_voice_start(asrpro_voice_cmd_handler_t handler);

#ifdef __cplusplus
}
#endif

#endif // ASRPRO_VOICE_H
