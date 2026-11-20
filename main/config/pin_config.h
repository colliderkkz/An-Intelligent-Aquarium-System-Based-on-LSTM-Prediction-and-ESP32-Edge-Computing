#ifndef PIN_CONFIG_H
#define PIN_CONFIG_H

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "hal/adc_types.h"
#include "hal/gpio_types.h"

/*
 * 板卡：ESP32-S3-WROOM-1 (N16R8) 核心板 + 自绘 4 层底板上
 * IO 依据底板丝印/连接器命名（见 鱼缸文档/io表.xlsx 与 BOM_2026-10-28.csv）：
 *
 *   连接器        丝印        功能                     ESP32-S3 引脚
 *   ----------------------------------------------------------------
 *   PCF8563       U10         RTC                      GPIO8(SDA)/GPIO9(SCL)
 *   CO2检测       CO2JINACE   SGP30 (I2C 4pin)         GPIO8/9
 *   光照          GUANGZHAO   光敏模块 (AO 4pin)       GPIO14 (ADC2_CH3)
 *   SD卡          U5          microSD (SPI)            CS10/SCK12/MISO11/MOSI13
 *   温控          TEMP1       DS18B20 (3pin)           GPIO4  (1-Wire)
 *   水位          SHUIWEI     水位传感器 (3pin)        GPIO5  (ADC1_CH4)
 *   tds           TDS         TDS 模块 (3pin)          GPIO2  (ADC1_CH1)
 *   浊度          ZHUODU      浊度模块 (3pin)          GPIO3  (ADC1_CH2)
 *   weishi        WEISHI      喂食舵机 (3pin PWM)      GPIO15
 *   电热          WARM        加热丝 (MOS 低边)        GPIO6
 *   水泵          SHUIBENG    循环/过滤泵 (MOS)        GPIO7
 *   吸水泵        JIASHUI     自动补水泵 (MOS)         GPIO17
 *   co2加气阀     DIANCIFA    CO2 电磁阀 (MOS)         GPIO18
 *   水草灯        DENG        水草灯 (MOS, PWM 调光)   GPIO16
 *   紫外          UAC         紫外灯 (MOS)             GPIO47
 *   气泵          QIBENG      气泵 (MOS)               GPIO38
 *
 * 备注（气泵 QIBENG）：
 *   BOM 中共 7 路 AO3400 低边开关，其中 6 路与 IO 表一一对应，第 7 路“气泵”
 *   在提供的 IO 表里被漏掉了。实测确认实际焊在 GPIO38，已按实物校正
 *   （AIR_PUMP_ENABLED=1 / AIR_PUMP_PIN=GPIO_NUM_38，见文件下方定义）。
 */

#define BOARD_NAME                  "ESP32-S3-N16R8 fish tank controller"

// ==================== I2C ====================
#define I2C_MASTER_PORT             I2C_NUM_0
#define I2C_MASTER_SDA_IO           GPIO_NUM_8
#define I2C_MASTER_SCL_IO           GPIO_NUM_9
#define I2C_MASTER_FREQ_HZ          50000
#define I2C_MASTER_TIMEOUT_MS       1000U

#define PCF8563_I2C_ADDR            0x51
#define SGP30_I2C_ADDR              0x58
#define PCF8563T_ADDR               PCF8563_I2C_ADDR

// ==================== SPI ====================
/* SD 卡 SPI 接线（已按实物确认）：
 *   SCK = GPIO12, MOSI = GPIO13, MISO = GPIO11, CS = GPIO10
 * 注意：这里和 ESP32-S3 SPI2 的 IOMUX 默认值(MOSI=11/MISO=13)是交叉的，
 * 板子实际就是这么接的，不能按默认值改。 */
#define SPI_SCK                     GPIO_NUM_12
#define SPI_MOSI                    GPIO_NUM_13
#define SPI_MISO                    GPIO_NUM_11
#define SPI_CS_SD_CARD              GPIO_NUM_10

#define SPI_HOST                    SPI2_HOST
/* SD 卡 SPI 时钟。400kHz 时 sdspi 初始化会长时间阻塞（把启动都拖死），
 * 10MHz 下只在 CMD8 阶段返回超时。现在挂载改到独立任务里做，
 * 用 10MHz 保证即使失败也不会卡住主循环。 */
#define SPI_CLOCK_SPEED_HZ          (10 * 1000 * 1000)
#define SPI_DMA_CHAN                SPI_DMA_CH_AUTO

#define SD_CARD_MOUNT_POINT         "/sdcard"

// ==================== OneWire ====================
// DS18B20 uses GPIO4. The board silk also marks this pin as ADC1_3 / TOUCH4.
#define ONE_WIRE_BUS                GPIO_NUM_4

// ==================== ADC ====================
#define ADC_UNIT_SENSOR             ADC_UNIT_1
#define ADC_UNIT_LIGHT_SENSOR       ADC_UNIT_2
#define ADC_WIDTH                   ADC_BITWIDTH_12
#define ADC_ATTEN                   ADC_ATTEN_DB_12

