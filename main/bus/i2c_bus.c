#include "i2c_bus.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "I2C_BUS";

typedef struct {
    uint8_t address;
    i2c_master_dev_handle_t handle;
} i2c_bus_device_slot_t;

struct i2c_bus_t {
    i2c_port_num_t port;
    i2c_master_bus_handle_t bus_handle;
    uint32_t clk_speed_hz;
    uint32_t timeout_ms;
    uint32_t scl_wait_us;
    int last_error;
    SemaphoreHandle_t lock;
    i2c_bus_device_slot_t devices[16];
    size_t device_count;
};

static SemaphoreHandle_t i2c_bus_create_lock(void) {
    SemaphoreHandle_t lock = xSemaphoreCreateMutex();
    if (lock == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
    }
    return lock;
}

static esp_err_t i2c_bus_lock(i2c_bus_handle_t bus, uint32_t timeout_ms) {
    TickType_t timeout = (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    if (xSemaphoreTake(bus->lock, timeout) != pdTRUE) {
        bus->last_error = I2C_ERR_TIMEOUT;
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static void i2c_bus_unlock(i2c_bus_handle_t bus) {
    xSemaphoreGive(bus->lock);
}

static bool i2c_bus_should_recover(esp_err_t ret) {
    return (ret == ESP_ERR_TIMEOUT) || (ret == ESP_ERR_INVALID_STATE);
}

static esp_err_t i2c_bus_reset_locked(i2c_bus_handle_t bus, const char *reason) {
    esp_err_t ret = i2c_master_bus_reset(bus->bus_handle);
    if (ret == ESP_OK) {
        ESP_LOGW(TAG, "I2C bus reset after %s", reason);
    } else {
        ESP_LOGE(TAG, "I2C bus reset failed after %s: %s", reason, esp_err_to_name(ret));
    }
    return ret;
}

static i2c_master_dev_handle_t i2c_bus_find_device(i2c_bus_handle_t bus, uint8_t dev_addr) {
    for (size_t i = 0; i < bus->device_count; i++) {
        if (bus->devices[i].address == dev_addr) {
            return bus->devices[i].handle;
        }
    }
    return NULL;
}

static esp_err_t i2c_bus_get_device(i2c_bus_handle_t bus, uint8_t dev_addr, i2c_master_dev_handle_t *out_dev) {
    if (!bus || !out_dev) {
        return ESP_ERR_INVALID_ARG;
    }

    i2c_master_dev_handle_t dev = i2c_bus_find_device(bus, dev_addr);
    if (dev != NULL) {
        *out_dev = dev;
        return ESP_OK;
    }

    if (bus->device_count >= (sizeof(bus->devices) / sizeof(bus->devices[0]))) {
        bus->last_error = I2C_ERR_INIT_FAIL;
        return ESP_ERR_NO_MEM;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = dev_addr,
        .scl_speed_hz = bus->clk_speed_hz,
        .scl_wait_us = bus->scl_wait_us,
    };

    esp_err_t ret = i2c_master_bus_add_device(bus->bus_handle, &dev_cfg, &dev);
    if (ret != ESP_OK) {
        bus->last_error = I2C_ERR_INIT_FAIL;
        return ret;
    }

    bus->devices[bus->device_count].address = dev_addr;
    bus->devices[bus->device_count].handle = dev;
    bus->device_count++;
    *out_dev = dev;
    return ESP_OK;
}

i2c_bus_handle_t i2c_bus_init(const i2c_bus_config_t *config) {
    if (!config) {
        ESP_LOGE(TAG, "Invalid config pointer");
        return NULL;
    }

    i2c_bus_handle_t bus = calloc(1, sizeof(struct i2c_bus_t));
    if (!bus) {
        ESP_LOGE(TAG, "Failed to allocate memory for I2C bus");
        return NULL;
    }

    bus->port = config->port;
    bus->clk_speed_hz = config->clk_speed_hz;
    bus->timeout_ms = config->timeout_ms;
    bus->scl_wait_us = config->scl_wait_us;
    bus->last_error = ESP_OK;
    bus->lock = i2c_bus_create_lock();
    if (!bus->lock) {
        free(bus);
        return NULL;
    }

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = config->port,
        .sda_io_num = config->sda_io_num,
        .scl_io_num = config->scl_io_num,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        // This project uses synchronous master transfers and device probing.
        // Keep the async transaction queue disabled to avoid probe incompatibility.
        .trans_queue_depth = 0,
        .flags.enable_internal_pullup = config->pullup_enable,
        .flags.allow_pd = 0,
    };

    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &bus->bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(ret));
        vSemaphoreDelete(bus->lock);
        free(bus);
        return NULL;
    }

    ESP_LOGI(TAG, "I2C%d initialized: SDA=%d, SCL=%d, speed=%lu Hz",
             bus->port, config->sda_io_num, config->scl_io_num, (unsigned long)config->clk_speed_hz);
    return bus;
}

