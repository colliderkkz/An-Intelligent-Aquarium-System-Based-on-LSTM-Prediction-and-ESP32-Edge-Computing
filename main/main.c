#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "actuators/pump.h"
#include "actuators/uv_lamp.h"
#include "actuators/co2_valve.h"
#include "actuators/heater.h"
#include "bus/i2c_bus.h"
#include "communication/voice_control.h"
#include "config/pin_config.h"
#include "config/system_config.h"
#include "control/feeding_control.h"
#include "control/light_control.h"
#include "control/temp_control.h"
#include "control/water_control.h"
#include "drivers/pcf8563.h"
#include "drivers/sgp30.h"

static const char *TAG = "MAIN";

static i2c_bus_handle_t g_i2c_bus = NULL;

static void log_i2c_line_levels(void) {
    ESP_LOGI(TAG, "I2C line levels: SDA=%d SCL=%d",
             gpio_get_level(I2C_MASTER_SDA_IO), gpio_get_level(I2C_MASTER_SCL_IO));
}

static void log_expected_i2c_devices(i2c_bus_handle_t bus) {
    bool rtc_found = i2c_bus_probe(bus, PCF8563_I2C_ADDR);
    ESP_LOGI(TAG, "I2C probe 0x%02X (%s): %s",
             PCF8563_I2C_ADDR, "PCF8563", rtc_found ? "ACK" : "NO ACK");

    bool sgp30_found = i2c_bus_probe(bus, SGP30_I2C_ADDR);
    ESP_LOGI(TAG, "I2C probe 0x%02X (%s): %s",
             SGP30_I2C_ADDR, "SGP30", sgp30_found ? "ACK" : "NO ACK");
}

static bool is_minutes_in_window(int current_minutes, int start_minutes, int end_minutes) {
    if (start_minutes == end_minutes) {
        return false;
    }
    if (start_minutes < end_minutes) {
        return (current_minutes >= start_minutes) && (current_minutes < end_minutes);
    }
    return (current_minutes >= start_minutes) || (current_minutes < end_minutes);
}

static void feeding_control_task(void *pvParameters) {
    (void)pvParameters;

    while (1) {
        if (!g_i2c_bus) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        pcf8563_time_t now = {0};
        if (pcf8563_get_time(g_i2c_bus, PCF8563_I2C_ADDR, &now) == ESP_OK) {
            esp_err_t ret = feeding_control_update(&now);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Feeding control update failed: %s", esp_err_to_name(ret));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void light_control_task(void *pvParameters) {
    (void)pvParameters;

    while (1) {
        if (!g_i2c_bus) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        pcf8563_time_t now = {0};
        if (pcf8563_get_time(g_i2c_bus, PCF8563_I2C_ADDR, &now) == ESP_OK) {
            esp_err_t ret = light_control_update(&now);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Light control update failed: %s", esp_err_to_name(ret));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void uv_schedule_task(void *pvParameters) {
    (void)pvParameters;
    bool has_state = false;
    bool last_should_on = false;

    const int start_minutes = UV_LAMP_ON_HOUR * 60 + UV_LAMP_ON_MINUTE;
    const int end_minutes = UV_LAMP_OFF_HOUR * 60 + UV_LAMP_OFF_MINUTE;

    while (1) {
        if (!g_i2c_bus) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        pcf8563_time_t now = {0};
        if (pcf8563_get_time(g_i2c_bus, PCF8563_I2C_ADDR, &now) == ESP_OK) {
            int current_minutes = now.hour * 60 + now.minute;
            bool should_on = is_minutes_in_window(current_minutes, start_minutes, end_minutes);

            if (!has_state || (should_on != last_should_on)) {
                (void)uv_lamp_set_state(should_on);
                ESP_LOGI(TAG, "UV auto %s by schedule at %02d:%02d",
                         should_on ? "ON" : "OFF", now.hour, now.minute);
                last_should_on = should_on;
                has_state = true;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void temp_control_task(void *pvParameters) {
    (void)pvParameters;

    while (1) {
        esp_err_t ret = temp_control_update();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Temperature control update failed: %s", esp_err_to_name(ret));
        }
        vTaskDelay(pdMS_TO_TICKS(TASK_CYCLE_SENSOR));
    }
}

static void water_control_task(void *pvParameters) {
    (void)pvParameters;

    while (1) {
        esp_err_t ret = water_control_update();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Water control update failed: %s", esp_err_to_name(ret));
        }
        vTaskDelay(pdMS_TO_TICKS(TASK_CYCLE_CONTROL));
    }
}

static void co2_valve_boot_test(void) {
#if CO2_VALVE_BOOT_TEST_ENABLE
    ESP_LOGI(TAG, "CO2 valve boot test: ON");
    esp_err_t ret = co2_valve_set_state(true);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "CO2 valve boot test ON failed: %s", esp_err_to_name(ret));
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(CO2_VALVE_BOOT_TEST_MS));

    ret = co2_valve_set_state(false);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "CO2 valve boot test: OFF");
    } else {
        ESP_LOGW(TAG, "CO2 valve boot test OFF failed: %s", esp_err_to_name(ret));
    }
#endif
}

static void heater_boot_test(void) {
#if HEATER_BOOT_TEST_ENABLE
    ESP_LOGI(TAG, "Heater boot test: ON");
    esp_err_t ret = heater_set_state(true);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Heater boot test ON failed: %s", esp_err_to_name(ret));
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(HEATER_BOOT_TEST_MS));

    ret = heater_set_state(false);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Heater boot test: OFF");
    } else {
        ESP_LOGW(TAG, "Heater boot test OFF failed: %s", esp_err_to_name(ret));
    }
#endif
}

