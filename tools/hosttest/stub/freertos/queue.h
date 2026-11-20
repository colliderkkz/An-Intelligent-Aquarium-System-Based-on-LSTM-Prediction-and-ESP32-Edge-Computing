#ifndef STUB_FREERTOS_QUEUE_H
#define STUB_FREERTOS_QUEUE_H

#include "freertos/FreeRTOS.h"

typedef struct queue_ctx_t *QueueHandle_t;

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size);
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t ticks);
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t ticks);
BaseType_t xQueueReset(QueueHandle_t queue);
void vQueueDelete(QueueHandle_t queue);

#endif