// ESP32-S3 ADC mapping from the board pinout image:
// GPIO1  -> ADC1_0
// GPIO2  -> ADC1_1
// GPIO3  -> ADC1_2
// GPIO4  -> ADC1_3 (used by OneWire / DS18B20)
// GPIO5  -> ADC1_4
// GPIO14 -> ADC2_3
#define PH_SENSOR_GPIO              GPIO_NUM_1      // ADC1_0
#define TDS_SENSOR_GPIO             GPIO_NUM_2      // ADC1_1
#define TURBIDITY_SENSOR_GPIO       GPIO_NUM_3      // ADC1_2
#define WATER_LEVEL_SENSOR_GPIO     GPIO_NUM_5      // ADC1_4
#define LIGHT_SENSOR_GPIO           GPIO_NUM_14     // ADC2_3

#define ADC1_CHANNEL_PH             ADC_CHANNEL_0   // GPIO1  / ADC1_0 / pH
#define ADC1_CHANNEL_TDS            ADC_CHANNEL_1   // GPIO2  / ADC1_1 / TDS
#define ADC1_CHANNEL_TURBIDITY      ADC_CHANNEL_2   // GPIO3  / ADC1_2 / turbidity
#define ADC1_CHANNEL_WATER_LEVEL    ADC_CHANNEL_4   // GPIO5  / ADC1_4 / water level
#define ADC2_CHANNEL_LIGHT          ADC_CHANNEL_3   // GPIO14 / ADC2_3 / light

// Backward-compatible aliases used by the current drivers.
#define ADC_CHANNEL_PH              ADC1_CHANNEL_PH
#define ADC_CHANNEL_TDS             ADC1_CHANNEL_TDS
#define ADC_CHANNEL_TURBIDITY       ADC1_CHANNEL_TURBIDITY
#define ADC_CHANNEL_WATER_LEVEL     ADC1_CHANNEL_WATER_LEVEL
#define ADC_CHANNEL_LIGHT           ADC2_CHANNEL_LIGHT

#define ADC_SAMPLE_COUNT            8
#define ADC_FILTER_DEPTH            10

/*
 * ADC 引脚的“贴轨”判别门限。
 * 传感器模块没插时，引脚通常悬空（读数乱跳/接近满量程）或对地短路（接近 0）。
 * 真实传感器输出不会正好压在 0 / 4095 上，低于 20（≈16mV）或高于 4075（≈3.28V）
 * 一律按“传感器未接入”处理，避免把 0V 换算成 pH 14.0 / 水温 0℃ 这种假值去触发动作。
 */
/* 贴轨门限：低于这个 raw 就认为探头没接（浮空脚读到的残压一般 30~80）。
 * 原来是 20，导致没接 pH 探头时 raw≈36~60 一路通过检查，算出并钳位成 14.00，
 * 还被标记为有效写进 CSV —— 假数据污染。抬到 100 后未接即判无效。 */
#define ADC_RAIL_LOW_RAW            100
#define ADC_RAIL_HIGH_RAW           4075

/*
 * 注意：ESP32-S3 的 ADC2 与 WiFi 共用硬件仲裁，WiFi 工作期间 ADC2 oneshot 读取
 * 会返回 ESP_ERR_TIMEOUT / ESP_ERR_INVALID_STATE。本机光照传感器落在 GPIO14
 * (ADC2_CH3)，因此：启用 WiFi/MQTT 时请把 LIGHT_SENSOR_ON_ADC2 置 1 并在
 * 读失败时容忍（本工程 light_sensor 已做重试 + 保持上次值）。
 */
#define LIGHT_SENSOR_ON_ADC2        1

// ==================== pH calibration (可在上位机 PARAM 中在线改) ====================
#define PH_ADC_REFERENCE_VOLTAGE    3.3f
#define PH_ADC_RESOLUTION           4095
#define PH_SLOPE                    (-5.887f)
#define PH_OFFSET                   21.677f
#define PH_MEDIAN_SAMPLE_COUNT      10
#define PH_TEMP_COMP_THRESHOLD      28.0f
#define PH_TEMP_COMP_HIGH           42.0f
#define PH_TEMP_COMP_MAX_ADJ        5

// ==================== GPIO outputs ====================
#define HEATER_PIN                  GPIO_NUM_6
#define PUMP_PIN                    GPIO_NUM_7
#define WATER_PUMP_PIN              GPIO_NUM_17
#define CO2_VALVE_PIN               GPIO_NUM_18
#define UV_LAMP_PIN                 GPIO_NUM_47

