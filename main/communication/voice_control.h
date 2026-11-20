#ifndef VOICE_CONTROL_H
#define VOICE_CONTROL_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t voice_control_init(void);
esp_err_t voice_control_start(void);
esp_err_t voice_control_execute_command(uint8_t cmd_id, const char *raw_text);
uint8_t voice_control_match_command(const char *line);

/* ---- 诊断（VOICE 调试命令用）---- */
void voice_control_set_sniff(bool on);
bool voice_control_sniff_on(void);
uint32_t voice_control_rx_frames(void);
uint32_t voice_control_unmapped(void);
const char *voice_control_last_hex(void);
const char *voice_control_last_text(void);
bool voice_control_is_started(void);
int voice_control_baud(void);
esp_err_t voice_control_send(const char *text);
esp_err_t voice_control_set_baud(int baud);

#ifdef __cplusplus
}
#endif

#endif // VOICE_CONTROL_H
