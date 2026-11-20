#ifndef STUB_ROM_ETS_SYS_H
#define STUB_ROM_ETS_SYS_H

#include <stdint.h>

void ets_delay_us(uint32_t us);
void ets_printf(const char *fmt, ...);

#endif
