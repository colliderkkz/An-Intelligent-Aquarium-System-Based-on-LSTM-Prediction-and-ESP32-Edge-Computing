#ifndef PIN_CONFIG_H
#define PIN_CONFIG_H

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/uart.h"
#include "hal/adc_types.h"
#include "hal/gpio_types.h"

// ==================== I2C ====================
#define I2C_MASTER_PORT             I2C_NUM_0
#define I2C_MASTER_SDA_IO           GPIO_NUM_8
#define I2C_MASTER_SCL_IO           GPIO_NUM_9
#define I2C_MASTER_FREQ_HZ          50000U
#define I2C_MASTER_TIMEOUT_MS       1000U
#define I2C_MASTER_SCL_WAIT_US      0U

#define PCF8563_I2C_ADDR            0x51
#define SGP30_I2C_ADDR              0x58
#define PCF8563T_ADDR               PCF8563_I2C_ADDR

// ==================== SPI ====================
#define SPI_SCK                     GPIO_NUM_12
#define SPI_MISO                    GPIO_NUM_11
#define SPI_MOSI                    GPIO_NUM_13
#define SPI_CS_SD_CARD              GPIO_NUM_10

#define SPI_HOST                    SPI2_HOST
#define SPI_CLOCK_SPEED_HZ          (10 * 1000 * 1000)
#define SPI_DMA_CHAN                SPI_DMA_CH_AUTO

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
// GPIO4  -> ADC1_3
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

// ==================== pH calibration ====================
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

#define GPIO_ACTIVE_HIGH            1
#define GPIO_ACTIVE_LOW             0

// ==================== PWM ====================
#define FEEDER_PIN                  GPIO_NUM_15
#define LIGHT_PIN                   GPIO_NUM_16

#define LEDC_TIMER                  LEDC_TIMER_0
#define LEDC_MODE                   LEDC_LOW_SPEED_MODE
#define LEDC_RESOLUTION             LEDC_TIMER_13_BIT
#define LEDC_FREQUENCY              50

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
#define VOICE_UART_NUM              UART_NUM_2
#define VOICE_UART_TX_PIN           GPIO_NUM_20
#define VOICE_UART_RX_PIN           GPIO_NUM_19
#define VOICE_UART_BAUD_RATE        9600
#define VOICE_UART_BUF_SIZE         1024

#endif // PIN_CONFIG_H
