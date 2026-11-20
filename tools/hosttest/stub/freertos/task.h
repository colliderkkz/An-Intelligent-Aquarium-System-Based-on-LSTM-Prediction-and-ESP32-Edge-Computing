#ifndef STUB_FREERTOS_TASK_H
#define STUB_FREERTOS_TASK_H

#include "freertos/FreeRTOS.h"

typedef void (*TaskFunction_t)(void *);
typedef void *TaskHandle_t;

BaseType_t xTaskCreate(TaskFunction_t task, const char *name, uint32_t stack_depth, void *param,
                       UBaseType_t priority, TaskHandle_t *created_task);
void vTaskDelay(TickType_t ticks);
void vTaskDelete(TaskHandle_t task_to_delete);
TickType_t xTaskGetTickCount(void);
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task);

#endif
