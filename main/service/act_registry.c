#include "act_registry.h"

#include "actuators/air_pump.h"
#include "actuators/co2_valve.h"
#include "actuators/heater.h"
#include "actuators/light_actuator.h"
#include "actuators/pump.h"
#include "actuators/uv_lamp.h"
#include "actuators/water_pump.h"
#include <stddef.h>
#include <string.h>

/* ---- 灯的接口是 float 百分比，包一层统一成 uint8_t ---- */
static esp_err_t light_set_level(uint8_t pct) {
    return light_actuator_set_brightness((float)pct);
}

static uint8_t light_get_percent(void) {
    float b = light_actuator_get_brightness();
    if (b < 0.0f) {
        b = 0.0f;
    }
    if (b > 100.0f) {
        b = 100.0f;
    }
    return (uint8_t)(b + 0.5f);
}

static bool light_get_state(void) {
    return light_actuator_get_brightness() > 0.5f;
}

/* ==================== 执行器表 ====================
 * 想加执行器？在这里加一行就行，记得在对应 actuators 目录的实现文件里写
 * set_level / get_percent / get_state 三个函数。
 */
static const act_desc_t s_acts[] = {
    { "light",  "水草灯",  ACT_KIND_DIMMABLE, 10, 100,
      light_set_level, light_get_percent, light_get_state },

    { "pump",   "循环泵",  ACT_KIND_DIMMABLE, 10, 100,
      pump_set_level, pump_get_percent, pump_get_state },

    { "air",    "气泵",    ACT_KIND_DIMMABLE, 10, 100,
      air_pump_set_level, air_pump_get_percent, air_pump_get_state },

    { "refill", "补水泵",  ACT_KIND_DIMMABLE, 10, 100,
      water_pump_set_level, water_pump_get_percent, water_pump_get_state },

    { "co2",    "CO2 阀",  ACT_KIND_DIMMABLE, 10, 100,
      co2_valve_set_level, co2_valve_get_percent, co2_valve_get_state },

    { "uv",     "紫外灯",  ACT_KIND_DIMMABLE, 10, 100,
      uv_lamp_set_level, uv_lamp_get_percent, uv_lamp_get_state },

    { "heater", "加热",    ACT_KIND_DIMMABLE, 10, 100,
      heater_set_level, heater_get_percent, heater_get_state },
};

#define ACT_COUNT ((int)(sizeof(s_acts) / sizeof(s_acts[0])))

int act_registry_count(void) {
    return ACT_COUNT;
}

const act_desc_t *act_registry_at(int idx) {
    if (idx < 0 || idx >= ACT_COUNT) {
        return NULL;
    }
    return &s_acts[idx];
}

const act_desc_t *act_registry_find(const char *id) {
    if (!id || !id[0]) {
        return NULL;
    }
    for (int i = 0; i < ACT_COUNT; i++) {
        if (strcmp(s_acts[i].id, id) == 0) {
            return &s_acts[i];
        }
    }
    return NULL;
}
