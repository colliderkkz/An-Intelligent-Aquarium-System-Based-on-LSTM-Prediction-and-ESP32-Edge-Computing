#include "debug/debug_commands.h"

#include "actuators/air_pump.h"
#include "actuators/co2_valve.h"
#include "actuators/feeder.h"
#include "actuators/heater.h"
#include "actuators/light_actuator.h"
#include "actuators/netlea_lamp.h"
#include "actuators/pump.h"
#include "actuators/uv_lamp.h"
#include "actuators/water_pump.h"
#include "config/error_codes.h"
#include "config/param_store.h"
#include "config/pin_config.h"
#include "config/system_config.h"
#include "debug/debug_protocol.h"
#include "driver/gpio.h"
#include "drivers/adc/adc_manager.h"
#include "drivers/adc/light_sensor.h"
#include "drivers/adc/ph_sensor_adc.h"
#include "drivers/adc/tds_sensor.h"
#include "drivers/adc/turbidity.h"
#include "drivers/adc/water_level.h"
#include "drivers/pcf8563.h"
#include "drivers/sd_card_spi.h"
#include "esp_app_desc.h"
#include "esp_flash.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "service/control_mode.h"
#include "net/mqtt_app.h"
#include "net/sntp_sync.h"
#include "net/wifi_manager.h"
#include "service/schedule.h"
#include "service/power_diag.h"
#include "communication/voice_control.h"
#include "service/sd_logger.h"
#include "service/sensor_hub.h"
#include "service/time_service.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "DEBUG_CMD";

static i2c_bus_handle_t s_bus = NULL;
static bool (*s_abort_check)(void) = NULL;

/* 遥测流（由 debug_console 轮询） */
static bool s_stream_enabled = false;
static uint32_t s_stream_period_ms = DEBUG_STREAM_PERIOD_MS;

/* ------------------------------------------------------------------ */
/* 小工具                                                              */
/* ------------------------------------------------------------------ */

static void emit(const dbg_io_t *io, dbg_out_t *o) {
    if (io && io->send_line && o) {
        io->send_line(io->ctx, dbg_out_str(o));
    }
}

static void emit_err(const dbg_io_t *io, esp_err_t err) {
    if (io && io->send_err) {
        io->send_err(io->ctx, app_err_name(err), app_err_desc(err));
    }
}

static void emit_err_text(const dbg_io_t *io, const char *code, const char *msg) {
    if (io && io->send_err) {
        io->send_err(io->ctx, code, msg);
    }
}

static bool arg_float(const dbg_fields_t *f, int idx, float *out) {
    const char *s = dbg_fields_at(f, idx);
    return (s != NULL) && dbg_parse_float(s, out);
}

static bool arg_bool(const dbg_fields_t *f, int idx, bool *out) {
    const char *s = dbg_fields_at(f, idx);
    return (s != NULL) && dbg_parse_bool(s, out);
}

static bool arg_u32(const dbg_fields_t *f, int idx, uint32_t *out) {
    const char *s = dbg_fields_at(f, idx);
    return (s != NULL) && dbg_parse_u32(s, out);
}

