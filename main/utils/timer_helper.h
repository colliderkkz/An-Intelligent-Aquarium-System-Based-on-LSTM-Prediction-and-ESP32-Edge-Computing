#ifndef TIMER_HELPER_H
#define TIMER_HELPER_H

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t timer_helper_now_ms(void);
bool timer_helper_has_elapsed(uint32_t start_ms, uint32_t duration_ms);

#ifdef __cplusplus
}
#endif

#endif // TIMER_HELPER_H
