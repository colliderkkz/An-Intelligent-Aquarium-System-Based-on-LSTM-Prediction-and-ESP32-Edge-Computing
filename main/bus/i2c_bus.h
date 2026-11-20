#ifndef I2C_BUS_H
#define I2C_BUS_H

#include "driver/i2c_master.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define I2C_ERR_BASE               0x1000
#define I2C_ERR_INIT_FAIL          (I2C_ERR_BASE + 1)
#define I2C_ERR_READ_FAIL          (I2C_ERR_BASE + 2)
#define I2C_ERR_WRITE_FAIL         (I2C_ERR_BASE + 3)
#define I2C_ERR_DEV_NOT_FOUND      (I2C_ERR_BASE + 4)
#define I2C_ERR_TIMEOUT            (I2C_ERR_BASE + 5)
#define I2C_ERR_LOCK_FAIL          (I2C_ERR_BASE + 6)

typedef struct {
    i2c_port_num_t port;
    gpio_num_t sda_io_num;
    gpio_num_t scl_io_num;
    uint32_t clk_speed_hz;
    bool pullup_enable;
    uint32_t timeout_ms;
} i2c_bus_config_t;

typedef struct i2c_bus_t *i2c_bus_handle_t;

i2c_bus_handle_t i2c_bus_init(const i2c_bus_config_t *config);
esp_err_t i2c_bus_deinit(i2c_bus_handle_t bus);
int i2c_bus_scan(i2c_bus_handle_t bus, uint8_t *addr_list, int max_count);
bool i2c_bus_probe(i2c_bus_handle_t bus, uint8_t dev_addr);
esp_err_t i2c_bus_read_reg8(i2c_bus_handle_t bus, uint8_t dev_addr,
                            uint8_t reg_addr, uint8_t *data, size_t len);
esp_err_t i2c_bus_write_reg8(i2c_bus_handle_t bus, uint8_t dev_addr,
                             uint8_t reg_addr, uint8_t *data, size_t len);
esp_err_t i2c_bus_read(i2c_bus_handle_t bus, uint8_t dev_addr,
                       uint8_t *data, size_t len);
esp_err_t i2c_bus_write(i2c_bus_handle_t bus, uint8_t dev_addr,
                        uint8_t *data, size_t len);
esp_err_t i2c_bus_read_u16(i2c_bus_handle_t bus, uint8_t dev_addr,
                           uint8_t reg_addr, uint16_t *value);
esp_err_t i2c_bus_write_u16(i2c_bus_handle_t bus, uint8_t dev_addr,
                            uint8_t reg_addr, uint16_t value);
int i2c_bus_last_error(i2c_bus_handle_t bus);

#ifdef __cplusplus
}
#endif

#endif // I2C_BUS_H