void app_main(void) {
    ESP_LOGI(TAG, "System startup");
    bool rtc_available = false;

    esp_err_t ret = nvs_flash_init();
    if ((ret == ESP_ERR_NVS_NO_FREE_PAGES) || (ret == ESP_ERR_NVS_NEW_VERSION_FOUND)) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    i2c_bus_config_t i2c_cfg = {
        .port = I2C_MASTER_PORT,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .clk_speed_hz = I2C_MASTER_FREQ_HZ,
        .pullup_enable = true,
        .timeout_ms = I2C_MASTER_TIMEOUT_MS,
        .scl_wait_us = I2C_MASTER_SCL_WAIT_US,
    };
    g_i2c_bus = i2c_bus_init(&i2c_cfg);
    if (!g_i2c_bus) {
        ESP_LOGE(TAG, "I2C bus init failed");
    } else {
        vTaskDelay(pdMS_TO_TICKS(3000));
        log_i2c_line_levels();
        log_expected_i2c_devices(g_i2c_bus);

        esp_err_t rtc_ret = pcf8563_init(g_i2c_bus, PCF8563_I2C_ADDR);
        if (rtc_ret != ESP_OK) {
            ESP_LOGW(TAG, "PCF8563 init failed: %s", esp_err_to_name(rtc_ret));
        } else {
            rtc_available = true;
        }

        esp_err_t sgp30_ret = sgp30_init(g_i2c_bus, SGP30_I2C_ADDR);
        if (sgp30_ret != ESP_OK) {
            ESP_LOGW(TAG, "SGP30 init failed: %s", esp_err_to_name(sgp30_ret));
        }
    }

    esp_err_t light_ctrl_ret = light_control_init();
    esp_err_t temp_ctrl_ret = temp_control_init();
    esp_err_t feed_ctrl_ret = feeding_control_init();
    esp_err_t water_ctrl_ret = water_control_init();
    esp_err_t voice_ctrl_ret = voice_control_init();
    esp_err_t pump_ret = pump_init();
    esp_err_t uv_ret = uv_lamp_init();

    if (light_ctrl_ret == ESP_OK) {
        co2_valve_boot_test();
    }
    if (temp_ctrl_ret == ESP_OK) {
        heater_boot_test();
    }

    if (pump_ret == ESP_OK) {
        (void)pump_set_state(true);
    } else {
        ESP_LOGW(TAG, "Pump init failed, circulation pump disabled");
    }

    if (voice_ctrl_ret == ESP_OK) {
        (void)voice_control_start();
    } else {
        ESP_LOGW(TAG, "Voice control init failed, voice commands disabled");
    }

    if (feed_ctrl_ret == ESP_OK && rtc_available) {
        xTaskCreate(feeding_control_task, "feeding_control", TASK_STACK_CONTROL, NULL,
                    TASK_PRIORITY_CONTROL, NULL);
    } else if (feed_ctrl_ret == ESP_OK) {
        ESP_LOGW(TAG, "Feeding control skipped, PCF8563 unavailable");
    } else {
        ESP_LOGW(TAG, "Feeding control init failed, auto feeding disabled");
    }

    if (light_ctrl_ret == ESP_OK && rtc_available) {
        xTaskCreate(light_control_task, "light_control", TASK_STACK_CONTROL, NULL,
                    TASK_PRIORITY_CONTROL, NULL);
    } else if (light_ctrl_ret == ESP_OK) {
        ESP_LOGW(TAG, "Light control skipped, PCF8563 unavailable");
    } else {
        ESP_LOGW(TAG, "Light control init failed, auto light/CO2 disabled");
    }

    if (temp_ctrl_ret == ESP_OK) {
        xTaskCreate(temp_control_task, "temp_control", TASK_STACK_CONTROL, NULL,
                    TASK_PRIORITY_CONTROL, NULL);
    } else {
        ESP_LOGW(TAG, "Temp control init failed, auto heating disabled");
    }

    if (water_ctrl_ret == ESP_OK) {
        xTaskCreate(water_control_task, "water_control", TASK_STACK_CONTROL, NULL,
                    TASK_PRIORITY_CONTROL, NULL);
    } else {
        ESP_LOGW(TAG, "Water control init failed, auto refill disabled");
    }

    if (uv_ret == ESP_OK && rtc_available) {
        xTaskCreate(uv_schedule_task, "uv_schedule", TASK_STACK_CONTROL, NULL,
                    TASK_PRIORITY_CONTROL, NULL);
    } else if (uv_ret == ESP_OK) {
        ESP_LOGW(TAG, "UV schedule skipped, PCF8563 unavailable");
    } else {
        ESP_LOGW(TAG, "UV lamp init failed, UV schedule disabled");
    }

    ESP_LOGI(TAG, "System initialization complete");
}
