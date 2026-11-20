#ifndef STUB_FREERTOS_H
#define STUB_FREERTOS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef unsigned int UBaseType_t;

#define pdTRUE  1
#define pdFALSE 0
#define pdPASS  1
#define pdFAIL  0

#define portMAX_DELAY 0xFFFFFFFFu

#ifndef configTICK_RATE_HZ
#define configTICK_RATE_HZ 100
#endif

#define portTICK_PERIOD_MS (1000 / configTICK_RATE_HZ)
#define pdMS_TO_TICKS(ms)  ((TickType_t)(((uint64_t)(ms) * configTICK_RATE_HZ) / 1000u))

typedef struct {
    int reserved;
} portMUX_TYPE;

#define portMUX_INITIALIZER_UNLOCKED { 0 }

void vPortEnterCritical(portMUX_TYPE *mux);
void vPortExitCritical(portMUX_TYPE *mux);

/* 中断上下文判定（真实 IDF 由 portmacro 提供） */
int xPortInIsrContext(void);

#define taskENTER_CRITICAL(mux) vPortEnterCritical(mux)
#define taskEXIT_CRITICAL(mux)  vPortExitCritical(mux)

#endif
