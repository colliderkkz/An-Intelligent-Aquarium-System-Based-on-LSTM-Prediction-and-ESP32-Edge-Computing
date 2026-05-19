#ifndef PCF8563_H
#define PCF8563_H

#include "bus/i2c_bus.h"
#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t weekday;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
} pcf8563_time_t;

esp_err_t pcf8563_init(i2c_bus_handle_t bus, uint8_t dev_addr);
esp_err_t pcf8563_set_time(i2c_bus_handle_t bus, uint8_t dev_addr, const pcf8563_time_t *time);
esp_err_t pcf8563_get_time(i2c_bus_handle_t bus, uint8_t dev_addr, pcf8563_time_t *time);

#ifdef __cplusplus
}
#endif

#endif // PCF8563_H
