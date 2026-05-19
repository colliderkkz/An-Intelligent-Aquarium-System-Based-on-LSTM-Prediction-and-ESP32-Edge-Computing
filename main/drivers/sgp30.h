#ifndef SGP30_H
#define SGP30_H

#include "bus/i2c_bus.h"
#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SGP30_CMD_INIT_AIR_QUALITY     0x2003
#define SGP30_CMD_MEASURE_AIR_QUALITY  0x2008

typedef struct {
    uint16_t co2_eq;
    uint16_t tvoc;
} sgp30_data_t;

esp_err_t sgp30_init(i2c_bus_handle_t bus, uint8_t dev_addr);
esp_err_t sgp30_read_data(i2c_bus_handle_t bus, uint8_t dev_addr, sgp30_data_t *data);

#ifdef __cplusplus
}
#endif

#endif // SGP30_H