static bool parse_uint_auto(const char *s, uint32_t *out) {
    if (!s || !*s) {
        return false;
    }
    char *end = NULL;
    unsigned long v = 0;
    if ((s[0] == '0') && (s[1] == 'x' || s[1] == 'X')) {
        v = strtoul(s + 2, &end, 16);
    } else {
        v = strtoul(s, &end, 10);
    }
    if (end == s || (end && *end != '\0')) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

static const char *uptime_str(char *buf, size_t len) {
    uint32_t secs = (uint32_t)(esp_timer_get_time() / 1000000);
    snprintf(buf, len, "%lud%02lu:%02lu:%02lu",
             (unsigned long)(secs / 86400), (unsigned long)((secs % 86400) / 3600),
             (unsigned long)((secs % 3600) / 60), (unsigned long)(secs % 60));
    return buf;
}

static void kv_mac(dbg_out_t *o, const char *key, const uint8_t mac[6]) {
    char tmp[24];
    snprintf(tmp, sizeof(tmp), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    dbg_out_kv_str(o, key, tmp);
}

static void kv_hex8(dbg_out_t *o, const char *key, uint8_t value) {
    char tmp[8];
    snprintf(tmp, sizeof(tmp), "0x%02X", value);
    dbg_out_kv_str(o, key, tmp);
}

/* ------------------------------------------------------------------ */
/* 命令：PING / INFO / HELP                                            */
/* ------------------------------------------------------------------ */

static esp_err_t cmd_ping(const dbg_fields_t *f, const dbg_io_t *io) {
    (void)f;
    char buf[DEBUG_REPLY_MAX];
    dbg_out_t o;
    dbg_out_init(&o, buf, sizeof(buf), "PONG");
    dbg_out_kv_u(&o, "uptime_ms", (unsigned long)(esp_timer_get_time() / 1000));
    dbg_out_kv_str(&o, "mode", control_mode_name());
    dbg_out_kv_u(&o, "proto", DEBUG_PROTOCOL_VERSION);
    emit(io, &o);
    return ESP_OK;
}

static esp_err_t cmd_info(const dbg_fields_t *f, const dbg_io_t *io) {
    (void)f;
    char buf[DEBUG_REPLY_MAX];
    char up[24];
    dbg_out_t o;
    const esp_app_desc_t *app = esp_app_get_description();

    uint32_t flash_size = 0;
    (void)esp_flash_get_size(NULL, &flash_size);

    uint8_t mac[6] = {0};
    (void)esp_read_mac(mac, ESP_MAC_WIFI_STA);

    dbg_out_init(&o, buf, sizeof(buf), "INFO");
    dbg_out_kv_str(&o, "fw", app ? app->project_name : SYSTEM_NAME);
    dbg_out_kv_str(&o, "ver", app ? app->version : FIRMWARE_VERSION_STRING);
    dbg_out_kv_u(&o, "proto", DEBUG_PROTOCOL_VERSION);
    dbg_out_kv_str(&o, "idf", esp_get_idf_version());
    dbg_out_kv_str(&o, "board", BOARD_NAME);
    dbg_out_kv_u(&o, "flash_kb", (unsigned long)(flash_size / 1024));
    dbg_out_kv_u(&o, "heap", (unsigned long)esp_get_free_heap_size());
    dbg_out_kv_u(&o, "heap_min", (unsigned long)esp_get_minimum_free_heap_size());
    dbg_out_kv_str(&o, "uptime", uptime_str(up, sizeof(up)));
    dbg_out_kv_i(&o, "reset", (long)esp_reset_reason());
    dbg_out_kv_str(&o, "reset_name", power_diag_reset_reason_text());
    dbg_out_kv_u(&o, "boot_cnt", (unsigned long)power_diag_boot_count());
    dbg_out_kv_u(&o, "brownout", (unsigned long)power_diag_brownout_count());
    dbg_out_kv_u(&o, "poweron", (unsigned long)power_diag_poweron_count());
    dbg_out_kv_str(&o, "prev_reset", power_diag_last_reason_text());
    dbg_out_kv_str(&o, "mode", control_mode_name());
    dbg_out_kv_str(&o, "clock", time_service_source());
    kv_mac(&o, "mac", mac);
    emit(io, &o);
    return ESP_OK;
}

static esp_err_t cmd_help(const dbg_fields_t *f, const dbg_io_t *io);

/* ------------------------------------------------------------------ */
/* 命令：SENSORS / SENSOR                                              */
/* ------------------------------------------------------------------ */

static void emit_one_sensor(const dbg_io_t *io, sensor_id_t id, uint32_t now_ms) {
    const sensor_sample_t *s = sensor_hub_get(id);
    char buf[DEBUG_REPLY_MAX];
    dbg_out_t o;
    dbg_out_init(&o, buf, sizeof(buf), "SENSOR");
    dbg_out_kv_str(&o, "id", sensor_hub_id_name(id));
    dbg_out_kv_str(&o, "label", sensor_hub_label(id));
    if (s && s->valid) {
        dbg_out_kv_f(&o, "val", s->value, sensor_hub_decimals(id));
    } else {
        dbg_out_kv_str(&o, "val", "nan");
    }
    dbg_out_kv_str(&o, "unit", sensor_hub_unit(id));
    dbg_out_kv_str(&o, "status", sensor_hub_status_str(id, SENSOR_STALE_TIMEOUT_MS));
    dbg_out_kv_u(&o, "age_ms", s ? (unsigned long)(s->valid ? (now_ms - s->updated_ms) : 0) : 0);
    dbg_out_kv_i(&o, "raw", s ? s->raw : 0);
    dbg_out_kv_i(&o, "mv", s ? s->mv : 0);
    dbg_out_kv_u(&o, "period_ms", (unsigned long)sensor_hub_period_ms(id));
    dbg_out_kv_u(&o, "errs", s ? (unsigned long)s->err_count : 0);
    dbg_out_kv_str(&o, "err", s ? app_err_name(s->err) : "INIT");
    if (id == SH_CO2 && s) {
        dbg_out_kv_f(&o, "tvoc", s->value2, 0);
    }
    emit(io, &o);
}

static esp_err_t cmd_sensors(const dbg_fields_t *f, const dbg_io_t *io) {
    (void)f;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    for (int i = 0; i < SH_COUNT; i++) {
        emit_one_sensor(io, (sensor_id_t)i, now);
    }
    char buf[64];
    dbg_out_t o;
    dbg_out_init(&o, buf, sizeof(buf), "DONE");
    dbg_out_kv_u(&o, "count", SH_COUNT);
    emit(io, &o);
    return ESP_OK;
}

static esp_err_t cmd_sensor(const dbg_fields_t *f, const dbg_io_t *io) {
    const char *name = dbg_fields_at(f, 1);
    if (!name || name[0] == '\0') {
        emit_err_text(io, "BAD_ARG", "usage: SENSOR,<temp|ph|tds|turb|level|light|co2>[,fresh]");
        return APP_ERR_BAD_ARG;
    }
    if (dbg_streq_ci(name, "ALL") || dbg_streq_ci(name, "LIST")) {
        return cmd_sensors(f, io);
    }

    int idx = sensor_hub_find(name);
    if (idx < 0) {
        emit_err_text(io, "UNKNOWN_ID", "unknown sensor id");
        return APP_ERR_UNKNOWN_ID;
    }

    const char *mode = dbg_fields_at(f, 2);
    bool fresh = true;
    if (mode && dbg_streq_ci(mode, "cache")) {
        fresh = false;
    }

    esp_err_t ret = ESP_OK;
    if (fresh) {
        sensor_sample_t sample = {0};
        ret = sensor_hub_sample((sensor_id_t)idx, &sample);
    }
    emit_one_sensor(io, (sensor_id_t)idx, (uint32_t)(esp_timer_get_time() / 1000));
    return ret;
}

/* ------------------------------------------------------------------ */
/* 命令：ACT                                                           */
/* ------------------------------------------------------------------ */

typedef enum {
    ACT_HEATER = 0,
    ACT_PUMP,
    ACT_REFILL,
    ACT_CO2,
    ACT_UV,
    ACT_AIR,
    ACT_LIGHT,
    ACT_FEEDER,
    ACT_COUNT
} act_id_t;

static const char *const s_act_names[ACT_COUNT] = {
    "heater", "pump", "refill", "co2", "uv", "air", "light", "feeder"
};

static int act_find(const char *name) {
    for (int i = 0; i < ACT_COUNT; i++) {
        if (dbg_streq_ci(name, s_act_names[i])) {
            return i;
        }
    }
    return -1;
}

static bool act_state(act_id_t id) {
    switch (id) {
        case ACT_HEATER: return heater_get_state();
        case ACT_PUMP: return pump_get_state();
        case ACT_REFILL: return water_pump_get_state();
        case ACT_CO2: return co2_valve_get_state();
        case ACT_UV: return uv_lamp_get_state();
        case ACT_AIR: return air_pump_get_state();
        case ACT_LIGHT: return light_actuator_get_brightness() > 0.5f;
        case ACT_FEEDER: return feeder_get_angle() > 1.0f;
        default: return false;
    }
}

/** 引脚实际电平；灯和舵机是 PWM，返回 -1 表示不适用 */
static int act_level(act_id_t id) {
    switch (id) {
        case ACT_HEATER: return heater_get_level();
        case ACT_PUMP: return pump_get_level();
        case ACT_REFILL: return water_pump_get_level();
        case ACT_CO2: return co2_valve_get_level();
        case ACT_UV: return uv_lamp_get_level();
        case ACT_AIR: return air_pump_get_level();
        default: return -1;
    }
}

static esp_err_t act_set_bool(act_id_t id, bool on) {
    switch (id) {
        case ACT_HEATER: return heater_set_state(on);
        case ACT_PUMP: return pump_set_state(on);
        case ACT_REFILL: return water_pump_set_state(on);
        case ACT_CO2: return co2_valve_set_state(on);
        case ACT_UV: return uv_lamp_set_state(on);
        case ACT_AIR: return air_pump_set_state(on);
        case ACT_LIGHT: return light_actuator_set_brightness(on ? 100.0f : 0.0f);
        case ACT_FEEDER: return feeder_set_angle(on ? SERVO_FEED_ANGLE : 0.0f);
        default: return ESP_ERR_INVALID_ARG;
    }
}

static void emit_act_list(const dbg_io_t *io) {
    char buf[DEBUG_REPLY_MAX];
    dbg_out_t o;
    dbg_out_init(&o, buf, sizeof(buf), "ACT");
    dbg_out_kv_bool(&o, "heater", heater_get_state());
    dbg_out_kv_i(&o, "heater_lvl", heater_get_level());
    dbg_out_kv_bool(&o, "pump", pump_get_state());
    dbg_out_kv_i(&o, "pump_lvl", pump_get_level());
    dbg_out_kv_bool(&o, "refill", water_pump_get_state());
    dbg_out_kv_i(&o, "refill_lvl", water_pump_get_level());
    dbg_out_kv_bool(&o, "co2", co2_valve_get_state());
    dbg_out_kv_i(&o, "co2_lvl", co2_valve_get_level());
    dbg_out_kv_bool(&o, "uv", uv_lamp_get_state());
    dbg_out_kv_i(&o, "uv_lvl", uv_lamp_get_level());
    dbg_out_kv_bool(&o, "air", air_pump_get_state());
    dbg_out_kv_i(&o, "air_lvl", air_pump_get_level());
    dbg_out_kv_f(&o, "light", light_actuator_get_brightness(), 0);
    dbg_out_kv_u(&o, "light_duty", (unsigned long)light_actuator_get_duty());
    dbg_out_kv_f(&o, "feeder", feeder_get_angle(), 0);
    dbg_out_kv_u(&o, "feeder_duty", (unsigned long)feeder_get_duty());
    dbg_out_kv_str(&o, "mode", control_mode_name());
    emit(io, &o);
}

static esp_err_t cmd_act(const dbg_fields_t *f, const dbg_io_t *io) {
    const char *name = dbg_fields_at(f, 1);
    if (!name || name[0] == '\0' || dbg_streq_ci(name, "LIST") || dbg_streq_ci(name, "GET")) {
        emit_act_list(io);
        return ESP_OK;
    }

    /* ACT,FEED：手动触发一次喂食 */
    if (dbg_streq_ci(name, "feed") || (dbg_streq_ci(name, "feeder") && dbg_streq_ci(dbg_fields_at(f, 2) ? dbg_fields_at(f, 2) : "", "feed"))) {
        control_mode_enter_manual(0);
        esp_err_t ret = feeder_feed_now();
        char buf[96];
        dbg_out_t o;
        dbg_out_init(&o, buf, sizeof(buf), "OK");
        dbg_out_kv_str(&o, "action", "feed");
        dbg_out_kv_bool(&o, "ok", ret == ESP_OK);
        emit(io, &o);
        return ret;
    }

    const int id = act_find(name);
    if (id < 0) {
        emit_err_text(io, "UNKNOWN_ID", "unknown actuator (heater/pump/refill/co2/uv/air/light/feeder)");
        return APP_ERR_UNKNOWN_ID;
    }

    const char *value = dbg_fields_at(f, 2);
    if (!value) {
        emit_err_text(io, "BAD_ARG", "usage: ACT,<name>,<on|off|value>");
        return APP_ERR_BAD_ARG;
    }

    esp_err_t ret = ESP_FAIL;
    bool bool_value = false;
    float float_value = 0.0f;

    if (dbg_streq_ci(name, "light") || dbg_streq_ci(name, "feeder")) {
        /* 允许 ACT,LIGHT,on / ACT,LIGHT,50 / ACT,FEEDER,120 */
        if (dbg_parse_bool(value, &bool_value) && (dbg_streq_ci(value, "on") || dbg_streq_ci(value, "off") ||
                                                   dbg_streq_ci(value, "true") || dbg_streq_ci(value, "false"))) {
            ret = act_set_bool((act_id_t)id, bool_value);
        } else if (dbg_parse_float(value, &float_value)) {
            if (dbg_streq_ci(name, "light")) {
                ret = light_actuator_set_brightness(float_value);
            } else {
                ret = feeder_set_angle(float_value);
            }
        } else {
            emit_err_text(io, "BAD_ARG", "invalid value");
            return APP_ERR_BAD_ARG;
        }
    } else {
        if (!dbg_parse_bool(value, &bool_value)) {
            emit_err_text(io, "BAD_ARG", "value must be on/off/1/0");
            return APP_ERR_BAD_ARG;
        }
        ret = act_set_bool((act_id_t)id, bool_value);
    }

    if (ret != ESP_OK) {
        emit_err(io, ret);
        return ret;
    }

    control_mode_enter_manual(0);
    /* 光进"手动模式"还不够：温控/补水闭环刻意不看 manual（安全优先），
     * 它们只认 act_hold。不设 hold 的话 `ACT,heater,off` 两秒后就被恒温
     * 逻辑重新打开 —— 界面回"已关"，实际还在加热。
     * 与 MQTT / 语音两条路径保持一致：单路保持 120 秒。 */
    schedule_hold_manual(s_act_names[id], 120);

    char buf[DEBUG_REPLY_MAX];
    dbg_out_t o;
    dbg_out_init(&o, buf, sizeof(buf), "OK");
    dbg_out_kv_str(&o, "act", s_act_names[id]);
    if (dbg_streq_ci(name, "light")) {
        dbg_out_kv_f(&o, "light", light_actuator_get_brightness(), 0);
    } else if (dbg_streq_ci(name, "feeder")) {
        dbg_out_kv_f(&o, "feeder", feeder_get_angle(), 0);
    } else {
        dbg_out_kv_bool(&o, "state", act_state((act_id_t)id));
        dbg_out_kv_u(&o, "hold_s", 120);   /* 刚设的保持时长 */
    }
    dbg_out_kv_str(&o, "mode", control_mode_name());
    dbg_out_kv_u(&o, "manual_left_s", control_mode_manual_remaining_s());
    emit(io, &o);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 命令：PARAM                                                         */
/* ------------------------------------------------------------------ */

static void emit_param(const dbg_io_t *io, int idx) {
    if (idx < 0 || idx >= param_store_count()) {
        return;
    }
    const param_desc_t *d = param_store_desc(idx);
    char buf[DEBUG_REPLY_MAX];
    dbg_out_t o;
    dbg_out_init(&o, buf, sizeof(buf), "P");
    dbg_out_kv_str(&o, "name", d->name);
    dbg_out_kv_f(&o, "val", param_get((param_id_t)idx), d->decimals);
    dbg_out_kv_f(&o, "min", d->min_value, d->decimals);
    dbg_out_kv_f(&o, "max", d->max_value, d->decimals);
    dbg_out_kv_f(&o, "def", d->default_value, d->decimals);
    dbg_out_kv_str(&o, "unit", d->unit);
    dbg_out_kv_str(&o, "grp", d->group);
    dbg_out_kv_i(&o, "dec", (long)d->decimals);
    emit(io, &o);
}

static esp_err_t cmd_param(const dbg_fields_t *f, const dbg_io_t *io) {
    const char *sub = dbg_fields_at(f, 1);
    if (!sub) {
        emit_err_text(io, "BAD_ARG", "usage: PARAM,<LIST|GET|SET|SAVE|LOAD|RESET>,...");
        return APP_ERR_BAD_ARG;
    }

    if (dbg_streq_ci(sub, "LIST") || dbg_streq_ci(sub, "ALL")) {
        for (int i = 0; i < param_store_count(); i++) {
            emit_param(io, i);
        }
        char buf[96];
        dbg_out_t o;
        dbg_out_init(&o, buf, sizeof(buf), "DONE");
        dbg_out_kv_i(&o, "count", param_store_count());
        dbg_out_kv_bool(&o, "dirty", param_store_dirty());
        emit(io, &o);
        return ESP_OK;
    }

    if (dbg_streq_ci(sub, "SAVE")) {
        esp_err_t ret = param_store_save();
        char buf[96];
        dbg_out_t o;
        dbg_out_init(&o, buf, sizeof(buf), "OK");
        dbg_out_kv_str(&o, "action", "save");
        dbg_out_kv_bool(&o, "ok", ret == ESP_OK);
        emit(io, &o);
        if (ret != ESP_OK) {
            emit_err(io, ret);
        }
        return ret;
    }

    if (dbg_streq_ci(sub, "LOAD")) {
        esp_err_t ret = param_store_load();
        char buf[128];
        dbg_out_t o;
        dbg_out_init(&o, buf, sizeof(buf), "OK");
        dbg_out_kv_str(&o, "action", "load");
        dbg_out_kv_bool(&o, "ok", ret == ESP_OK || ret == ESP_ERR_NVS_NOT_FOUND);
        dbg_out_kv_str(&o, "result", app_err_name(ret));
        emit(io, &o);
        return ESP_OK;
    }

    if (dbg_streq_ci(sub, "RESET") || dbg_streq_ci(sub, "DEFAULT")) {
        esp_err_t ret = param_store_reset_defaults();
        char buf[96];
        dbg_out_t o;
        dbg_out_init(&o, buf, sizeof(buf), "OK");
        dbg_out_kv_str(&o, "action", "defaults");
        dbg_out_kv_bool(&o, "ok", ret == ESP_OK);
        dbg_out_kv_bool(&o, "dirty", param_store_dirty());
        emit(io, &o);
        return ret;
    }

    const char *name = dbg_fields_at(f, 2);
    if (!name || name[0] == '\0') {
        emit_err_text(io, "BAD_ARG", "missing parameter name");
        return APP_ERR_BAD_ARG;
    }
    int idx = param_store_find(name);
    if (idx < 0) {
        emit_err_text(io, "UNKNOWN_ID", "unknown parameter name");
        return APP_ERR_UNKNOWN_ID;
    }

    if (dbg_streq_ci(sub, "GET")) {
        emit_param(io, idx);
        return ESP_OK;
    }

    if (dbg_streq_ci(sub, "SET")) {
        float value = 0.0f;
        if (!arg_float(f, 3, &value)) {
            emit_err_text(io, "BAD_ARG", "usage: PARAM,SET,<name>,<value>");
            return APP_ERR_BAD_ARG;
        }
        esp_err_t ret = param_set((param_id_t)idx, value);
        if (ret != ESP_OK) {
            emit_err(io, ret);
            return ret;
        }
        const param_desc_t *d = param_store_desc(idx);
        char buf[DEBUG_REPLY_MAX];
        dbg_out_t o;
        dbg_out_init(&o, buf, sizeof(buf), "OK");
        dbg_out_kv_str(&o, "name", d->name);
        dbg_out_kv_f(&o, "val", param_get((param_id_t)idx), d->decimals);
        dbg_out_kv_bool(&o, "dirty", param_store_dirty());
        emit(io, &o);
        return ESP_OK;
    }

    emit_err_text(io, "BAD_ARG", "usage: PARAM,<LIST|GET|SET|SAVE|LOAD|RESET>,...");
    return APP_ERR_BAD_ARG;
}

/* ------------------------------------------------------------------ */
/* 命令：I2C                                                           */
/* ------------------------------------------------------------------ */

static esp_err_t cmd_i2c(const dbg_fields_t *f, const dbg_io_t *io) {
    if (!s_bus) {
        emit_err_text(io, "INVALID_STATE", "i2c bus not initialized");
        return APP_ERR_INVALID_STATE;
    }

    const char *sub = dbg_fields_at(f, 1);
    if (!sub || dbg_streq_ci(sub, "SCAN")) {
        uint8_t addrs[16] = {0};
        int count = i2c_bus_scan(s_bus, addrs, (int)sizeof(addrs));
        char buf[DEBUG_REPLY_MAX];
        dbg_out_t o;
        dbg_out_init(&o, buf, sizeof(buf), "I2C");
        dbg_out_kv_i(&o, "count", count);
        for (int i = 0; i < count; i++) {
            char key[16];
            snprintf(key, sizeof(key), "a%d", i);
            kv_hex8(&o, key, addrs[i]);
        }
        emit(io, &o);
        return ESP_OK;
    }

    if (dbg_streq_ci(sub, "READ")) {
        uint32_t addr = 0;
        uint32_t reg = 0;
        uint32_t len = 1;
        if (!parse_uint_auto(dbg_fields_at(f, 2), &addr) || !parse_uint_auto(dbg_fields_at(f, 3), &reg)) {
            emit_err_text(io, "BAD_ARG", "usage: I2C,READ,<addr>,<reg>[,len]");
            return APP_ERR_BAD_ARG;
        }
        (void)parse_uint_auto(dbg_fields_at(f, 4), &len);
        if (len == 0 || len > 32) {
            len = 1;
        }

        uint8_t data[32] = {0};
        esp_err_t ret = i2c_bus_read_reg8(s_bus, (uint8_t)addr, (uint8_t)reg, data, len);
        if (ret != ESP_OK) {
            emit_err(io, ret);
            return ret;
        }

        char buf[DEBUG_REPLY_MAX];
        dbg_out_t o;
        dbg_out_init(&o, buf, sizeof(buf), "I2C");
        kv_hex8(&o, "addr", (uint8_t)addr);
        kv_hex8(&o, "reg", (uint8_t)reg);
        dbg_out_kv_u(&o, "len", (unsigned long)len);
        /* 以十六进制串输出，如 data=51,02,1A */
        char hex[3 * 32 + 1];
        size_t pos = 0;
        for (uint32_t i = 0; i < len && pos + 3 < sizeof(hex); i++) {
            pos += (size_t)snprintf(hex + pos, sizeof(hex) - pos, "%02X", data[i]);
            if (i + 1 < len) {
                hex[pos++] = ',';
                hex[pos] = '\0';
            }
        }
        dbg_out_kv_str(&o, "data", hex);
        emit(io, &o);
        return ESP_OK;
    }

    if (dbg_streq_ci(sub, "WRITE")) {
        uint32_t addr = 0;
        uint32_t reg = 0;
        if (!parse_uint_auto(dbg_fields_at(f, 2), &addr) || !parse_uint_auto(dbg_fields_at(f, 3), &reg)) {
            emit_err_text(io, "BAD_ARG", "usage: I2C,WRITE,<addr>,<reg>,<byte>[...]");
            return APP_ERR_BAD_ARG;
        }
        uint8_t data[8] = {0};
        size_t len = 0;
        for (int i = 4; i < f->count && len < sizeof(data); i++) {
            uint32_t b = 0;
            if (!parse_uint_auto(dbg_fields_at(f, i), &b)) {
                break;
            }
            data[len++] = (uint8_t)b;
        }
        if (len == 0) {
            emit_err_text(io, "BAD_ARG", "no data bytes");
            return APP_ERR_BAD_ARG;
        }
        esp_err_t ret = i2c_bus_write_reg8(s_bus, (uint8_t)addr, (uint8_t)reg, data, len);
        char buf[128];
        dbg_out_t o;
        dbg_out_init(&o, buf, sizeof(buf), "OK");
        dbg_out_kv_str(&o, "action", "i2c_write");
        dbg_out_kv_bool(&o, "ok", ret == ESP_OK);
        dbg_out_kv_u(&o, "len", (unsigned long)len);
        emit(io, &o);
        if (ret != ESP_OK) {
            emit_err(io, ret);
        }
        return ret;
    }

    if (dbg_streq_ci(sub, "RAWREAD") || dbg_streq_ci(sub, "RR")) {
        /* 裸读：START, addr|R, 读 len 字节, STOP。
         * SGP30 这类"先写命令再裸读"的器件只能这么读。 */
        const char *a_s = dbg_fields_at(f, 2);
        const char *n_s = dbg_fields_at(f, 3);
        if (!a_s || !n_s) {
            emit_err_text(io, "BAD_ARG", "usage: I2C,RAWREAD,<addr>,<len>");
            return APP_ERR_BAD_ARG;
        }
        const uint8_t addr = (uint8_t)strtoul(a_s, NULL, 0);
        int len = atoi(n_s);
        if (len <= 0 || len > 32) {
            emit_err_text(io, "BAD_ARG", "len 1..32");
            return APP_ERR_BAD_ARG;
        }

        uint8_t raw[32] = {0};
        esp_err_t ret = i2c_bus_read(s_bus, addr, raw, (size_t)len);
        if (ret != ESP_OK) {
            emit_err(io, ret);
            return APP_ERR_IO;
        }

        char hex[100];
        size_t off = 0;
        for (int i = 0; i < len && off + 3 < sizeof(hex); ++i) {
            off += (size_t)snprintf(hex + off, sizeof(hex) - off, "%02X", raw[i]);
        }
        hex[off] = '\0';

        char buf[DEBUG_REPLY_MAX];
        dbg_out_t o;
        dbg_out_init(&o, buf, sizeof(buf), "I2C");
        dbg_out_kv_u(&o, "addr", (unsigned long)addr);
        dbg_out_kv_i(&o, "len", len);
        dbg_out_kv_str(&o, "hex", hex);
        emit(io, &o);
        return ESP_OK;
    }

    emit_err_text(io, "BAD_ARG", "usage: I2C,<SCAN|READ|WRITE>,...");
    return APP_ERR_BAD_ARG;
}

/* ------------------------------------------------------------------ */
/* 命令：RTC                                                           */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* 命令：SCH（执行器定时计划）                                          */
/* ------------------------------------------------------------------ */

static bool parse_hhmm(const char *s, uint16_t *out) {
    if (!s || !out) {
        return false;
    }
    if (strchr(s, ':')) {
        int h = 0;
        int m = 0;
        if (sscanf(s, "%d:%d", &h, &m) != 2) {
            return false;
        }
        if (h < 0 || h > 23 || m < 0 || m > 59) {
            return false;
        }
        *out = (uint16_t)(h * 60 + m);
        return true;
    }
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end && *end == '\0' && v >= 0 && v < 1440) {
        *out = (uint16_t)v;
        return true;
    }
    return false;
}

static void emit_sch_one(const dbg_io_t *io, int idx) {
    const sched_entry_t *e = schedule_get(idx);
    if (!e) {
        return;
    }
    char tbuf[8];
    char buf[DEBUG_REPLY_MAX];
    dbg_out_t o;
    schedule_format_minutes(e->start_min, tbuf, sizeof(tbuf));
    dbg_out_init(&o, buf, sizeof(buf), "SCH");
    dbg_out_kv_str(&o, "name", schedule_name(idx));
    dbg_out_kv_bool(&o, "en", e->enabled != 0);
    dbg_out_kv_str(&o, "start", tbuf);
    dbg_out_kv_i(&o, "start_min", e->start_min);
    dbg_out_kv_i(&o, "dur", e->dur_min);
    dbg_out_kv_i(&o, "pct", e->percent);
    dbg_out_kv_bool(&o, "active", schedule_is_active(idx));

    /* 当前是否处于计划窗口内 + 剩余/倒计时 （上位机用来显示“生效中 剩 320 分”） */
    pcf8563_time_t now = {0};
    time_service_now(&now);
    bool in_win = false;
    uint16_t remain_min = 0;
    uint16_t until_min = 0;
    schedule_window_info(idx, &now, &in_win, &remain_min, &until_min);
    dbg_out_kv_bool(&o, "in_window", in_win);
    dbg_out_kv_i(&o, "remain_min", remain_min);
    dbg_out_kv_i(&o, "until_start_min", until_min);

    emit(io, &o);
}

static void emit_sch_all(const dbg_io_t *io) {
    for (int i = 0; i < schedule_count(); ++i) {
        emit_sch_one(io, i);
    }
    char buf[DEBUG_REPLY_MAX];
    dbg_out_t o;
    dbg_out_init(&o, buf, sizeof(buf), "DONE");
    dbg_out_kv_i(&o, "count", schedule_count());
    emit(io, &o);
}

static esp_err_t cmd_sch(const dbg_fields_t *f, const dbg_io_t *io) {
    const char *sub = dbg_fields_at(f, 1);
    if (!sub || dbg_streq_ci(sub, "LIST")) {
        emit_sch_all(io);
        return ESP_OK;
    }

    if (dbg_streq_ci(sub, "SET")) {
        const char *name = dbg_fields_at(f, 2);
        int idx = -1;
        if (!name || !schedule_find(name, &idx)) {
            emit_err_text(io, "UNKNOWN_ID", "unknown target (light/air/pump/uv/co2)");
            return APP_ERR_UNKNOWN_ID;
        }
        const sched_entry_t *cur = schedule_get(idx);
        if (!cur) {
            emit_err_text(io, "INTERNAL", "bad index");
            return ESP_FAIL;
        }
        sched_entry_t e = *cur;

        const char *a_en = dbg_fields_at(f, 3);
        const char *a_start = dbg_fields_at(f, 4);
        const char *a_dur = dbg_fields_at(f, 5);
        const char *a_pct = dbg_fields_at(f, 6);

        if (a_en) {
            e.enabled = (dbg_streq_ci(a_en, "1") || dbg_streq_ci(a_en, "on") ||
                         dbg_streq_ci(a_en, "true") || dbg_streq_ci(a_en, "yes")) ? 1 : 0;
        }
        if (a_start) {
            uint16_t m = 0;
            if (!parse_hhmm(a_start, &m)) {
                emit_err_text(io, "BAD_ARG", "start must be HH:MM or minutes 0-1439");
                return APP_ERR_BAD_ARG;
            }
            e.start_min = m;
        }
        if (a_dur) {
            int v = atoi(a_dur);
            if (v < 1 || v > 1440) {
                emit_err_text(io, "BAD_ARG", "duration must be 1-1440 minutes");
                return APP_ERR_BAD_ARG;
            }
            e.dur_min = (uint16_t)v;
        }
        if (a_pct) {
            int v = atoi(a_pct);
            if (v < 0 || v > 100) {
                emit_err_text(io, "BAD_ARG", "percent must be 0-100");
                return APP_ERR_BAD_ARG;
            }
            e.percent = (uint8_t)v;
        }

        esp_err_t ret = schedule_set(idx, &e);
        if (ret != ESP_OK) {
            emit_err(io, ret);
            return ret;
        }
        emit_sch_one(io, idx);
        return ESP_OK;
    }

    if (dbg_streq_ci(sub, "SAVE")) {
        esp_err_t ret = schedule_save();
        if (ret != ESP_OK) {
            emit_err(io, ret);
            return ret;
        }
        char buf[DEBUG_REPLY_MAX];
        dbg_out_t o;
        dbg_out_init(&o, buf, sizeof(buf), "OK");
        dbg_out_kv_str(&o, "action", "sch_save");
        dbg_out_kv_i(&o, "saved", 1);
        emit(io, &o);
        return ESP_OK;
    }

    if (dbg_streq_ci(sub, "CLR") || dbg_streq_ci(sub, "RESET")) {
        schedule_reset_all();
        (void)schedule_save();
        emit_sch_all(io);
        return ESP_OK;
    }

    emit_err_text(io, "BAD_ARG", "usage: SCH,<LIST|SET|SAVE|CLR>,...");
    return APP_ERR_BAD_ARG;
}

/* ------------------------------------------------------------------ */
/* 命令：VOICE（ASRPRO 板载语音诊断）                                   */
/* ------------------------------------------------------------------ */

static esp_err_t cmd_voice(const dbg_fields_t *f, const dbg_io_t *io) {
    const char *sub = dbg_fields_at(f, 1);

    if (!sub || dbg_streq_ci(sub, "STATUS")) {
        char buf[DEBUG_REPLY_MAX];
        dbg_out_t o;
        dbg_out_init(&o, buf, sizeof(buf), "VOICE");
        dbg_out_kv_str(&o, "uart", "2");
        dbg_out_kv_str(&o, "tx_pin", "19");
        dbg_out_kv_str(&o, "rx_pin", "20");
        dbg_out_kv_i(&o, "baud", voice_control_baud());
        dbg_out_kv_i(&o, "started", voice_control_is_started() ? 1 : 0);
        dbg_out_kv_i(&o, "frames", (int)voice_control_rx_frames());
        dbg_out_kv_i(&o, "unmapped", (int)voice_control_unmapped());
        dbg_out_kv_i(&o, "sniff", voice_control_sniff_on() ? 1 : 0);
        dbg_out_kv_str(&o, "last_hex", voice_control_last_hex());
        dbg_out_kv_str(&o, "last_text", voice_control_last_text());
        emit(io, &o);
        return ESP_OK;
    }

    if (dbg_streq_ci(sub, "SNIFF")) {
        const char *v = dbg_fields_at(f, 2);
        bool on = !v || dbg_streq_ci(v, "1") || dbg_streq_ci(v, "on") || dbg_streq_ci(v, "true");
        voice_control_set_sniff(on);

        char buf[DEBUG_REPLY_MAX];
        dbg_out_t o;
        dbg_out_init(&o, buf, sizeof(buf), "OK");
        dbg_out_kv_str(&o, "action", "voice_sniff");
        dbg_out_kv_i(&o, "sniff", on ? 1 : 0);
        emit(io, &o);
        return ESP_OK;
    }

    if (dbg_streq_ci(sub, "SEND")) {
        const char *txt = dbg_fields_at(f, 2);
        if (!txt || !txt[0]) {
            emit_err_text(io, "BAD_ARG", "usage: VOICE,SEND,<text>");
            return APP_ERR_BAD_ARG;
        }
        esp_err_t ret = voice_control_send(txt);
        if (ret != ESP_OK) {
            emit_err(io, ret);
            return ret;
        }
        char buf[DEBUG_REPLY_MAX];
        dbg_out_t o;
        dbg_out_init(&o, buf, sizeof(buf), "OK");
        dbg_out_kv_str(&o, "action", "voice_send");
        dbg_out_kv_str(&o, "text", txt);
        emit(io, &o);
        return ESP_OK;
    }

    if (dbg_streq_ci(sub, "BAUD")) {
        const char *b = dbg_fields_at(f, 2);
        int v = b ? atoi(b) : 0;
        if (v < 1200 || v > 921600) {
            emit_err_text(io, "BAD_ARG", "baud must be 1200-921600");
            return APP_ERR_BAD_ARG;
        }
        esp_err_t ret = voice_control_set_baud(v);
        if (ret != ESP_OK) {
            emit_err(io, ret);
            return ret;
        }
        char buf[DEBUG_REPLY_MAX];
        dbg_out_t o;
        dbg_out_init(&o, buf, sizeof(buf), "OK");
        dbg_out_kv_str(&o, "action", "voice_baud");
        dbg_out_kv_i(&o, "baud", v);
        emit(io, &o);
        return ESP_OK;
    }

    if (dbg_streq_ci(sub, "EXEC")) {
        const char *c = dbg_fields_at(f, 2);
        int id = c ? (int)strtol(c, NULL, 0) : 0;
        if (id <= 0 || id > 0x1F) {
            emit_err_text(io, "BAD_ARG", "usage: VOICE,EXEC,<cmd 1-19>");
            return APP_ERR_BAD_ARG;
        }
        esp_err_t ret = voice_control_execute_command((uint8_t)id, "debug");
        if (ret != ESP_OK) {
            emit_err(io, ret);
            return ret;
        }
        char buf[DEBUG_REPLY_MAX];
        dbg_out_t o;
        dbg_out_init(&o, buf, sizeof(buf), "OK");
        dbg_out_kv_str(&o, "action", "voice_exec");
        dbg_out_kv_i(&o, "cmd", id);
        emit(io, &o);
        return ESP_OK;
    }

    emit_err_text(io, "BAD_ARG", "usage: VOICE,<STATUS|SNIFF|SEND|BAUD|EXEC>,...");
    return APP_ERR_BAD_ARG;
}

/* ------------------------------------------------------------------ */
/* 命令：SD（SD 卡记录状态）                                            */
/* ------------------------------------------------------------------ */

static void emit_sd_status(const dbg_io_t *io) {
    char buf[DEBUG_REPLY_MAX];
    dbg_out_t o;
    dbg_out_init(&o, buf, sizeof(buf), "SD");
    dbg_out_kv_bool(&o, "mounted", sd_card_is_mounted());
    dbg_out_kv_bool(&o, "active", sd_logger_is_active());
    dbg_out_kv_u(&o, "rows", (unsigned long)sd_logger_rows());
    dbg_out_kv_u(&o, "dropped", (unsigned long)sd_logger_dropped());
    dbg_out_kv_str(&o, "data", sd_logger_data_path());
    dbg_out_kv_str(&o, "log", sd_logger_log_path());
    /* BUGFIX: 不要在这里 stat()！这是另一个任务访问 FATFS，
     * 和记录任务的写入抢 VFS 锁，卡繁忙时直接触发看门狗
     * 复位（表现为 STATUS 后设备重启，READ 全部 INVALID_STATE）。
     * 文件大小改用 rows 估算，下载进度上位机只显示字节数。 */
    dbg_out_kv_u(&o, "rows", (unsigned long)sd_logger_rows());
    dbg_out_kv_str(&o, "mount_err", esp_err_to_name(sd_card_last_error()));
    dbg_out_kv_bool(&o, "card_seen", sd_card_was_detected());
    dbg_out_kv_str(&o, "mount_hint", sd_card_last_error_hint());
    dbg_out_kv_u(&o, "data_size", (unsigned long)sd_logger_data_size());
    dbg_out_kv_u(&o, "log_size", (unsigned long)sd_logger_log_size());
    dbg_out_kv_bool(&o, "stream", sd_logger_is_streaming());
    dbg_out_kv_u(&o, "csv_seq", (unsigned long)sd_logger_streamed_csv());
    dbg_out_kv_u(&o, "log_seq", (unsigned long)sd_logger_streamed_log());
    emit(io, &o);
}

/* ------------------------------------------------------------------ */
/* 命令：NET（WiFi / MQTT 远程访问配置）                                 */
/* ------------------------------------------------------------------ */

static void emit_net_status(const dbg_io_t *io) {
    char buf[DEBUG_REPLY_MAX];
    dbg_out_t o;
    dbg_out_init(&o, buf, sizeof(buf), "NET");
    dbg_out_kv_bool(&o, "wifi_up", net_wifi_up());
    dbg_out_kv_str(&o, "ip", net_ip());
    dbg_out_kv_str(&o, "ssid", net_get("ssid"));
    dbg_out_kv_i(&o, "wifi_n", net_wifi_count());
    dbg_out_kv_i(&o, "wifi_idx", net_wifi_active_index());
    dbg_out_kv_i(&o, "ent", net_get("ent")[0] == '1' ? 1 : 0);
    dbg_out_kv_str(&o, "uri", net_mqtt_uri());
    dbg_out_kv_str(&o, "prefix", net_prefix());
    dbg_out_kv_bool(&o, "mqtt", mqtt_app_connected());
    dbg_out_kv_str(&o, "mqtt_msg", mqtt_app_status());
    dbg_out_kv_str(&o, "mqtt_user", net_mqtt_user()[0] ? net_mqtt_user() : "-");
    dbg_out_kv_bool(&o, "ntp_ok", sntp_sync_ok());
    dbg_out_kv_str(&o, "ntp_msg", sntp_sync_status());
    emit(io, &o);
}

static const dbg_io_t *s_scan_io = NULL;

static void scan_emit_ap(const char *ssid, int rssi, int ch, int auth) {
    if (!s_scan_io) {
        return;
    }
    char buf[DEBUG_REPLY_MAX];
    dbg_out_t o;
    dbg_out_init(&o, buf, sizeof(buf), "WIFI");
    dbg_out_kv_str(&o, "ssid", ssid);
    dbg_out_kv_i(&o, "rssi", rssi);
    dbg_out_kv_i(&o, "ch", ch);
    dbg_out_kv_i(&o, "auth", auth);
    emit(s_scan_io, &o);
}

static esp_err_t cmd_net(const dbg_fields_t *f, const dbg_io_t *io) {
    const char *sub = dbg_fields_at(f, 1);
    if (!sub || dbg_streq_ci(sub, "STATUS")) {
        emit_net_status(io);
        return ESP_OK;
    }

    if (dbg_streq_ci(sub, "SCAN")) {
        char buf[DEBUG_REPLY_MAX];
        dbg_out_t o;
        s_scan_io = io;
        int total = net_scan(scan_emit_ap, 20);
        s_scan_io = NULL;
        if (total < 0) {
            /* 负数是错误码，绝不能塞进 DONE.count ——
             * 上位机会把它当"扫到 -2 个"或干脆当成功。 */
            emit_err_text(io, total == -2 ? "BUSY" : "IO",
                          total == -2 ? "wifi busy, retry in a moment" : "scan failed");
            return APP_ERR_IO;
        }
        dbg_out_init(&o, buf, sizeof(buf), "DONE");
        dbg_out_kv_i(&o, "count", total);
        emit(io, &o);
        return ESP_OK;
    }

    if (dbg_streq_ci(sub, "WIFI")) {
        const char *op = dbg_fields_at(f, 2);
        if (!op || dbg_streq_ci(op, "LIST")) {
            /* 每一条单独发一行，上位机好逐条渲染 */
            for (int i = 0; i < net_wifi_count(); i++) {
                char b[DEBUG_REPLY_MAX];
                dbg_out_t o;
                dbg_out_init(&o, b, sizeof(b), "WIFI");
                dbg_out_kv_str(&o, "ssid", net_wifi_ssid_at(i));
                dbg_out_kv_i(&o, "idx", i);
                dbg_out_kv_bool(&o, "active", net_wifi_active_index() == i);
                dbg_out_kv_i(&o, "ent", net_wifi_ent_at(i));
                dbg_out_kv_str(&o, "user", net_wifi_user_at(i));
                emit(io, &o);
            }
            char b[DEBUG_REPLY_MAX];
            dbg_out_t o;
            dbg_out_init(&o, b, sizeof(b), "OK");
            dbg_out_kv_str(&o, "action", "wifi_list");
            dbg_out_kv_i(&o, "count", net_wifi_count());
            emit(io, &o);
            return ESP_OK;
        }
        if (dbg_streq_ci(op, "ADD")) {
            /* 三种格式都支持：
             *   NET,WIFI,ADD,<ssid>,<password>                     普通 WiFi
             *   NET,WIFI,ADD,<ssid>,<ent>,[<user>],<password>      企业网(账号+密码)
             * 账号为空时中间那段可以省略（<ssid>,0,<password>），
             * 密码放在最后一个字段，允许带逗号（后面的字段都会拼回来）。 */
            const char *ssid = dbg_fields_at(f, 3);
            const char *a4 = dbg_fields_at(f, 4);
            const char *a5 = dbg_fields_at(f, 5);
            const char *a6 = dbg_fields_at(f, 6);
            if (!ssid || !ssid[0] || !a4) {
                emit_err_text(io, "BAD_ARG",
                              "usage: NET,WIFI,ADD,<ssid>,<pass> | ,<ssid>,<ent>,<user>,<pass>");
                return APP_ERR_BAD_ARG;
            }
            int ent = 0;
            char acct[64] = {0};
            char joined[128];
            int first_extra = 5;
            /* 普通 WiFi 的"账号"是空串。上位机若把中间这个空字段吞掉，
             * 命令就退化成 <ssid>,<ent>,<pass>。早期这里会把 ent 当成密码，
             * 存成 "0,123456788" —— 之后永远 4WAY_HANDSHAKE_TIMEOUT，
             * 表现就是"密码明明是对的，板子死活连不上"。这里兜住三段式。 */
            const bool a4_is_ent_flag = (a4[1] == '\0') && (a4[0] == '0' || a4[0] == '1');
            if (a6) {
                ent = (a4[0] == '1') ? 1 : 0;
                snprintf(acct, sizeof(acct), "%s", a5 ? a5 : "");
                snprintf(joined, sizeof(joined), "%s", a6);
                first_extra = 7;
            } else if (a5 && a4_is_ent_flag) {
                /* <ssid>,<ent>,<pass>：账号为空，ent 不能当密码用 */
                ent = (a4[0] == '1') ? 1 : 0;
                snprintf(joined, sizeof(joined), "%s", a5);
                first_extra = 6;
            } else {
                snprintf(joined, sizeof(joined), "%s", a4);
            }
            for (int i = first_extra; i < first_extra + 3; ++i) {
                const char *more = dbg_fields_at(f, i);
                if (!more) {
                    break;
                }
                strncat(joined, ",", sizeof(joined) - strlen(joined) - 1);
                strncat(joined, more, sizeof(joined) - strlen(joined) - 1);
            }
            /* 注意：这里故意不落盘。ADD 如果命中已有 SSID 就是"改密码"，
             * 密码打错又自动保存的话，板子重启后就再也连不上了（踩过）。
             * 需要保存请显式发 NET,SAVE（上位机面板会自动带上）。 */
            esp_err_t r = net_wifi_add_ex(ssid, joined, acct, ent);
            char b[DEBUG_REPLY_MAX];
            dbg_out_t o;
            dbg_out_init(&o, b, sizeof(b), "OK");
            dbg_out_kv_str(&o, "action", "wifi_add");
            dbg_out_kv_bool(&o, "ok", r == ESP_OK);
            dbg_out_kv_i(&o, "count", net_wifi_count());
            emit(io, &o);
            return r;
        }
        if (dbg_streq_ci(op, "DEL")) {
            /* 必须严格解析：空字段不是 NULL（切分保留空字段），
             * atoi("")=0 会把第 0 条凭据删掉，而主机紧接着 NET,SAVE，
             * 一次手滑就永久丢掉一条 WiFi。 */
            uint32_t idx = 0;
            if (!arg_u32(f, 3, &idx) || idx >= (uint32_t)net_wifi_count()) {
                emit_err_text(io, "BAD_ARG", "usage: NET,WIFI,DEL,<index 0..n-1>");
                return APP_ERR_BAD_ARG;
            }
            esp_err_t r = net_wifi_del((int)idx);   /* 同样由 NET,SAVE 显式落盘 */
            char b[DEBUG_REPLY_MAX];
            dbg_out_t o;
            dbg_out_init(&o, b, sizeof(b), "OK");
            dbg_out_kv_str(&o, "action", "wifi_del");
            dbg_out_kv_bool(&o, "ok", r == ESP_OK);
            dbg_out_kv_i(&o, "count", net_wifi_count());
            emit(io, &o);
            return r;
        }
        if (dbg_streq_ci(op, "SETENT")) {
            uint32_t ai = 0, av = 0;
            if (!arg_u32(f, 3, &ai) || !arg_u32(f, 4, &av) ||
                ai >= (uint32_t)net_wifi_count() || av > 1u) {
                emit_err_text(io, "BAD_ARG", "usage: NET,WIFI,SETENT,<index>,<0|1>");
                return APP_ERR_BAD_ARG;
            }
            esp_err_t r = net_wifi_set_ent((int)ai, (int)av);
            char b[DEBUG_REPLY_MAX];
            dbg_out_t o;
            dbg_out_init(&o, b, sizeof(b), "OK");
            dbg_out_kv_str(&o, "action", "wifi_setent");
            dbg_out_kv_bool(&o, "ok", r == ESP_OK);
            emit(io, &o);
            return r;
        }
        if (dbg_streq_ci(op, "SWAP")) {
            uint32_t a1 = 0, a2 = 0;
            if (!arg_u32(f, 3, &a1) || !arg_u32(f, 4, &a2) ||
                a1 >= (uint32_t)net_wifi_count() || a2 >= (uint32_t)net_wifi_count()) {
                emit_err_text(io, "BAD_ARG", "usage: NET,WIFI,SWAP,<a>,<b>");
                return APP_ERR_BAD_ARG;
            }
            esp_err_t r = net_wifi_swap((int)a1, (int)a2);
            char b[DEBUG_REPLY_MAX];
            dbg_out_t o;
            dbg_out_init(&o, b, sizeof(b), "OK");
            dbg_out_kv_str(&o, "action", "wifi_swap");
            dbg_out_kv_bool(&o, "ok", r == ESP_OK);
            emit(io, &o);
            return r;
        }
        if (dbg_streq_ci(op, "CLR")) {
            net_wifi_clear();
            esp_err_t r = net_save();
            char b[DEBUG_REPLY_MAX];
            dbg_out_t o;
            dbg_out_init(&o, b, sizeof(b), "OK");
            dbg_out_kv_str(&o, "action", "wifi_clear");
            dbg_out_kv_bool(&o, "ok", r == ESP_OK);
            emit(io, &o);
            return r;
        }
        if (dbg_streq_ci(op, "SCAN")) {
            /* 只列出"附近能看到、而且我已经记过密码"的网络，方便确认优先级 */
            int n = net_wifi_count();
            char b[DEBUG_REPLY_MAX];
            dbg_out_t o;
            dbg_out_init(&o, b, sizeof(b), "OK");
            dbg_out_kv_str(&o, "action", "wifi_scan");
            dbg_out_kv_i(&o, "known", n);
            emit(io, &o);
            return ESP_OK;
        }
        emit_err_text(io, "BAD_ARG", "usage: NET,WIFI,<LIST|ADD|DEL|CLR|SCAN>");
        return APP_ERR_BAD_ARG;
    }

    if (dbg_streq_ci(sub, "SET")) {
        const char *key = dbg_fields_at(f, 2);
        /* 值里可能带逗号（密码/URI），把剩余部分整体拼回去 */
        const char *val = dbg_fields_at(f, 3);
        if (!key || !val) {
            emit_err_text(io, "BAD_ARG", "usage: NET,SET,<ssid|user|pass|ent|uri|prefix>,<value>");
            return APP_ERR_BAD_ARG;
        }
        char joined[128];
        snprintf(joined, sizeof(joined), "%s", val);
        for (int i = 4; i < 8; ++i) {
            const char *more = dbg_fields_at(f, i);
            if (!more) {
                break;
            }
            strncat(joined, ",", sizeof(joined) - strlen(joined) - 1);
            strncat(joined, more, sizeof(joined) - strlen(joined) - 1);
        }
        esp_err_t ret = net_set(key, joined);
        if (ret != ESP_OK) {
            emit_err_text(io, "BAD_ARG", "unknown key");
            return APP_ERR_BAD_ARG;
        }
        emit_net_status(io);
        /* broker 地址 / 主题前缀 / MQTT 账号只在 mqtt_app_start() 时被读取，
         * 改完必须重启才生效。以前这里只回一帧状态，看起来像"已经生效"，
         * 结果手机 App 用旧前缀寻址、怎么都连不上还查不出原因。 */
        if (dbg_streq_ci(key, "uri") || dbg_streq_ci(key, "uri2") ||
            dbg_streq_ci(key, "prefix") || dbg_streq_ci(key, "mqtt_user") ||
            dbg_streq_ci(key, "mqtt_pass") || dbg_streq_ci(key, "mqttuser") ||
            dbg_streq_ci(key, "mqttpass")) {
            char b2[DEBUG_REPLY_MAX];
            dbg_out_t o2;
            dbg_out_init(&o2, b2, sizeof(b2), "OK");
            dbg_out_kv_str(&o2, "action", "net_set");
            dbg_out_kv_str(&o2, "key", key);
            dbg_out_kv_bool(&o2, "applied", false);
            dbg_out_kv_str(&o2, "need", "save+reboot");
            emit(io, &o2);
        }
        return ESP_OK;
    }

    if (dbg_streq_ci(sub, "SAVE")) {
        esp_err_t ret = net_save();
        if (ret != ESP_OK) {
            emit_err(io, ret);
            return ret;
        }
        char buf[DEBUG_REPLY_MAX];
        dbg_out_t o;
        dbg_out_init(&o, buf, sizeof(buf), "OK");
        dbg_out_kv_str(&o, "action", "net_save");
        dbg_out_kv_i(&o, "saved", 1);
        emit(io, &o);
        return ESP_OK;
    }

    if (dbg_streq_ci(sub, "CONNECT")) {
        (void)mqtt_app_start();
        /* 用异步版：net_connect() 要扫 AP + 逐个试凭据，最坏能卡 20 秒以上，
         * 而这期间串口任务完全不读命令、也不发遥测，上位机直接判"无响应"。
         * 现在只"受理"，真正的进度看 NET,STATUS / 后台日志。 */
        esp_err_t ret = net_connect_async();
        char b[DEBUG_REPLY_MAX];
        dbg_out_t o;
        dbg_out_init(&o, b, sizeof(b), "OK");
        dbg_out_kv_str(&o, "action", "net_connect");
        dbg_out_kv_bool(&o, "ok", ret == ESP_OK);
        dbg_out_kv_bool(&o, "accepted", ret == ESP_OK);
        dbg_out_kv_bool(&o, "pending", ret == ESP_OK);
        emit(io, &o);
        emit_net_status(io);
        return ESP_OK;
    }

    if (dbg_streq_ci(sub, "DISCONNECT")) {
        net_disconnect();
        char b[DEBUG_REPLY_MAX];
        dbg_out_t o;
        dbg_out_init(&o, b, sizeof(b), "OK");
        dbg_out_kv_str(&o, "action", "net_disconnect");
        dbg_out_kv_bool(&o, "stopped", true);
        dbg_out_kv_str(&o, "resume", "NET,CONNECT or reboot");
        emit(io, &o);
        emit_net_status(io);
        return ESP_OK;
    }

    if (dbg_streq_ci(sub, "PUB")) {
        mqtt_app_publish_now();
        char buf[DEBUG_REPLY_MAX];
        dbg_out_t o;
        dbg_out_init(&o, buf, sizeof(buf), "OK");
        dbg_out_kv_str(&o, "action", "mqtt_publish");
        emit(io, &o);
        return ESP_OK;
    }

    emit_err_text(io, "BAD_ARG",
                  "usage: NET,<STATUS|SET|SAVE|CONNECT|DISCONNECT|SCAN|PUB>");
    return APP_ERR_BAD_ARG;
}

static esp_err_t cmd_sd(const dbg_fields_t *f, const dbg_io_t *io) {
    const char *sub = dbg_fields_at(f, 1);
    if (!sub || dbg_streq_ci(sub, "STATUS") || dbg_streq_ci(sub, "GET")) {
        emit_sd_status(io);
        return ESP_OK;
    }
    if (dbg_streq_ci(sub, "STREAM")) {
        /* 实时推送：打开后每写入一行就发一帧 #SDLINE 给上位机 */
        const char *arg = dbg_fields_at(f, 2);
        const bool on = !(arg && (dbg_streq_ci(arg, "OFF") || dbg_streq_ci(arg, "0")));
        sd_logger_set_stream(on);

        char buf[DEBUG_REPLY_MAX];
        dbg_out_t o;
        dbg_out_init(&o, buf, sizeof(buf), "SD");
        dbg_out_kv_bool(&o, "stream", sd_logger_is_streaming());
        dbg_out_kv_bool(&o, "mounted", sd_card_is_mounted());
        dbg_out_kv_bool(&o, "active", sd_logger_is_active());
        emit(io, &o);
        return ESP_OK;
    }

    if (dbg_streq_ci(sub, "FLUSH")) {
        (void)sd_logger_flush();
        emit_sd_status(io);
        return ESP_OK;
    }

    if (dbg_streq_ci(sub, "PIN") || dbg_streq_ci(sub, "PINS")) {
        /* 已经挂载时绝对不能测：这 4 根线正被 SPI 外设用着，
         * 一旦改成 GPIO 输入，SPI 就再也驱动不了它们 —— 记录会静默停止
         * （任务以为还在写，卡上一行都没有），而且要断电重启才能恢复。 */
        if (sd_card_is_mounted()) {
            emit_err_text(io, "NOT_ALLOWED",
                          "SD is mounted; unplug/reformat first (PIN probe would steal SPI pins)");
            return APP_ERR_BAD_ARG;
        }
        /* 挂载失败时 SPI 总线已释放，这里把 4 根线临时配成输入读电平。
         * 期望（卡座正常、卡在位）：CS/MOSI/MISO 都是 1（上拉），CLK 任意。
         * MISO 恒为 0 -> 卡没响应或没供电；CS 恒为 0 -> 片选被拉死。 */
        const int pins[4] = { SPI_CS_SD_CARD, SPI_SCK, SPI_MOSI, SPI_MISO };
        const char *names[4] = { "cs", "sck", "mosi", "miso" };
        int levels[4] = { -1, -1, -1, -1 };

        for (int i = 0; i < 4; ++i) {
            (void)gpio_set_direction((gpio_num_t)pins[i], GPIO_MODE_INPUT);
            /* 开内部上拉：读到 1 = 引脚悬空(卡座没接/没供电)，
             * 读到 0 = 被外部强拉低(短路/下拉)。 */
            (void)gpio_set_pull_mode((gpio_num_t)pins[i], GPIO_PULLUP_ONLY);
        }
        esp_rom_delay_us(5000);
        for (int i = 0; i < 4; ++i) {
            levels[i] = gpio_get_level((gpio_num_t)pins[i]);
        }

        char buf[DEBUG_REPLY_MAX];
        dbg_out_t o;
        dbg_out_init(&o, buf, sizeof(buf), "SDPIN");
        for (int i = 0; i < 4; ++i) {
            dbg_out_kv_i(&o, names[i], levels[i]);
        }
        dbg_out_kv_str(&o, "note", "1=high 0=low (expect cs/mosi/miso high)");
        emit(io, &o);
        return ESP_OK;
    }

    emit_err_text(io, "BAD_ARG", "usage: SD,<STATUS|STREAM|FLUSH|PIN>");
    return APP_ERR_BAD_ARG;
}

static void emit_rtc(const dbg_io_t *io, bool rtc_ok_label) {
    pcf8563_time_t now = {0};
    char text[32];
    time_service_now(&now);
    time_service_format(&now, text, sizeof(text));

    char buf[DEBUG_REPLY_MAX];
    dbg_out_t o;
    dbg_out_init(&o, buf, sizeof(buf), "RTC");
    dbg_out_kv_str(&o, "time", text);
    dbg_out_kv_u(&o, "epoch", (unsigned long)time_service_epoch());
    dbg_out_kv_str(&o, "source", time_service_source());
    dbg_out_kv_bool(&o, "rtc_ok", time_service_rtc_ok());
    dbg_out_kv_i(&o, "wday", now.weekday);
    if (rtc_ok_label && s_bus) {
        dbg_out_kv_bool(&o, "rtc_chip", pcf8563_time_is_valid(s_bus, PCF8563_I2C_ADDR));
    }
    emit(io, &o);
}

static bool parse_date(const char *s, int *y, int *m, int *d) {
    if (!s) {
        return false;
    }
    int a = 0;
    int b = 0;
    int c = 0;
    if (sscanf(s, "%d-%d-%d", &a, &b, &c) == 3) {
        *y = a;
        *m = b;
        *d = c;
        return true;
    }
    return false;
}

static bool parse_clock(const char *s, int *hh, int *mm, int *ss) {
    if (!s) {
        return false;
    }
    int a = 0;
    int b = 0;
    int c = 0;
    int n = sscanf(s, "%d:%d:%d", &a, &b, &c);
    if (n >= 2) {
        *hh = a;
        *mm = b;
        *ss = (n == 3) ? c : 0;
        return true;
    }
    return false;
}

static esp_err_t cmd_rtc(const dbg_fields_t *f, const dbg_io_t *io) {
    const char *sub = dbg_fields_at(f, 1);
    if (!sub || dbg_streq_ci(sub, "GET")) {
        /* 不读芯片：这颗 PCF8563 的 I2C 读会时不时卡住，一卡这条命令就回不了帧，
         * 上位机时钟也就永远显示 --:--:--。快速路径只用软件时钟/NVS。 */
        emit_rtc(io, false);
        return ESP_OK;
    }

    if (dbg_streq_ci(sub, "CHIP")) {
        emit_rtc(io, true);
        return ESP_OK;
    }

    if (dbg_streq_ci(sub, "SET")) {
        pcf8563_time_t now = {0};
        time_service_now(&now);

        const char *date_str = dbg_fields_at(f, 2);
        const char *time_str = dbg_fields_at(f, 3);
        int y = now.year;
        int mo = now.month;
        int d = now.day;
        int hh = 0;
        int mi = 0;
        int ss = 0;

        if (date_str && strchr(date_str, '-')) {
            if (!parse_date(date_str, &y, &mo, &d)) {
                emit_err_text(io, "BAD_ARG", "date must be YYYY-MM-DD");
                return APP_ERR_BAD_ARG;
            }
            if (!parse_clock(time_str, &hh, &mi, &ss)) {
                emit_err_text(io, "BAD_ARG", "time must be HH:MM[:SS]");
                return APP_ERR_BAD_ARG;
            }
        } else if (date_str && strchr(date_str, ':')) {
            /* RTC,SET,21:47:31 只改时间 */
            if (!parse_clock(date_str, &hh, &mi, &ss)) {
                emit_err_text(io, "BAD_ARG", "time must be HH:MM[:SS]");
                return APP_ERR_BAD_ARG;
            }
        } else {
            emit_err_text(io, "BAD_ARG", "usage: RTC,SET,YYYY-MM-DD,HH:MM[:SS]");
            return APP_ERR_BAD_ARG;
        }

        pcf8563_time_t t = {
            .year = (uint16_t)y,
            .month = (uint8_t)mo,
            .day = (uint8_t)d,
            .hour = (uint8_t)hh,
            .minute = (uint8_t)mi,
            .second = (uint8_t)ss,
        };
        esp_err_t ret = time_service_set(&t);
        if (ret != ESP_OK) {
            /* 只发 !ERR。以前先发一帧 #RTC（新时间）再补 !ERR，
             * 上位机看到 #RTC 就认为成功并刷新界面，后面那条 !ERR
             * 还会被当成"下一条命令"的错误。 */
            emit_err(io, ret);
            return ret;
        }
        emit_rtc(io, true);
        return ESP_OK;
    }

    emit_err_text(io, "BAD_ARG", "usage: RTC,<GET|SET>,...");
    return APP_ERR_BAD_ARG;
}

/* ------------------------------------------------------------------ */
/* 命令：GPIO                                                          */
/* ------------------------------------------------------------------ */

/* ESP32-S3: GPIO26~32 接 SPI Flash，GPIO33~37 接 Octal PSRAM（N16R8），禁止操作。
 *
 * 另外所有"已经被外设占着"的引脚也必须挡住 —— 这是安全性上最关键的一条：
 * ESP32-S3 上 gpio_set_direction() 会把 IO_MUX 从外设功能切回普通 GPIO，
 * LEDC/UART/I2C/SPI 之后再也驱动不了这个引脚。也就是说
 * `GPIO,SET,6,1`（加热棒）能把它钉死在 100% 常开，随后 heater_set_state(false)、
 * 超温联锁、最长加热时间保护全都只写 LEDC 寄存器，物理上关不掉，只能断电重启。
 * 同理 43/44 是控制台串口，被夺走后连 REBOOT 都发不进来了。 */
static bool pin_is_reserved(int pin) {
    if (pin < 0 || pin > 48) {
        return true;
    }
    if (pin >= 22 && pin <= 37) {
        return true;                    /* Flash / PSRAM / S3 未引出 */
    }
    static const int reserved[] = {
        (int)DEBUG_UART_TX_PIN, (int)DEBUG_UART_RX_PIN,      /* 43 / 44 控制台 */
        (int)VOICE_UART_TX_PIN, (int)VOICE_UART_RX_PIN,      /* 19 / 20 语音 */
        (int)I2C_MASTER_SDA_IO, (int)I2C_MASTER_SCL_IO,      /* 8 / 9 全部 I2C 传感器 */
        (int)ONE_WIRE_BUS,                                   /* 4 水温 */
        (int)SPI_CS_SD_CARD, (int)SPI_SCK, (int)SPI_MISO, (int)SPI_MOSI,  /* 10~13 SD */
        (int)HEATER_PIN, (int)PUMP_PIN, (int)WATER_PUMP_PIN,              /* 6 / 7 / 17 */
        (int)CO2_VALVE_PIN, (int)UV_LAMP_PIN, (int)AIR_PUMP_PIN,          /* 18 / 47 / 38 */
        (int)FEEDER_PIN, (int)LIGHT_PIN,                                  /* 15 / 16 */
        (int)PH_SENSOR_GPIO, (int)TDS_SENSOR_GPIO, (int)TURBIDITY_SENSOR_GPIO,
        (int)WATER_LEVEL_SENSOR_GPIO, (int)LIGHT_SENSOR_GPIO,             /* ADC 五路 */
    };
    for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++) {
        if (pin == reserved[i]) {
            return true;
        }
    }
    return false;
}

static esp_err_t cmd_gpio(const dbg_fields_t *f, const dbg_io_t *io) {
    const char *sub = dbg_fields_at(f, 1);

    if (!sub || dbg_streq_ci(sub, "LS")) {
        char buf[DEBUG_REPLY_MAX];
        dbg_out_t o;
        dbg_out_init(&o, buf, sizeof(buf), "GPIO");
        static const int pins[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
                                   15, 16, 17, 18, 19, 20, 43, 44, 47, 48};
        for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
            char key[16];
            snprintf(key, sizeof(key), "p%d", pins[i]);
            dbg_out_kv_i(&o, key, gpio_get_level((gpio_num_t)pins[i]));
        }
        emit(io, &o);
        return ESP_OK;
    }

    uint32_t pin = 0;
    if (!arg_u32(f, 2, &pin) || pin_is_reserved((int)pin)) {
        emit_err_text(io, "BAD_ARG", "invalid or reserved pin");
        return APP_ERR_BAD_ARG;
    }

    if (dbg_streq_ci(sub, "GET")) {
        char buf[96];
        dbg_out_t o;
        dbg_out_init(&o, buf, sizeof(buf), "GPIO");
        dbg_out_kv_i(&o, "pin", (long)pin);
        dbg_out_kv_i(&o, "level", gpio_get_level((gpio_num_t)pin));
        emit(io, &o);
        return ESP_OK;
    }

    if (dbg_streq_ci(sub, "SET")) {
        bool level = false;
        if (!arg_bool(f, 3, &level)) {
            emit_err_text(io, "BAD_ARG", "usage: GPIO,SET,<pin>,<0|1>");
            return APP_ERR_BAD_ARG;
        }
        esp_err_t ret = gpio_set_direction((gpio_num_t)pin, GPIO_MODE_INPUT_OUTPUT);
        if (ret == ESP_OK) {
            ret = gpio_set_level((gpio_num_t)pin, level ? 1 : 0);
        }
        if (ret != ESP_OK) {
            /* 失败只发 !ERR：以前先发 #OK 再补 !ERR，一条命令两条回复，
             * 上位机的"下一条应答"会错位。 */
            emit_err(io, ret);
            return ret;
        }
        char buf[96];
        dbg_out_t o;
        dbg_out_init(&o, buf, sizeof(buf), "OK");
        dbg_out_kv_str(&o, "action", "gpio_set");
        dbg_out_kv_i(&o, "pin", (long)pin);
        dbg_out_kv_i(&o, "level", gpio_get_level((gpio_num_t)pin));
        emit(io, &o);
        return ESP_OK;
    }

    if (dbg_streq_ci(sub, "MODE")) {
        const char *mode = dbg_fields_at(f, 3);
        gpio_mode_t m = GPIO_MODE_INPUT;
        if (mode && dbg_streq_ci(mode, "out")) {
            m = GPIO_MODE_OUTPUT;
        } else if (mode && dbg_streq_ci(mode, "inout")) {
            m = GPIO_MODE_INPUT_OUTPUT;
        } else if (mode && dbg_streq_ci(mode, "od")) {
            m = GPIO_MODE_INPUT_OUTPUT_OD;
        }
        esp_err_t ret = gpio_set_direction((gpio_num_t)pin, m);
        if (ret != ESP_OK) {
            emit_err(io, ret);
            return ret;
        }
        char buf[96];
        dbg_out_t o;
        dbg_out_init(&o, buf, sizeof(buf), "OK");
        dbg_out_kv_str(&o, "action", "gpio_mode");
        dbg_out_kv_i(&o, "pin", (long)pin);
        dbg_out_kv_i(&o, "mode", (long)m);
        emit(io, &o);
        return ESP_OK;
    }

    emit_err_text(io, "BAD_ARG", "usage: GPIO,<LS|GET|SET|MODE>,...");
    return APP_ERR_BAD_ARG;
}

