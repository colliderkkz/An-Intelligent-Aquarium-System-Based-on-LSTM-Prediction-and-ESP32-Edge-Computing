#ifndef STUB_FREERTOS_SEMPHR_H
#define STUB_FREERTOS_SEMPHR_H

#include "freertos/FreeRTOS.h"

typedef struct semaphore_ctx_t *SemaphoreHandle_t;

SemaphoreHandle_t xSemaphoreCreateMutex(void);
SemaphoreHandle_t xSemaphoreCreateBinary(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks);
BaseType_t xSemaphoreGive(SemaphoreHandle_t sem);
void vSemaphoreDelete(SemaphoreHandle_t sem);

#endif
