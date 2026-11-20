#include "actuators/gpio_switch.h"

#include "config/pin_config.h"
#include "driver/ledc.h"
#include "esp_log.h"

static const char *TAG = "GPIO_SW";

/* ==================== PWM 映射 ====================
 * 通道 0/1 已被舵机和灯占用，这里用 2~7。
 * 定时器 2 = 1kHz（加热/CO2/紫外），定时器 3 = 20kHz（电机类，避开可闻频段）
 */
#define SW_PWM_BITS   10
#define SW_PWM_MAX    ((1u << SW_PWM_BITS) - 1u)

typedef struct {
    gpio_num_t pin;
    ledc_channel_t ch;
    ledc_timer_t tmr;
} sw_pwm_t;

static const sw_pwm_t s_pwm_map[] = {
    { HEATER_PIN,     LEDC_CHANNEL_2, LEDC_TIMER_2 },
    { CO2_VALVE_PIN,  LEDC_CHANNEL_3, LEDC_TIMER_2 },
    { UV_LAMP_PIN,    LEDC_CHANNEL_4, LEDC_TIMER_2 },
    { PUMP_PIN,       LEDC_CHANNEL_5, LEDC_TIMER_3 },
    { WATER_PUMP_PIN, LEDC_CHANNEL_6, LEDC_TIMER_3 },
    { AIR_PUMP_PIN,   LEDC_CHANNEL_7, LEDC_TIMER_3 },
};

static bool s_timers_ready = false;

static const sw_pwm_t *find_pwm(gpio_num_t pin) {
    for (size_t i = 0; i < sizeof(s_pwm_map) / sizeof(s_pwm_map[0]); i++) {
        if (s_pwm_map[i].pin == pin) {
            return &s_pwm_map[i];
        }
    }
    return NULL;
}

static esp_err_t pwm_timers_init(void) {
    if (s_timers_ready) {
        return ESP_OK;
    }
    ledc_timer_config_t t_slow = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_2,
        .freq_hz = 1000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config_t t_fast = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_3,
        .freq_hz = 20000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t r = ledc_timer_config(&t_slow);
    if (r != ESP_OK) {
        return r;
    }
    r = ledc_timer_config(&t_fast);
    if (r != ESP_OK) {
        return r;
    }
    s_timers_ready = true;
    return ESP_OK;
}


static int logical_to_level(const gpio_switch_t *sw, bool on) {
    bool invert = false;
    if (sw->invert_param != PARAM_IDX_COUNT) {
        invert = param_get_bool(sw->invert_param);
    }
    return (on != invert) ? 1 : 0;
}

esp_err_t gpio_switch_init(gpio_switch_t *sw) {
    if (!sw) {
        return ESP_ERR_INVALID_ARG;
    }

    const sw_pwm_t *pm = find_pwm(sw->pin);

    /* 先把输出锁存写成“关”，再切输出模式，避免上电/复位瞬间的误触发 */
    const int idle_level = logical_to_level(sw, false);
    (void)gpio_set_level(sw->pin, idle_level);

    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        /* BUGFIX: 原来用 GPIO_MODE_OUTPUT，这种模式下 gpio_get_level() 读的是
         * 输入寄存器，读不到输出锁存值，会一直返回 0 —— 上位机现场校验极性时
         * 就会看到“逻辑开但电平是 0”的假象。改成 INPUT_OUTPUT 才能真实回读。 */
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pin_bit_mask = (1ULL << sw->pin),
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
    };
    esp_err_t ret = pwm_timers_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "pwm timer init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if (pm) {
        /* 反相（invert=1）时"逻辑关"对应的是满占空比，不是 0。
         * 原来这里写死 duty=0：一旦有人把 inv_heater / inv_refill 打开，
         * 开机瞬间加热棒/补水泵就是满功率，而且 sw->percent 记的是 0，
         * 后面所有"关"都会被状态缓存早退吞掉 —— 超温联锁也关不掉。 */
        bool inv0 = false;
        if (sw->invert_param != PARAM_IDX_COUNT) {
            inv0 = param_get_bool(sw->invert_param);
        }
        const uint32_t idle_duty = inv0 ? SW_PWM_MAX : 0;
        ledc_channel_config_t cc = {
            .gpio_num = sw->pin,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = pm->ch,
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = pm->tmr,
            .duty = idle_duty,
            .hpoint = 0,
        };
        ret = ledc_channel_config(&cc);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "%s: ledc_channel_config(%d) failed: %s",
                     sw->name ? sw->name : "?", (int)sw->pin, esp_err_to_name(ret));
            return ret;
        }
        (void)ledc_update_duty(LEDC_LOW_SPEED_MODE, pm->ch);
        sw->state = false;
        sw->percent = 0;
        sw->initialized = true;
        ESP_LOGI(TAG, "%s PWM on GPIO %d (ch%d tmr%d, idle duty %u)",
                 sw->name ? sw->name : "?", (int)sw->pin,
                 (int)pm->ch, (int)pm->tmr, (unsigned)idle_duty);
        return ESP_OK;
    }

    ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "%s: gpio_config(%d) failed: %s",
                 sw->name ? sw->name : "?", (int)sw->pin, esp_err_to_name(ret));
        return ret;
    }

    (void)gpio_set_level(sw->pin, idle_level);
    sw->state = false;
    sw->percent = 0;
    sw->initialized = true;
    ESP_LOGI(TAG, "%s initialized on GPIO %d (idle level %d)",
             sw->name ? sw->name : "?", (int)sw->pin, idle_level);
    return ESP_OK;
}

esp_err_t gpio_switch_set_percent(gpio_switch_t *sw, uint8_t percent) {
    if (!sw) {
        return ESP_ERR_INVALID_ARG;
    }
    if (percent > 100) {
        percent = 100;
    }

    /* 注意：这里**不能**因为 percent == sw->percent 就早退。
     * 反相配置下"逻辑关"要写的是满占空比，早退会让第一次关断根本没写到硬件，
     * 超温联锁/超时保护全部失效。LEDC 写入只是两条寄存器操作，不差这点时间。 */

    const sw_pwm_t *pm = find_pwm(sw->pin);
    if (!pm) {
        /* 不在 PWM 表里：退化成开关量 */
        esp_err_t r = gpio_set_level(sw->pin, logical_to_level(sw, percent > 0));
        if (r == ESP_OK) {
            sw->state = (percent > 0);
            sw->percent = percent;
        }
        return r;
    }

    bool invert = false;
    if (sw->invert_param != PARAM_IDX_COUNT) {
        invert = param_get_bool(sw->invert_param);
    }
    uint32_t duty = (SW_PWM_MAX * (uint32_t)percent) / 100u;
    if (invert) {
        duty = SW_PWM_MAX - duty;
    }
    esp_err_t r = ledc_set_duty(LEDC_LOW_SPEED_MODE, pm->ch, duty);
    if (r == ESP_OK) {
        r = ledc_update_duty(LEDC_LOW_SPEED_MODE, pm->ch);
    }
    if (r == ESP_OK) {
        sw->percent = percent;
        sw->state = (percent > 0);
    }
    return r;
}

uint8_t gpio_switch_get_percent(const gpio_switch_t *sw) {
    return sw ? sw->percent : 0;
}

esp_err_t gpio_switch_set(gpio_switch_t *sw, bool on) {
    return gpio_switch_set_percent(sw, on ? 100 : 0);
}

bool gpio_switch_get(const gpio_switch_t *sw) {
    return sw ? sw->state : false;
}

int gpio_switch_read_level(const gpio_switch_t *sw) {
    if (!sw) {
        return -1;
    }
    return gpio_get_level(sw->pin);
}