/* ------------------------------------------------------------------ */
/* 命令：ADC                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *id;
    adc_unit_t unit;
    adc_channel_t channel;
} adc_desc_t;

static const adc_desc_t s_adc[] = {
    {"ph", ADC_UNIT_1, ADC_CHANNEL_0},
    {"tds", ADC_UNIT_1, ADC_CHANNEL_1},
    {"turb", ADC_UNIT_1, ADC_CHANNEL_2},
    {"level", ADC_UNIT_1, ADC_CHANNEL_4},
    {"light", ADC_UNIT_2, ADC_CHANNEL_3},
};

static esp_err_t cmd_adc(const dbg_fields_t *f, const dbg_io_t *io) {
    const char *sub = dbg_fields_at(f, 1);
    const bool list_mode = (sub == NULL) || dbg_streq_ci(sub, "LIST") || dbg_streq_ci(sub, "LS");

    const char *want = NULL;
    if (!list_mode) {
        if (dbg_streq_ci(sub, "GET")) {
            want = dbg_fields_at(f, 2);
        } else {
            want = sub;   /* 支持 ADC,ph 这种简写 */
        }
        if (!want || want[0] == '\0') {
            emit_err_text(io, "BAD_ARG", "usage: ADC,<LIST|GET>,<ph|tds|turb|level|light>");
            return APP_ERR_BAD_ARG;
        }
    }

    int emitted = 0;
    for (size_t i = 0; i < sizeof(s_adc) / sizeof(s_adc[0]); i++) {
        if (want && !dbg_streq_ci(want, s_adc[i].id)) {
            continue;
        }

        int raw = 0;
        int mv = 0;
        esp_err_t ret = adc_manager_read(s_adc[i].unit, s_adc[i].channel, &raw, &mv);

        char buf[DEBUG_REPLY_MAX];
        dbg_out_t o;
        dbg_out_init(&o, buf, sizeof(buf), "ADC");
        dbg_out_kv_str(&o, "id", s_adc[i].id);
        dbg_out_kv_i(&o, "unit", (long)s_adc[i].unit + 1);
        dbg_out_kv_i(&o, "ch", (long)s_adc[i].channel);
        dbg_out_kv_bool(&o, "ok", ret == ESP_OK);
        dbg_out_kv_i(&o, "raw", raw);
        dbg_out_kv_i(&o, "mv", mv);
        dbg_out_kv_str(&o, "err", app_err_name(ret));
        emit(io, &o);
        emitted++;
    }

    if (want && emitted == 0) {
        emit_err_text(io, "UNKNOWN_ID", "unknown adc channel (ph/tds/turb/level/light)");
        return APP_ERR_UNKNOWN_ID;
    }

    if (list_mode) {
        char buf[64];
        dbg_out_t o;
        dbg_out_init(&o, buf, sizeof(buf), "DONE");
        dbg_out_kv_i(&o, "count", (long)(sizeof(s_adc) / sizeof(s_adc[0])));
        emit(io, &o);
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 命令：MODE / STREAM                                                 */
/* ------------------------------------------------------------------ */

static esp_err_t cmd_mode(const dbg_fields_t *f, const dbg_io_t *io) {
    const char *sub = dbg_fields_at(f, 1);

    if (sub && dbg_streq_ci(sub, "MANUAL")) {
        uint32_t timeout = 0;
        (void)arg_u32(f, 2, &timeout);
        control_mode_enter_manual(timeout);
    } else if (sub && dbg_streq_ci(sub, "AUTO")) {
        control_mode_exit_manual();
    } else if (sub && !dbg_streq_ci(sub, "GET")) {
        emit_err_text(io, "BAD_ARG", "usage: MODE,<GET|MANUAL[,s]|AUTO>");
        return APP_ERR_BAD_ARG;
    }

    char buf[DEBUG_REPLY_MAX];
    dbg_out_t o;
    dbg_out_init(&o, buf, sizeof(buf), "MODE");
    dbg_out_kv_str(&o, "mode", control_mode_name());
    dbg_out_kv_u(&o, "manual_left_s", control_mode_manual_remaining_s());
    dbg_out_kv_bool(&o, "auto_enable", control_mode_auto_enabled());
    dbg_out_kv_u(&o, "manual_timeout_s", param_get_u32(PARAM_IDX_MANUAL_TIMEOUT_S));
    emit(io, &o);
    return ESP_OK;
}

static esp_err_t cmd_stream(const dbg_fields_t *f, const dbg_io_t *io) {
    const char *sub = dbg_fields_at(f, 1);
    if (!sub) {
        emit_err_text(io, "BAD_ARG", "usage: STREAM,<ON[,ms]|OFF|NOW>");
        return APP_ERR_BAD_ARG;
    }

    if (dbg_streq_ci(sub, "ON")) {
        uint32_t period = 0;
        if (arg_u32(f, 2, &period) && period >= 100) {
            s_stream_period_ms = period;
        } else {
            s_stream_period_ms = param_get_u32(PARAM_IDX_STREAM_PERIOD_MS);
        }
        if (s_stream_period_ms < 100) {
            s_stream_period_ms = 100;
        }
        s_stream_enabled = true;
    } else if (dbg_streq_ci(sub, "OFF")) {
        s_stream_enabled = false;
    } else if (dbg_streq_ci(sub, "NOW")) {
        char frame[DEBUG_REPLY_MAX];
        debug_commands_build_telemetry(frame, sizeof(frame));
        if (io && io->send_line) {
            io->send_line(io->ctx, frame);
        }
        return ESP_OK;
    } else {
        emit_err_text(io, "BAD_ARG", "usage: STREAM,<ON[,ms]|OFF|NOW>");
        return APP_ERR_BAD_ARG;
    }

    char buf[128];
    dbg_out_t o;
    dbg_out_init(&o, buf, sizeof(buf), "OK");
    dbg_out_kv_str(&o, "action", "stream");
    dbg_out_kv_bool(&o, "enabled", s_stream_enabled);
    dbg_out_kv_u(&o, "period_ms", (unsigned long)s_stream_period_ms);
    emit(io, &o);
    return ESP_OK;
}

bool debug_commands_stream_enabled(void) {
    return s_stream_enabled;
}

uint32_t debug_commands_stream_period_ms(void) {
    return s_stream_period_ms;
}

/* ------------------------------------------------------------------ */
/* 命令：TEST / SELFTEST                                               */
/* ------------------------------------------------------------------ */

static bool abort_requested(void) {
    return (s_abort_check != NULL) && s_abort_check();
}

static esp_err_t cmd_test(const dbg_fields_t *f, const dbg_io_t *io) {
    const char *sub = dbg_fields_at(f, 1);
    if (!sub || !dbg_streq_ci(sub, "ACTUATORS")) {
        emit_err_text(io, "BAD_ARG", "usage: TEST,ACTUATORS[,<ms>]");
        return APP_ERR_BAD_ARG;
    }

    uint32_t step_ms = 800;
    (void)arg_u32(f, 2, &step_ms);
    if (step_ms < 100) {
        step_ms = 100;
    }
    if (step_ms > 5000) {
        step_ms = 5000;
    }

    control_mode_enter_manual(0);
    ESP_LOGW(TAG, "actuator self-test started, step=%lums", (unsigned long)step_ms);

    /* 先全部关断，避免多项同时动作 */
    (void)heater_set_state(false);
    (void)pump_set_state(false);
    (void)water_pump_set_state(false);
    (void)co2_valve_set_state(false);
    (void)uv_lamp_set_state(false);
    (void)air_pump_set_state(false);
    (void)light_actuator_set_brightness(0.0f);
    (void)feeder_set_angle(0.0f);

    bool aborted = false;
    for (int i = 0; i < ACT_COUNT; i++) {
        if (abort_requested()) {
            aborted = true;
            break;
        }

        esp_err_t ret;
        char buf[160];
        dbg_out_t o;

        if (i == ACT_LIGHT) {
            ret = light_actuator_set_brightness(50.0f);
        } else if (i == ACT_FEEDER) {
            ret = feeder_set_angle(P(PARAM_IDX_FEED_ANGLE));
        } else {
            ret = act_set_bool((act_id_t)i, true);
        }

        dbg_out_init(&o, buf, sizeof(buf), "TEST");
        dbg_out_kv_str(&o, "step", s_act_names[i]);
        dbg_out_kv_bool(&o, "on", true);
        dbg_out_kv_i(&o, "level", act_level((act_id_t)i));
        dbg_out_kv_bool(&o, "ok", ret == ESP_OK);
        emit(io, &o);

        vTaskDelay(pdMS_TO_TICKS(step_ms));

        if (i == ACT_LIGHT) {
            (void)light_actuator_set_brightness(0.0f);
        } else if (i == ACT_FEEDER) {
            (void)feeder_set_angle(0.0f);
        } else {
            (void)act_set_bool((act_id_t)i, false);
        }
        vTaskDelay(pdMS_TO_TICKS(150));
    }

    /* 测试结束回到自动模式：否则计划/温控会一直停在"手动暂停"里
     * （默认 120 秒），用户会以为刚才那下点动把自动控制弄坏了。 */
    control_mode_exit_manual();

    char buf[128];
    dbg_out_t o;
    dbg_out_init(&o, buf, sizeof(buf), "DONE");
    dbg_out_kv_str(&o, "test", "actuators");
    dbg_out_kv_bool(&o, "aborted", aborted);
    dbg_out_kv_str(&o, "mode", control_mode_name());
    emit(io, &o);
    return ESP_OK;
}

static void selftest_check(const dbg_io_t *io, const char *name, bool ok, const char *detail) {
    char buf[192];
    dbg_out_t o;
    dbg_out_init(&o, buf, sizeof(buf), "CHK");
    dbg_out_kv_str(&o, "name", name);
    dbg_out_kv_bool(&o, "ok", ok);
    dbg_out_kv_str(&o, "detail", detail ? detail : "");
    emit(io, &o);
}

static esp_err_t cmd_selftest(const dbg_fields_t *f, const dbg_io_t *io) {
    (void)f;
    int failures = 0;

    if (!s_bus) {
        selftest_check(io, "i2c_bus", false, "not initialized");
        failures++;
    } else {
        const bool pcf = i2c_bus_probe(s_bus, PCF8563_I2C_ADDR);
        const bool sgp = i2c_bus_probe(s_bus, SGP30_I2C_ADDR);
        selftest_check(io, "rtc_pcf8563", pcf, pcf ? "ack" : "no ack on 0x51");
        selftest_check(io, "co2_sgp30", sgp, sgp ? "ack" : "no ack on 0x58");
        failures += (pcf ? 0 : 1) + (sgp ? 0 : 1);
    }

    {
        sensor_sample_t sample = {0};
        esp_err_t ret = sensor_hub_sample(SH_TEMP, &sample);
        char detail[64];
        if (ret == ESP_OK) {
            snprintf(detail, sizeof(detail), "%.2f C", (double)sample.value);
        } else {
            snprintf(detail, sizeof(detail), "%s", app_err_name(ret));
        }
        selftest_check(io, "ds18b20", ret == ESP_OK, detail);
        failures += (ret == ESP_OK) ? 0 : 1;
    }

    for (int i = 0; i < SH_COUNT; i++) {
        if (i == SH_TEMP) {
            continue;
        }
        sensor_sample_t sample = {0};
        esp_err_t ret = sensor_hub_sample((sensor_id_t)i, &sample);
        char detail[80];
        if (ret == ESP_OK) {
            snprintf(detail, sizeof(detail), "raw=%d mv=%d", sample.raw, sample.mv);
        } else {
            snprintf(detail, sizeof(detail), "%s", app_err_name(ret));
        }
        selftest_check(io, sensor_hub_id_name((sensor_id_t)i), ret == ESP_OK, detail);
        failures += (ret == ESP_OK) ? 0 : 1;
    }

    selftest_check(io, "rtc_time", time_service_rtc_ok(),
                   time_service_rtc_ok() ? "rtc ok" : "software clock");
    selftest_check(io, "sdcard", sd_card_is_mounted(),
                   sd_card_is_mounted() ? SD_CARD_MOUNT_POINT : "not mounted");

    char buf[128];
    dbg_out_t o;
    dbg_out_init(&o, buf, sizeof(buf), "DONE");
    dbg_out_kv_str(&o, "test", "selftest");
    dbg_out_kv_i(&o, "failures", failures);
    emit(io, &o);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 命令：LOG / REBOOT / HEAP                                           */
/* ------------------------------------------------------------------ */

static esp_err_t cmd_log(const dbg_fields_t *f, const dbg_io_t *io) {
    const char *level = dbg_fields_at(f, 1);
    esp_log_level_t lv = ESP_LOG_INFO;
    if (level) {
        if (dbg_streq_ci(level, "none")) {
            lv = ESP_LOG_NONE;
        } else if (dbg_streq_ci(level, "error")) {
            lv = ESP_LOG_ERROR;
        } else if (dbg_streq_ci(level, "warn")) {
            lv = ESP_LOG_WARN;
        } else if (dbg_streq_ci(level, "info")) {
            lv = ESP_LOG_INFO;
        } else if (dbg_streq_ci(level, "debug")) {
            lv = ESP_LOG_DEBUG;
        } else if (dbg_streq_ci(level, "verbose")) {
            lv = ESP_LOG_VERBOSE;
        } else {
            emit_err_text(io, "BAD_ARG", "usage: LOG,<none|error|warn|info|debug|verbose>");
            return APP_ERR_BAD_ARG;
        }
    }
    esp_log_level_set("*", lv);

    char buf[64];
    dbg_out_t o;
    dbg_out_init(&o, buf, sizeof(buf), "OK");
    dbg_out_kv_str(&o, "action", "log_level");
    dbg_out_kv_i(&o, "level", (long)lv);
    emit(io, &o);
    return ESP_OK;
}

static esp_err_t cmd_reboot(const dbg_fields_t *f, const dbg_io_t *io) {
    (void)f;
    char buf[64];
    dbg_out_t o;
    dbg_out_init(&o, buf, sizeof(buf), "OK");
    dbg_out_kv_str(&o, "action", "reboot");
    emit(io, &o);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 命令表                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *verb;
    esp_err_t (*handler)(const dbg_fields_t *f, const dbg_io_t *io);
    const char *usage;
} dbg_cmd_t;

/* ------------------------------------------------------------------ */
/* 命令：LAMP —— 尼特利 BLE 灯 / 板载 PWM 灯 二选一                     */
/* ------------------------------------------------------------------ */

static void emit_lamp_status(const dbg_io_t *io) {
    netlea_state_t rep = {0};
    netlea_state_t tgt = {0};
    netlea_lamp_reported(&rep);
    netlea_lamp_target(&tgt);

    char buf[DEBUG_REPLY_MAX];
    dbg_out_t o;
    dbg_out_init(&o, buf, sizeof(buf), "LAMP");
    dbg_out_kv_i(&o, "src", lamp_source_get());
    dbg_out_kv_str(&o, "src_name", lamp_source_get() == LAMP_SRC_NETLEA ? "netlea" : "board");
    dbg_out_kv_str(&o, "transport", netlea_lamp_transport());
    dbg_out_kv_bool(&o, "ready", netlea_lamp_ready());
    dbg_out_kv_bool(&o, "on", rep.on);
    dbg_out_kv_i(&o, "bright", tgt.bright);
    dbg_out_kv_i(&o, "r", rep.r);
    dbg_out_kv_i(&o, "g", rep.g);
    dbg_out_kv_i(&o, "w", rep.w);
    dbg_out_kv_i(&o, "b", rep.b);
    dbg_out_kv_i(&o, "fan", rep.fan);
    dbg_out_kv_f(&o, "board_pwm", light_actuator_get_brightness(), 0);
    dbg_out_kv_str(&o, "mac", netlea_lamp_mac_str());
    emit(io, &o);
}

static esp_err_t cmd_lamp(const dbg_fields_t *f, const dbg_io_t *io) {
    const char *sub = dbg_fields_at(f, 1);
    if (!sub || dbg_streq_ci(sub, "STATUS")) {
        emit_lamp_status(io);
        return ESP_OK;
    }
    if (dbg_streq_ci(sub, "SRC")) {
        uint32_t v = 0;
        if (!arg_u32(f, 2, &v) || v > 1u) {
            emit_err_text(io, "BAD_ARG", "LAMP,SRC,<0=板载 1=尼特利>");
            return APP_ERR_BAD_ARG;
        }
        (void)lamp_source_set((int)v);
        emit_lamp_status(io);
        return ESP_OK;
    }
    if (dbg_streq_ci(sub, "ON") || dbg_streq_ci(sub, "OFF")) {
        const bool on = dbg_streq_ci(sub, "ON");
        /* 走 light_actuator 的统一入口：它会按当前光源路由，
         * 同时把"逻辑亮度"记下来，切回板载灯时才能恢复。 */
        float b = light_actuator_get_brightness();
        if (on && b <= 0.0f) {
            b = 100.0f;
        }
        (void)light_actuator_set_brightness(on ? b : 0.0f);
        emit_lamp_status(io);
        return ESP_OK;
    }
    if (dbg_streq_ci(sub, "LEVEL")) {
        uint32_t v = 0;
        if (!arg_u32(f, 2, &v) || v > 100u) {
            emit_err_text(io, "BAD_ARG", "LAMP,LEVEL,<0-100>");
            return APP_ERR_BAD_ARG;
        }
        (void)light_actuator_set_brightness((float)v);
        emit_lamp_status(io);
        return ESP_OK;
    }
    if (dbg_streq_ci(sub, "RGBW")) {
        uint32_t r = 0, g = 0, b = 0, w = 0;
        if (!arg_u32(f, 2, &r) || !arg_u32(f, 3, &g) ||
            !arg_u32(f, 4, &b) || !arg_u32(f, 5, &w) ||
            r > 100u || g > 100u || b > 100u || w > 100u) {
            emit_err_text(io, "BAD_ARG", "LAMP,RGBW,<r>,<g>,<b>,<w>  (每路 0-100)");
            return APP_ERR_BAD_ARG;
        }
        (void)netlea_lamp_set_rgbw((uint8_t)r, (uint8_t)g, (uint8_t)b, (uint8_t)w);
        emit_lamp_status(io);
        return ESP_OK;
    }
    if (dbg_streq_ci(sub, "FAN")) {
        uint32_t v = 0;
        if (!arg_u32(f, 2, &v) || v > 255u) {
            emit_err_text(io, "BAD_ARG", "LAMP,FAN,<0-255>");
            return APP_ERR_BAD_ARG;
        }
        (void)netlea_lamp_set_fan((uint8_t)v);
        emit_lamp_status(io);
        return ESP_OK;
    }
    if (dbg_streq_ci(sub, "SIM")) {
        uint32_t v = 0;
        if (!arg_u32(f, 2, &v) || v > 1u) {
            emit_err_text(io, "BAD_ARG", "LAMP,SIM,<0|1>  (1=用内置模拟灯)");
            return APP_ERR_BAD_ARG;
        }
        (void)netlea_lamp_set_sim((int)v);
        emit_lamp_status(io);
        return ESP_OK;
    }
    if (dbg_streq_ci(sub, "QUERY")) {
        (void)netlea_lamp_query();
        vTaskDelay(pdMS_TO_TICKS(120));
        emit_lamp_status(io);
        return ESP_OK;
    }
    if (dbg_streq_ci(sub, "SAVE")) {
        esp_err_t r = netlea_lamp_save();
        if (r != ESP_OK) {
            emit_err(io, r);
            return r;
        }
        char b2[96];
        dbg_out_t o2;
        dbg_out_init(&o2, b2, sizeof(b2), "OK");
        dbg_out_kv_str(&o2, "action", "lamp_save");
        emit(io, &o2);
        return ESP_OK;
    }
    if (dbg_streq_ci(sub, "SCAN")) {
        (void)netlea_lamp_scan_start();
        for (int i = 0; i < 24 && !netlea_lamp_scan_done(); i++) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        netlea_scan_item_t items[NETLEA_SCAN_MAX];
        int n = netlea_lamp_scan_items(items, NETLEA_SCAN_MAX);
        for (int i = 0; i < n; i++) {
            char ms[18];
            /* ble_addr_t.val 是小端，打印成人读顺序 */
            snprintf(ms, sizeof(ms), "%02X:%02X:%02X:%02X:%02X:%02X",
                     items[i].mac[5], items[i].mac[4], items[i].mac[3],
                     items[i].mac[2], items[i].mac[1], items[i].mac[0]);
            char b[DEBUG_REPLY_MAX];
            dbg_out_t o;
            dbg_out_init(&o, b, sizeof(b), "NLAMP");
            dbg_out_kv_i(&o, "idx", i);
            dbg_out_kv_str(&o, "name", items[i].name);
            dbg_out_kv_str(&o, "mac", ms);
            dbg_out_kv_i(&o, "rssi", items[i].rssi);
            emit(io, &o);
        }
        char b2[128];
        dbg_out_t o2;
        dbg_out_init(&o2, b2, sizeof(b2), "DONE");
        dbg_out_kv_str(&o2, "scan", "lamp");
        dbg_out_kv_i(&o2, "count", n);
        emit(io, &o2);
        return ESP_OK;
    }
    if (dbg_streq_ci(sub, "BIND")) {
        const char *mac = dbg_fields_at(f, 2);
        esp_err_t r = netlea_lamp_bind(mac);
        if (r != ESP_OK) {
            emit_err_text(io, "BAD_ARG", "LAMP,BIND,<AA:BB:CC:DD:EE:FF>");
            return APP_ERR_BAD_ARG;
        }
        emit_lamp_status(io);
        return ESP_OK;
    }
    emit_err_text(io, "BAD_ARG",
                  "LAMP,<STATUS|SRC,n|ON|OFF|LEVEL,n|RGBW,r,g,b,w|FAN,n|SIM,0|1|QUERY|SAVE|SCAN|BIND,mac>");
    return APP_ERR_BAD_ARG;
}

static const dbg_cmd_t s_cmds[] = {
    {"PING",     cmd_ping,     "PING"},
    {"INFO",     cmd_info,     "INFO"},
    {"HELP",     cmd_help,     "HELP"},
    {"SENSORS",  cmd_sensors,  "SENSORS"},
    {"SENSOR",   cmd_sensor,   "SENSOR,<id>[,fresh|cache]"},
    {"ACT",      cmd_act,      "ACT,<LIST|heater|pump|refill|co2|uv|air|light|feeder|feed>[,on|off|值]"},
    {"PARAM",    cmd_param,    "PARAM,<LIST|GET|SET|SAVE|LOAD|RESET>,<name>[,value]"},
    {"I2C",      cmd_i2c,      "I2C,<SCAN|READ|WRITE>,<addr>,<reg>[,len|data...]"},
    {"RTC",      cmd_rtc,      "RTC,<GET|SET>,<YYYY-MM-DD>,<HH:MM[:SS]>"},
    {"SCH",      cmd_sch,      "SCH,<LIST|SET|SAVE|CLR>,<target>,<en>,<HH:MM>,<dur>,<pct>"},
    {"VOICE",    cmd_voice,    "VOICE,<STATUS|SNIFF|SEND|BAUD|EXEC>,..."},
    {"SD",       cmd_sd,       "SD,<STATUS|STREAM|FLUSH|PIN>"},
    {"LAMP",     cmd_lamp,     "LAMP,<STATUS|SRC|ON|OFF|LEVEL|RGBW|FAN|SIM|QUERY|SAVE>"},
    {"NET",      cmd_net,      "NET,<STATUS|SET|SAVE|CONNECT|DISCONNECT|SCAN|PUB>"},
    {"GPIO",     cmd_gpio,     "GPIO,<LS|GET|SET|MODE>,<pin>[,<0|1|in|out>]"},
    {"ADC",      cmd_adc,      "ADC,<LIST|GET>,<ph|tds|turb|level|light>"},
    {"MODE",     cmd_mode,     "MODE,<GET|MANUAL[,秒]|AUTO>"},
    {"STREAM",   cmd_stream,   "STREAM,<ON[,毫秒]|OFF|NOW>"},
    {"TEST",     cmd_test,     "TEST,ACTUATORS[,步进毫秒]  (逐路点动，发任意字节可中断)"},
    {"SELFTEST", cmd_selftest, "SELFTEST"},
    {"LOG",      cmd_log,      "LOG,<none|error|warn|info|debug|verbose>"},
    {"REBOOT",   cmd_reboot,   "REBOOT"},
};

#define DBG_CMD_COUNT (sizeof(s_cmds) / sizeof(s_cmds[0]))

static esp_err_t cmd_help(const dbg_fields_t *f, const dbg_io_t *io) {
    (void)f;
    for (size_t i = 0; i < DBG_CMD_COUNT; i++) {
        char buf[DEBUG_REPLY_MAX];
        dbg_out_t o;
        dbg_out_init(&o, buf, sizeof(buf), "HELP");
        dbg_out_kv_str(&o, "verb", s_cmds[i].verb);
        dbg_out_kv_str(&o, "usage", s_cmds[i].usage);
        emit(io, &o);
    }
    char buf[96];
    dbg_out_t o;
    dbg_out_init(&o, buf, sizeof(buf), "DONE");
    dbg_out_kv_u(&o, "count", (unsigned long)DBG_CMD_COUNT);
    dbg_out_kv_u(&o, "proto", DEBUG_PROTOCOL_VERSION);
    emit(io, &o);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 遥测帧                                                              */
/* ------------------------------------------------------------------ */

void debug_commands_build_telemetry(char *buf, size_t cap) {
    if (!buf || cap == 0) {
        return;
    }
    dbg_out_t o;
    dbg_out_init(&o, buf, cap, "TEL");

    static const sensor_id_t ids[] = {SH_TEMP, SH_PH, SH_TDS, SH_TURBIDITY, SH_LEVEL, SH_LIGHT, SH_CO2};
    static const char *const keys[] = {"temp", "ph", "tds", "turb", "level", "light", "co2"};

    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        const sensor_sample_t *s = sensor_hub_get(ids[i]);
        if (s && s->valid) {
            dbg_out_kv_f(&o, keys[i], s->value, sensor_hub_decimals(ids[i]));
        }
    }

    dbg_out_kv_bool(&o, "heater", heater_get_state());
    dbg_out_kv_bool(&o, "pump", pump_get_state());
    dbg_out_kv_bool(&o, "refill", water_pump_get_state());
    dbg_out_kv_bool(&o, "co2v", co2_valve_get_state());
    dbg_out_kv_bool(&o, "uv", uv_lamp_get_state());
    dbg_out_kv_bool(&o, "air", air_pump_get_state());
    dbg_out_kv_f(&o, "light_pwm", light_actuator_get_brightness(), 0);
    dbg_out_kv_f(&o, "servo", feeder_get_angle(), 0);
    dbg_out_kv_str(&o, "mode", control_mode_name());
    dbg_out_kv_u(&o, "up_s", (unsigned long)(esp_timer_get_time() / 1000000));
}

/* ------------------------------------------------------------------ */
/* 分发                                                                */
/* ------------------------------------------------------------------ */

esp_err_t debug_commands_init(i2c_bus_handle_t bus) {
    s_bus = bus;
    s_stream_enabled = false;
    s_stream_period_ms = param_get_u32(PARAM_IDX_STREAM_PERIOD_MS);
    if (s_stream_period_ms < 100) {
        s_stream_period_ms = DEBUG_STREAM_PERIOD_MS;
    }
    ESP_LOGI(TAG, "debug command table ready, %u verbs", (unsigned)DBG_CMD_COUNT);
    return ESP_OK;
}

esp_err_t debug_commands_dispatch(const dbg_fields_t *fields, const dbg_io_t *io) {
    if (!fields || fields->count == 0 || !fields->field[0]) {
        return APP_ERR_BAD_FRAME;
    }

    const char *verb = fields->field[0];
    for (size_t i = 0; i < DBG_CMD_COUNT; i++) {
        if (dbg_streq_ci(verb, s_cmds[i].verb)) {
            esp_err_t ret = s_cmds[i].handler(fields, io);
            return ret;
        }
    }

    /* 兼容常见别名 */
    if (dbg_streq_ci(verb, "TIME")) {
        return cmd_rtc(fields, io);
    }
    if (dbg_streq_ci(verb, "STATUS")) {
        return cmd_sensors(fields, io);
    }

    emit_err_text(io, "UNKNOWN_CMD", "unknown verb, send HELP to list commands");
    return APP_ERR_UNKNOWN_CMD;
}

bool debug_commands_should_abort(void) {
    return abort_requested();
}

void debug_commands_set_abort_check(bool (*fn)(void)) {
    s_abort_check = fn;
}
