#include "utils/timer_helper.h"

uint32_t timer_helper_now_ms(void) {
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

bool timer_helper_has_elapsed(uint32_t start_ms, uint32_t duration_ms) {
    return (timer_helper_now_ms() - start_ms) >= duration_ms;
}
