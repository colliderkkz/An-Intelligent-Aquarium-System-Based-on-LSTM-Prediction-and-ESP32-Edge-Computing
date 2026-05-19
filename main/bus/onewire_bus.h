#ifndef ONEWIRE_BUS_H
#define ONEWIRE_BUS_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化单总线
 * 
 * @return esp_err_t 
 */
esp_err_t onewire_bus_init(void);

/**
 * @brief 复位单总线并检测设备存在脉冲
 * 
 * @return true 发现设备
 * @return false 未发现设备
 */
bool onewire_bus_reset(void);

/**
 * @brief 写入 1 位数据
 * 
 * @param bit 数据位 (0 或 1)
 */
void onewire_bus_write_bit(uint8_t bit);

/**
 * @brief 读取 1 位数据
 * 
 * @return uint8_t 读取到的位 (0 或 1)
 */
uint8_t onewire_bus_read_bit(void);

/**
 * @brief 写入 1 字节数据
 * 
 * @param data 要写入的字节
 */
void onewire_bus_write_byte(uint8_t data);

/**
 * @brief 读取 1 字节数据
 * 
 * @return uint8_t 读取到的字节
 */
uint8_t onewire_bus_read_byte(void);

#ifdef __cplusplus
}
#endif

#endif // ONEWIRE_BUS_H