esp_err_t i2c_bus_deinit(i2c_bus_handle_t bus) {
    if (!bus) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < bus->device_count; i++) {
        if (bus->devices[i].handle) {
            (void)i2c_master_bus_rm_device(bus->devices[i].handle);
        }
    }

    esp_err_t ret = i2c_del_master_bus(bus->bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_del_master_bus failed: %s", esp_err_to_name(ret));
    }

    if (bus->lock) {
        vSemaphoreDelete(bus->lock);
    }

    free(bus);
    return ret;
}

int i2c_bus_scan(i2c_bus_handle_t bus, uint8_t *addr_list, int max_count) {
    if (!bus || !addr_list || max_count <= 0) {
        return 0;
    }

    if (i2c_bus_lock(bus, bus->timeout_ms) != ESP_OK) {
        return 0;
    }

    int count = 0;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        if (i2c_master_probe(bus->bus_handle, addr, 50) == ESP_OK) {
            if (count < max_count) {
                addr_list[count] = addr;
            }
            count++;
        }
    }

    i2c_bus_unlock(bus);
    return count;
}

bool i2c_bus_probe(i2c_bus_handle_t bus, uint8_t dev_addr) {
    if (!bus) {
        return false;
    }

    if (i2c_bus_lock(bus, bus->timeout_ms) != ESP_OK) {
        return false;
    }

    esp_err_t ret = i2c_master_probe(bus->bus_handle, dev_addr, 50);
    if (i2c_bus_should_recover(ret)) {
        (void)i2c_bus_reset_locked(bus, "probe failure");
        ret = i2c_master_probe(bus->bus_handle, dev_addr, 50);
    }

    i2c_bus_unlock(bus);
    return ret == ESP_OK;
}

