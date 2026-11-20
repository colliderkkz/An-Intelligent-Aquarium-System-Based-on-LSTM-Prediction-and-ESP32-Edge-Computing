# 尼特利灯的传输层二选一：
#   开蓝牙 -> NimBLE GATT 客户端（真灯）
#   关蓝牙 -> 内置模拟灯（没有实体灯也能跑通整条链路）
set(NETLEA_SRC "actuators/netlea_sim.c")
set(NETLEA_EXTRA_REQUIRES "")
if(CONFIG_BT_ENABLED)
    set(NETLEA_SRC "actuators/netlea_ble.c")
    set(NETLEA_EXTRA_REQUIRES bt)
    message(STATUS "netlea: BLE transport enabled")
else()
    message(STATUS "netlea: simulated lamp transport")
endif()

idf_component_register(
    SRCS
        "main.c"
        # ---- 总线 ----
        "bus/i2c_bus.c"
        "bus/onewire_bus.c"
        "bus/spi_bus.c"
        # ---- 驱动 ----
        "drivers/pcf8563.c"
        "drivers/sgp30.c"
        "drivers/sd_card_spi.c"
        "drivers/ds18b20.c"
        "drivers/adc/adc_manager.c"
        "drivers/adc/ph_sensor_adc.c"
        "drivers/adc/tds_sensor.c"
        "drivers/adc/turbidity.c"
        "drivers/adc/light_sensor.c"
        "drivers/adc/water_level.c"
        # ---- 传感器抽象 ----
        "sensors/co2_sensor.c"
        "sensors/light_sensor.c"
        "sensors/ph_sensor.c"
        "sensors/tds_sensor.c"
        "sensors/temp_sensor.c"
        "sensors/turbidity_sensor.c"
        "sensors/water_level.c"
        # ---- 执行器 ----
        "actuators/gpio_switch.c"
        "actuators/light_actuator.c"
        "actuators/pump.c"
        "actuators/water_pump.c"
        "actuators/heater.c"
        "actuators/co2_valve.c"
        "actuators/air_pump.c"
        "actuators/uv_lamp.c"
        "actuators/feeder.c"
        "actuators/netlea_lamp.c"
        ${NETLEA_SRC}
        # ---- 服务层 ----
        "service/sensor_hub.c"
        "service/power_diag.c"
        "service/act_hold.c"
        "service/act_registry.c"
        "service/history.c"
        "service/time_service.c"
        "service/control_mode.c"
        "service/schedule.c"
        "service/sd_logger.c"
        # ---- 控制逻辑 ----
        "control/temp_control.c"
        "control/light_control.c"
        "control/feeding_control.c"
        "control/water_control.c"
        "control/uv_control.c"
        # ---- 算法 ----
        "algorithm/data_fusion.c"
        "algorithm/feature_extraction.c"
        "algorithm/kalman_filter.c"
        "algorithm/outlier_detection.c"
        # ---- 工具 ----
        "utils/filter.c"
        "utils/queue_helper.c"
        "utils/timer_helper.c"
        # ---- 通信 ----
        "communication/uart_receiver.c"
        "communication/voice_control.c"
        # ---- 网络（WiFi + MQTT 远程访问）----
        "net/wifi_manager.c"
        "net/mqtt_app.c"
        "net/sntp_sync.c"
        # ---- 配置 ----
        "config/error_codes.c"
        "config/param_store.c"
        # ---- 上位机调试 ----
        "debug/debug_protocol.c"
        "debug/debug_commands.c"
        "debug/debug_console.c"
    INCLUDE_DIRS
        "."
    PRIV_REQUIRES
        driver
        esp_adc
        esp_app_format
        esp_timer
        fatfs
        sdmmc
        spi_flash
        nvs_flash
        esp_wifi
        wpa_supplicant
        esp_netif
        esp_event
        mqtt
        json
        lwip
        bt
        ${NETLEA_EXTRA_REQUIRES}
)

# 只对本工程代码开额外告警（不加 -Werror，避免 IDF 版本差异导致构建失败）
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter
    -Wno-sign-compare
)

# NimBLE 的 host 头文件不在 bt 组件的公共 include 里，这里显式补上
if(CONFIG_BT_ENABLED)
    set(NIMBLE_ROOT "${IDF_PATH}/components/bt/host/nimble/nimble")
    target_include_directories(${COMPONENT_LIB} PRIVATE
        "${NIMBLE_ROOT}/nimble/include"
        "${NIMBLE_ROOT}/nimble/host/include"
        "${NIMBLE_ROOT}/nimble/host/services/gap/include"
        "${NIMBLE_ROOT}/nimble/host/services/gatt/include"
        "${NIMBLE_ROOT}/porting/nimble/include"
        "${NIMBLE_ROOT}/porting/npl/freertos/include"
        "${NIMBLE_ROOT}/nimble/transport/include"
        "${IDF_PATH}/components/bt/host/nimble/port/include"
        "${IDF_PATH}/components/bt/host/nimble/esp-hci/include"
    )
endif()
