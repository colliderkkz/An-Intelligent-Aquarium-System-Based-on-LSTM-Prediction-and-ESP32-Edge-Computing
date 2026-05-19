#include "utils/queue_helper.h"

QueueHandle_t queue_helper_create(UBaseType_t length, UBaseType_t item_size) {
    return xQueueCreate(length, item_size);
}

BaseType_t queue_helper_send_latest(QueueHandle_t queue, const void *item, TickType_t ticks_to_wait) {
    if (!queue || !item) {
        return pdFALSE;
    }

    if (xQueueSend(queue, item, ticks_to_wait) == pdPASS) {
        return pdPASS;
    }

    xQueueReset(queue);
    return xQueueSend(queue, item, ticks_to_wait);
}

BaseType_t queue_helper_receive(QueueHandle_t queue, void *item, TickType_t ticks_to_wait) {
    if (!queue || !item) {
        return pdFALSE;
    }
    return xQueueReceive(queue, item, ticks_to_wait);
}
