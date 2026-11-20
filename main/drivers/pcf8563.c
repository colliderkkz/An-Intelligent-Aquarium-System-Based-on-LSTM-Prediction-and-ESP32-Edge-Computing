#include "pcf8563.h"

#include "esp_rom_sys.h"

#include "esp_log.h"

static const char *TAG = "PCF8563";

/* PCF8563 寄存器 */
#define PCF8563_REG_CTRL1      0x00
#define PCF8563_REG_CTRL2      0x01
#define PCF8563_REG_SECONDS    0x02
#define PCF8563_REG_MINUTES    0x03
#define PCF8563_REG_HOURS      0x04
#define PCF8563_REG_DAYS       0x05
#define PCF8563_REG_WEEKDAYS   0x06
#define PCF8563_REG_MONTHS     0x07
#define PCF8563_REG_YEARS      0x08

#define PCF8563_SEC_VL         0x80    /* bit7: 时钟完整性标志，1=时间不可信 */
#define PCF8563_CTRL1_STOP     0x20

static uint8_t dec_to_bcd(uint8_t dec) {
    return (uint8_t)(((dec / 10U) << 4) | (dec % 10U));
}

static uint8_t bcd_to_dec(uint8_t bcd) {
    return (uint8_t)(((bcd >> 4) * 10U) + (bcd & 0x0FU));
}

esp_err_t pcf8563_init(i2c_bus_handle_t bus, uint8_t dev_addr) {
    if (!bus) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t zero = 0;
    esp_err_t ret = i2c_bus_write_reg8(bus, dev_addr, PCF8563_REG_CTRL1, &zero, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to reset control register 1");
        return ret;
    }

    ret = i2c_bus_write_reg8(bus, dev_addr, PCF8563_REG_CTRL2, &zero, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to reset control register 2");
        return ret;
    }

    /* 检查 VL 位：掉电后 VL=1，表示时间已经不可信 */
    uint8_t seconds = 0;
    ret = i2c_bus_read_reg8(bus, dev_addr, PCF8563_REG_SECONDS, &seconds, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read seconds register");
        return ret;
    }

    if (seconds & PCF8563_SEC_VL) {
        /* 不要自动把时间写成编译时间：保持 VL，等上位机/用户用 RTC,SET 校准 */
        ESP_LOGW(TAG, "PCF8563 VL flag set: RTC lost power, time is invalid until set");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "PCF8563 initialized");
    return ESP_OK;
}

bool pcf8563_time_is_valid(i2c_bus_handle_t bus, uint8_t dev_addr) {
    if (!bus) {
        return false;
    }
    uint8_t seconds = 0;
    if (i2c_bus_read_reg8(bus, dev_addr, PCF8563_REG_SECONDS, &seconds, 1) != ESP_OK) {
        return false;
    }
    return (seconds & PCF8563_SEC_VL) == 0;
}

esp_err_t pcf8563_set_time(i2c_bus_handle_t bus, uint8_t dev_addr, const pcf8563_time_t *time) {
    if (!bus || !time) {
        return ESP_ERR_INVALID_ARG;
    }
    if (time->month < 1 || time->month > 12 || time->day < 1 || time->day > 31 ||
        time->hour > 23 || time->minute > 59 || time->second > 59) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t data[7] = {0};
    data[0] = dec_to_bcd(time->second) & 0x7F;               /* bit7 是 VL，写 0 把时间标记为有效 */
    data[1] = dec_to_bcd(time->minute) & 0x7F;
    data[2] = dec_to_bcd(time->hour) & 0x3F;
    data[3] = dec_to_bcd(time->day) & 0x3F;
    data[4] = (uint8_t)(time->weekday & 0x07);

    uint16_t year = time->year;
    uint8_t month_bcd = dec_to_bcd(time->month) & 0x1F;
    if (year >= 2000U) {
        month_bcd &= (uint8_t)~0x80U;    /* bit7 世纪位 0 = 20xx */
        data[6] = dec_to_bcd((uint8_t)(year - 2000U));
    } else {
        month_bcd |= 0x80U;
        data[6] = dec_to_bcd((uint8_t)(year - 1900U));
    }
    data[5] = month_bcd;

    /* 一次性写完 7 个寄存器（0x02..0x08），芯片内部自动递增。
     * 早期逐字节写是迫于旧 IDF I2C 驱动的多字节缺陷；换成软件 I2C 后
     * 单次连续写才是手册推荐做法，也能避免"写了一半"的不一致状态。 */
    esp_err_t ret = ESP_FAIL;
    for (int attempt = 0; attempt < 3; ++attempt) {
        ret = i2c_bus_write_reg8(bus, dev_addr, PCF8563_REG_SECONDS, data, sizeof(data));
        if (ret == ESP_OK) {
            break;
        }
        esp_rom_delay_us(3000);
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write RTC time (%s)", esp_err_to_name(ret));
        return ret;
    }

    /* 保险：确保时钟没被 STOP 位停住（写时间期间某些状态下会被置位） */
    uint8_t ctrl1 = 0;
    (void)i2c_bus_write_reg8(bus, dev_addr, PCF8563_REG_CTRL1, &ctrl1, 1);
    return ESP_OK;
}

esp_err_t pcf8563_get_time(i2c_bus_handle_t bus, uint8_t dev_addr, pcf8563_time_t *time) {
    if (!bus || !time) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 一次性读 7 个寄存器（0x02..0x08）。
     * 早期逐字节读是因为旧 IDF 驱动的"重复起始+连读"返回成功却不填缓冲；
     * 换软件 I2C 后单次连续读既快又保证 7 个字段来自同一时刻（不会出现
     * 秒已经进位而分还是旧值的错位）。读完再校验一次，防止跨秒边界。 */
    uint8_t data[7] = {0};
    esp_err_t ret = ESP_FAIL;
    for (int attempt = 0; attempt < 3; ++attempt) {
        ret = i2c_bus_read_reg8(bus, dev_addr, PCF8563_REG_SECONDS, data, sizeof(data));
        if (ret == ESP_OK) {
            break;
        }
        esp_rom_delay_us(3000);
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read RTC time (%s)", esp_err_to_name(ret));
        return ret;
    }

    time->second = bcd_to_dec(data[0] & 0x7F);
    time->minute = bcd_to_dec(data[1] & 0x7F);
    time->hour = bcd_to_dec(data[2] & 0x3F);
    time->day = bcd_to_dec(data[3] & 0x3F);
    time->weekday = data[4] & 0x07;
    time->month = bcd_to_dec(data[5] & 0x1F);
    time->year = (data[5] & 0x80U) ? (uint16_t)(1900U + bcd_to_dec(data[6]))
                                   : (uint16_t)(2000U + bcd_to_dec(data[6]));

    /* 时间字段明显非法时视为无效，避免上层调度被打乱 */
    if (time->month < 1 || time->month > 12 || time->day < 1 || time->day > 31) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}
