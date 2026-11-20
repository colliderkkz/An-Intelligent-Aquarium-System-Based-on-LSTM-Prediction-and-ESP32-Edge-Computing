/* 主机语法检查用的 ESP-IDF 桩头文件（仅用于 cl.exe 语法检查，不参与真实构建） */
#ifndef STUB_ESP_ERR_H
#define STUB_ESP_ERR_H

#include <stdint.h>
#include <stddef.h>

typedef int esp_err_t;

#define ESP_OK                  0
#define ESP_FAIL                (-1)

#define ESP_ERR_NO_MEM          0x101
#define ESP_ERR_INVALID_ARG     0x102
#define ESP_ERR_INVALID_STATE   0x103
#define ESP_ERR_INVALID_SIZE    0x104
#define ESP_ERR_NOT_FOUND       0x105
#define ESP_ERR_NOT_SUPPORTED   0x106
#define ESP_ERR_TIMEOUT         0x107
#define ESP_ERR_INVALID_RESPONSE 0x108
#define ESP_ERR_INVALID_CRC     0x109
#define ESP_ERR_INVALID_VERSION 0x10A
#define ESP_ERR_INVALID_MAC     0x10B
#define ESP_ERR_NOT_FINISHED    0x10C

/* 注意：真实的 IDF 里 ESP_ERR_NVS_* 定义在 nvs.h，不在 esp_err.h。
 * 桩里也照此放在 nvs.h，避免主机检查漏掉 “忘了 include nvs.h” 的问题。 */

#define ESP_ERROR_CHECK(x) do { (void)(x); } while (0)

const char *esp_err_to_name(esp_err_t code);

#endif