esp_err_t i2c_bus_read_reg8(i2c_bus_handle_t bus, uint8_t dev_addr,
                            uint8_t reg_addr, uint8_t *data, size_t len) {
    if (!bus || !data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = i2c_bus_lock(bus, bus->timeout_ms);
    if (ret != ESP_OK) {
        return ret;
    }

    i2c_master_dev_handle_t dev = NULL;
    ret = i2c_bus_get_device(bus, dev_addr, &dev);
    if (ret == ESP_OK) {
        for (int attempt = 0; attempt < 2; attempt++) {
            ret = i2c_master_transmit_receive(dev, &reg_addr, sizeof(reg_addr), data, len, (int)bus->timeout_ms);
            if (!i2c_bus_should_recover(ret) || (attempt > 0)) {
                break;
            }
            (void)i2c_bus_reset_locked(bus, "register read failure");
        }
    }

    i2c_bus_unlock(bus);

    if (ret != ESP_OK) {
        bus->last_error = I2C_ERR_READ_FAIL;
    }
    return ret;
}

esp_err_t i2c_bus_write_reg8(i2c_bus_handle_t bus, uint8_t dev_addr,
                             uint8_t reg_addr, uint8_t *data, size_t len) {
    if (!bus || !data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = i2c_bus_lock(bus, bus->timeout_ms);
    if (ret != ESP_OK) {
        return ret;
    }

    i2c_master_dev_handle_t dev = NULL;
    ret = i2c_bus_get_device(bus, dev_addr, &dev);
    if (ret == ESP_OK) {
        uint8_t stack_buf[33] = {0};
        uint8_t *write_buf = stack_buf;
        bool use_heap = len > (sizeof(stack_buf) - 1U);
        if (use_heap) {
            write_buf = malloc(len + 1U);
            if (write_buf == NULL) {
                ret = ESP_ERR_NO_MEM;
            }
        }

        if (ret == ESP_OK) {
            write_buf[0] = reg_addr;
            memcpy(&write_buf[1], data, len);
        }

        for (int attempt = 0; attempt < 2; attempt++) {
            if (ret != ESP_OK) {
                break;
            }
            ret = i2c_master_transmit(dev, write_buf, len + 1U, (int)bus->timeout_ms);
            if (!i2c_bus_should_recover(ret) || (attempt > 0)) {
                break;
            }
            (void)i2c_bus_reset_locked(bus, "register write failure");
        }

        if (use_heap) {
            free(write_buf);
        }
    }

    i2c_bus_unlock(bus);

    if (ret != ESP_OK) {
        bus->last_error = I2C_ERR_WRITE_FAIL;
    }
    return ret;
}

esp_err_t i2c_bus_read(i2c_bus_handle_t bus, uint8_t dev_addr,
                       uint8_t *data, size_t len) {
    if (!bus || !data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = i2c_bus_lock(bus, bus->timeout_ms);
    if (ret != ESP_OK) {
        return ret;
    }

    i2c_master_dev_handle_t dev = NULL;
    ret = i2c_bus_get_device(bus, dev_addr, &dev);
    if (ret == ESP_OK) {
        for (int attempt = 0; attempt < 2; attempt++) {
            ret = i2c_master_receive(dev, data, len, (int)bus->timeout_ms);
            if (!i2c_bus_should_recover(ret) || (attempt > 0)) {
                break;
            }
            (void)i2c_bus_reset_locked(bus, "raw read failure");
        }
    }

    i2c_bus_unlock(bus);

    if (ret != ESP_OK) {
        bus->last_error = I2C_ERR_READ_FAIL;
    }
    return ret;
}

esp_err_t i2c_bus_write(i2c_bus_handle_t bus, uint8_t dev_addr,
                        uint8_t *data, size_t len) {
    if (!bus || !data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = i2c_bus_lock(bus, bus->timeout_ms);
    if (ret != ESP_OK) {
        return ret;
    }

    i2c_master_dev_handle_t dev = NULL;
    ret = i2c_bus_get_device(bus, dev_addr, &dev);
    if (ret == ESP_OK) {
        for (int attempt = 0; attempt < 2; attempt++) {
            ret = i2c_master_transmit(dev, data, len, (int)bus->timeout_ms);
            if (!i2c_bus_should_recover(ret) || (attempt > 0)) {
                break;
            }
            (void)i2c_bus_reset_locked(bus, "raw write failure");
        }
    }

    i2c_bus_unlock(bus);

    if (ret != ESP_OK) {
        bus->last_error = I2C_ERR_WRITE_FAIL;
    }
    return ret;
}

esp_err_t i2c_bus_read_u16(i2c_bus_handle_t bus, uint8_t dev_addr,
                           uint8_t reg_addr, uint16_t *value) {
    if (!value) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t data[2] = {0};
    esp_err_t ret = i2c_bus_read_reg8(bus, dev_addr, reg_addr, data, sizeof(data));
    if (ret == ESP_OK) {
        *value = ((uint16_t)data[0] << 8) | data[1];
    }
    return ret;
}

esp_err_t i2c_bus_write_u16(i2c_bus_handle_t bus, uint8_t dev_addr,
                            uint8_t reg_addr, uint16_t value) {
    uint8_t data[2] = {
        (uint8_t)((value >> 8) & 0xFF),
        (uint8_t)(value & 0xFF),
    };
    return i2c_bus_write_reg8(bus, dev_addr, reg_addr, data, sizeof(data));
}

int i2c_bus_last_error(i2c_bus_handle_t bus) {
    return bus ? bus->last_error : -1;
}
