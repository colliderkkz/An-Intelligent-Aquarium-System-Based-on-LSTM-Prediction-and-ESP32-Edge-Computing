#ifndef QUEUE_HELPER_H
#define QUEUE_HELPER_H

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

QueueHandle_t queue_helper_create(UBaseType_t length, UBaseType_t item_size);
BaseType_t queue_helper_send_latest(QueueHandle_t queue, const void *item, TickType_t ticks_to_wait);
BaseType_t queue_helper_receive(QueueHandle_t queue, void *item, TickType_t ticks_to_wait);

#ifdef __cplusplus
}
#endif

#endif // QUEUE_HELPER_H