/*
 * 气泵（底板丝印 QIBENG，BOM 里第 7 路 AO3400 低边 MOS）。
 *
 * ⚠ 这一路在《io表.xlsx》里没有列出，原始代码也没分配引脚。
 *   实测确认实际焊在 GPIO38（已按实物校正，原先占位的 GPIO21 作废）。
 *
 *   GPIO38 在 ESP32-S3 上是普通 IO：
 *     - 不在 26~32（SPI Flash）也不在 33~37（Octal PSRAM，N16R8 用满）
 *     - 不在 39~42（JTAG / MTCK-MTMS）、45/46（strapping）、43/44（UART0）
 *   所以可以安全地做普通输出。
 */
#define AIR_PUMP_ENABLED            1
#define AIR_PUMP_PIN                GPIO_NUM_38

#define GPIO_ACTIVE_HIGH            1
#define GPIO_ACTIVE_LOW             0

// ==================== PWM ====================
// 喂食舵机（WEISHI 3pin）：50Hz / 13bit，0.5~2.5ms -> 205~1024
#define FEEDER_PIN                  GPIO_NUM_15
// 水草灯（DENG 2pin，经 AO3400 低边开关做 PWM 调光）：5kHz / 13bit
#define LIGHT_PIN                   GPIO_NUM_16

#define LEDC_TIMER_SERVO            LEDC_TIMER_0
#define LEDC_TIMER_LIGHT            LEDC_TIMER_1
#define LEDC_MODE                   LEDC_LOW_SPEED_MODE

#define LEDC_SERVO_RESOLUTION       LEDC_TIMER_13_BIT
#define LEDC_SERVO_FREQUENCY        50

#define LEDC_LIGHT_RESOLUTION       LEDC_TIMER_13_BIT
#define LEDC_LIGHT_FREQUENCY        5000

// 兼容旧代码的名字
#define LEDC_TIMER                  LEDC_TIMER_SERVO
#define LEDC_RESOLUTION             LEDC_SERVO_RESOLUTION
#define LEDC_FREQUENCY              LEDC_SERVO_FREQUENCY

#define SERVO_MAX_ANGLE             180.0f
#define SERVO_FEED_ANGLE            120.0f
#define SERVO_DUTY_MIN              205
#define SERVO_DUTY_MAX              1024

#define LED_MIN_DUTY                0
#define LED_MAX_DUTY                8191
#define LED_DEBUG_PIN               GPIO_NUM_48

// ==================== UART ====================
// ASRPRO cross wiring:
// Board TX -> GPIO19 (ESP RX)
// Board RX -> GPIO20 (ESP TX)
// Board pinout image:
// GPIO19 -> ADC2_8 / U1RTS / USB_D-
// GPIO20 -> ADC2_9 / U1CTS / USB_D+
//
// 注意：GPIO19/20 同时是 ESP32-S3 的 USB D+/D-，占用后板载 USB-CDC 不可用，
// 只能通过 UART0(CP2102/CH340) 口烧写与调试。
#define VOICE_UART_NUM              UART_NUM_2
/* 接线（实测）：ASRPRO PA6(TX) -> ESP GPIO20(RX)
 *               ASRPRO PA5(RX) <- ESP GPIO19(TX)
 * 注意：原来这里写反了，导致 ESP 一直听在 TX 脚上，永远收不到数据。 */
#define VOICE_UART_TX_PIN           GPIO_NUM_19
#define VOICE_UART_RX_PIN           GPIO_NUM_20
#define VOICE_UART_BAUD_RATE        9600
#define VOICE_UART_BUF_SIZE         1024

// ==================== 上位机调试串口 ====================
// 复用 UART0（板载 USB 转串口），与 ESP_LOG 共线：
//   协议行以 '@'(上行)/'#'(下行)/'!'(错误) 开头，日志行不带前缀，上位机自动忽略。
#define DEBUG_UART_NUM              UART_NUM_0
#define DEBUG_UART_BAUD_RATE        115200
#define DEBUG_UART_TX_PIN           GPIO_NUM_43
#define DEBUG_UART_RX_PIN           GPIO_NUM_44
#define DEBUG_UART_RX_BUF_SIZE      2048
/* TX 环形缓冲：给 0 的话 uart_write_bytes 会一直阻塞到 FIFO 发完，
 * 一行 60 字节日志就要 5ms、300 字节要 26ms，连 MQTT 事件任务都躲不掉。
 * 给 4KB 后写入变成本地 memcpy，发送交给 UART 中断。 */
#define DEBUG_UART_TX_BUF_SIZE      4096
#define DEBUG_UART_TASK_STACK       6144
#define DEBUG_UART_TASK_PRIORITY    3
// 上位机协议版本，改动协议时 +1
#define DEBUG_PROTOCOL_VERSION      1
/* 版本号字符串（READY/INFO 帧里显示）。
 * 改版本时要同时改这里 + system_config.h 的 MAJOR/MINOR/PATCH + 根 CMakeLists
 * 的 project(... VERSION ...)，三处保持一致。 */
#define FIRMWARE_VERSION_STRING     "1.1.1"

#endif // PIN_CONFIG_H
