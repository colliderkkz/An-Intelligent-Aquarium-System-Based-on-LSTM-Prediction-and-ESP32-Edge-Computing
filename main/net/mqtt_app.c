#include "net/mqtt_app.h"
#include "esp_timer.h"

#include "actuators/air_pump.h"
#include "actuators/co2_valve.h"
#include "actuators/feeder.h"
#include "actuators/heater.h"
#include "actuators/light_actuator.h"
#include "actuators/netlea_lamp.h"
#include "actuators/pump.h"
#include "actuators/uv_lamp.h"
#include "actuators/water_pump.h"
#include "cJSON.h"
#include "config/system_config.h"
#include "esp_log.h"
#include "drivers/sd_card_spi.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "mqtt_client.h"
#include "net/wifi_manager.h"
#include "service/control_mode.h"
#include "config/param_store.h"
#include "service/schedule.h"
#include "service/act_hold.h"
#include "service/act_registry.h"
#include "service/sd_logger.h"
#include "service/history.h"
#include "service/sensor_hub.h"
#include "service/time_service.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "MQTT_APP";

/* 状态上报周期。
 * 原来 5 秒：手机上点了开关要等最多 5 秒按钮才变色，感觉"没反应"就会重复点。
 * 降到 2 秒；另外执行完命令还会立即补发一次（见 handle_command）。 */
#define PUB_PERIOD_MS   2000

/* ---------------- 双路上行 ----------------
 * 链路 0 = 家里 NAS 的 Mosquitto（局域网，延迟最低）
 * 链路 1 = 公网 broker 直连（不经过 NAS，NAS 关机/桥接断了外网也能用）
 *
 * 两条链路同时在线、同时发布；收命令任一路到达就执行。
 * 同一条命令可能"公网直连"和"桥接转发"各来一次，用报文内容去重。
 */
#define MQTT_LINKS 2

static esp_mqtt_client_handle_t s_cli[MQTT_LINKS] = { NULL, NULL };
static bool s_cli_up[MQTT_LINKS] = { false, false };
static char s_uri[MQTT_LINKS][96];
static bool s_started = false;

/* 句柄生命周期保护。
 * stop_link() 会 esp_mqtt_client_destroy() 把句柄释放掉，而 mqtt_pub 任务 /
 * sd_logger 任务 / MQTT 事件任务可能正好在 esp_mqtt_client_publish() 里用同一个
 * 句柄 —— 那是一次 use-after-free，表现就是"网络一切换就偶发重启/死机"。
 * 做法：发布前在临界区里取出句柄并计数；停止时先把句柄从数组摘掉（新发布者
 * 立刻看不到它），再等计数归零才真正 destroy。 */
static portMUX_TYPE s_cli_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile int s_pub_busy[MQTT_LINKS] = { 0, 0 };
/* 事件回调里正在跑（handle_command 可能还在用 e->data）。
 * 释放客户端之前必须等它退出，否则事件参数会指向已释放的 in_buffer。 */
static volatile int s_evt_busy = 0;
/* 最近一次"发布成功"的时间戳（ms）。用来识别"NAT 隧道假活"：
 * TCP 还在、broker 也回了 CONNACK，但发出去的数据其实到不了对端，
 * 这时 s_cli_up 仍是 true，只有"发布是否有返回"能看出问题。 */
static volatile uint32_t s_last_pub_ok_ms = 0;

static uint32_t mqtt_now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}
/* 当前实际在用哪一路（0 = 局域网 NAS，1 = 公网隧道） */
static volatile int s_active_link = 0;
static void start_link(int i);
static void stop_link(int i);
static bool s_connected = false;                 /* 任意一路在线 */
static char s_id[16] = "esp32";
static char s_topic_state[96];
static char s_topic_cmd[96];
static char s_topic_ack[96];
static char s_topic_sdline[96];
static char s_topic_disc[96];
static char s_topic_caps[96];

static void publish_discovery(void);
static void publish_caps(void);
static void hist_task(void *arg);
static atomic_bool lamp_scan_running;
static void lamp_scan_task(void *arg);
static void feed_task(void *arg);
static char s_summ[128] = "not started";
static TaskHandle_t s_pub_task = NULL;

static void build_topics(void) {
    snprintf(s_topic_state, sizeof(s_topic_state), "%s/%s/state", net_prefix(), s_id);
    snprintf(s_topic_cmd, sizeof(s_topic_cmd), "%s/%s/cmd", net_prefix(), s_id);
    snprintf(s_topic_ack, sizeof(s_topic_ack), "%s/%s/ack", net_prefix(), s_id);
    snprintf(s_topic_sdline, sizeof(s_topic_sdline), "%s/%s/sdline", net_prefix(), s_id);
    snprintf(s_topic_disc, sizeof(s_topic_disc), "%s/%s/discovery", net_prefix(), s_id);
    snprintf(s_topic_caps, sizeof(s_topic_caps), "%s/%s/caps", net_prefix(), s_id);
}

static bool any_link_up(void) {
    for (int i = 0; i < MQTT_LINKS; i++) {
        if (s_cli_up[i]) {
            return true;
        }
    }
    return false;
}

/* 两条链路都发一遍；retain=1 时各自 broker 都会存一份 */
static void publish_ex(const char *topic, const char *text, int retain) {
    if (!topic || !text) {
        return;
    }
    for (int i = 0; i < MQTT_LINKS; i++) {
        esp_mqtt_client_handle_t c = NULL;
        taskENTER_CRITICAL(&s_cli_mux);
        if (s_cli[i] && s_cli_up[i]) {
            c = s_cli[i];
            s_pub_busy[i]++;
        }
        taskEXIT_CRITICAL(&s_cli_mux);
        if (c == NULL) {
            continue;
        }
        const int r = esp_mqtt_client_publish(c, topic, text, 0, 0, retain);
        taskENTER_CRITICAL(&s_cli_mux);
        s_pub_busy[i]--;
        taskEXIT_CRITICAL(&s_cli_mux);
        if (r >= 0) {
            s_last_pub_ok_ms = mqtt_now_ms();
        }
    }
}

static void publish_text(const char *topic, const char *text) {
    publish_ex(topic, text, 0);
}

/* 命令去重：完全相同的报文 3 秒内只执行一次。
 * 手机端每条命令都会带 "_t" 时间戳，所以两次真实操作绝不会得到相同报文。 */
static char s_last_cmd[256];
static int s_last_len = -1;
static uint32_t s_last_cmd_ms = 0;

/* NVS 落盘要 flash 擦写，10~100ms（页满时更久）。绝不能放在 MQTT 事件任务里，
 * 否则每次改参数 / 存计划，回执都要晚这么多。这里只置标志，由 save_task 合并落盘。 */
static volatile bool s_need_param_save = false;
static volatile bool s_need_sched_save = false;

static bool cmd_is_dup(const char *data, int len) {
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (data && len > 0 && len < (int)sizeof(s_last_cmd) && s_last_len == len &&
        s_last_cmd[0] &&
        memcmp(s_last_cmd, data, (size_t)len) == 0 &&
        (uint32_t)(now - s_last_cmd_ms) < 3000u) {
        return true;                  /* 不刷新时间戳，连续补发也只挡一次 */
    }
    if (data && len > 0 && len < (int)sizeof(s_last_cmd)) {
        memcpy(s_last_cmd, data, (size_t)len);
        s_last_cmd[len] = '\0';
        s_last_len = len;
        s_last_cmd_ms = now;
    }
    return false;
}

static void ack(const char *what, bool ok, const char *detail) {
    char buf[192];
    snprintf(buf, sizeof(buf), "{\"cmd\":\"%s\",\"ok\":%d,\"msg\":\"%s\"}",
             what ? what : "?", ok ? 1 : 0, detail ? detail : "");
    publish_text(s_topic_ack, buf);
}


/* ---------------- SD 行 -> MQTT ---------------- */

/**
 * @brief sd_logger 每写入一行就回调到这里，转成 JSON 发出去
 *
 * 每行单独一条消息（QoS0），延迟低、手机端能"边写边看"。
 * 文本直接放进 JSON 的 "t" 字段，cJSON 会处理逗号/引号转义。
 */
static void sd_line_publisher(const char *kind, const char *text, size_t len) {
    if (!s_connected || !text || len == 0) {
        return;
    }
    char buf[256];
    size_t n = (len < sizeof(buf) - 1) ? len : sizeof(buf) - 1;
    memcpy(buf, text, n);
    buf[n] = '\0';

    cJSON *o = cJSON_CreateObject();
    if (!o) {
        return;
    }
    cJSON_AddStringToObject(o, "k", kind ? kind : "?");
    cJSON_AddStringToObject(o, "t", buf);
    char *js = cJSON_PrintUnformatted(o);
    if (js) {
        publish_text(s_topic_sdline, js);
        cJSON_free(js);
    }
    cJSON_Delete(o);
}

/* ---------------- 状态上报 ---------------- */

static void publish_state(void) {
    if (!s_connected) {
        return;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return;
    }

    const sensor_sample_t *t = sensor_hub_get(SH_TEMP);
    const sensor_sample_t *p = sensor_hub_get(SH_PH);
    const sensor_sample_t *tds = sensor_hub_get(SH_TDS);
    const sensor_sample_t *tur = sensor_hub_get(SH_TURBIDITY);
    const sensor_sample_t *lvl = sensor_hub_get(SH_LEVEL);
    const sensor_sample_t *lit = sensor_hub_get(SH_LIGHT);
    const sensor_sample_t *co2 = sensor_hub_get(SH_CO2);

    /* 传感器有效才发数值，无效发 null（手机端好区分"0"和"没数据"） */
#define ADD_SENSOR(key, samp)                                            \
    do {                                                                 \
        if ((samp) && (samp)->err == ESP_OK) {                           \
            cJSON_AddNumberToObject(root, key, (samp)->value);           \
        } else {                                                         \
            cJSON_AddNullToObject(root, key);                            \
        }                                                                \
    } while (0)

    ADD_SENSOR("temp", t);
    ADD_SENSOR("ph", p);
    ADD_SENSOR("tds", tds);
    ADD_SENSOR("turb", tur);
    ADD_SENSOR("level", lvl);
    ADD_SENSOR("light", lit);
    ADD_SENSOR("co2", co2);
#undef ADD_SENSOR

    cJSON_AddBoolToObject(root, "heater", heater_get_state());
    cJSON_AddBoolToObject(root, "pump", pump_get_state());
    cJSON_AddBoolToObject(root, "refill", water_pump_get_state());
    cJSON_AddBoolToObject(root, "co2v", co2_valve_get_state());
    cJSON_AddBoolToObject(root, "uv", uv_lamp_get_state());
    cJSON_AddBoolToObject(root, "air", air_pump_get_state());
    cJSON_AddNumberToObject(root, "light_pwm", light_actuator_get_brightness());
    /* 灯：板载 PWM / 尼特利 BLE 二选一 + 尼特利当前状态 */
    cJSON_AddNumberToObject(root, "lamp_src", lamp_source_get());
    {
        netlea_state_t n = {0};
        netlea_lamp_reported(&n);
        cJSON_AddBoolToObject(root, "nlea_on", n.on);
        cJSON_AddNumberToObject(root, "nlea_r", n.r);
        cJSON_AddNumberToObject(root, "nlea_g", n.g);
        cJSON_AddNumberToObject(root, "nlea_w", n.w);
        cJSON_AddNumberToObject(root, "nlea_b", n.b);
        cJSON_AddNumberToObject(root, "nlea_fan", n.fan);
        cJSON_AddBoolToObject(root, "nlea_ok", netlea_lamp_ready());
        cJSON_AddStringToObject(root, "nlea_mac", netlea_lamp_mac_str());
        cJSON_AddStringToObject(root, "nlea_tr", netlea_lamp_transport());
    }
    /* 六路 PWM 的实际占空比，调光/调速时能看到真实值 */
    cJSON_AddNumberToObject(root, "pump_pct", (double)pump_get_percent());
    cJSON_AddNumberToObject(root, "air_pct", (double)air_pump_get_percent());
    cJSON_AddNumberToObject(root, "refill_pct", (double)water_pump_get_percent());
    cJSON_AddNumberToObject(root, "co2_pct", (double)co2_valve_get_percent());
    cJSON_AddNumberToObject(root, "uv_pct", (double)uv_lamp_get_percent());
    cJSON_AddNumberToObject(root, "heater_pct", (double)heater_get_percent());
    cJSON_AddNumberToObject(root, "servo", feeder_get_angle());
    cJSON_AddStringToObject(root, "mode", control_mode_name());
    /* 用户关心的设定值，手机端要直接显示 */
    cJSON_AddNumberToObject(root, "temp_target", (double)param_get(PARAM_IDX_TEMP_TARGET));
    cJSON_AddNumberToObject(root, "level_min", (double)param_get(PARAM_IDX_LEVEL_MIN_PCT));
    /* 计划暂停剩余：显式手动模式 or 任一执行器的独立保持，取最大 */
    double hold_s = (double)control_mode_manual_left_s();
    double sched_s = (double)act_hold_max_left_s();
    cJSON_AddNumberToObject(root, "manual_left_s", hold_s > sched_s ? hold_s : sched_s);
    cJSON_AddBoolToObject(root, "manual_mode", control_mode_is_manual());
    char held[48];
    act_hold_names(held, sizeof(held));
    cJSON_AddStringToObject(root, "held", held);

    pcf8563_time_t now = {0};
    time_service_now(&now);
    char ts[32];
    snprintf(ts, sizeof(ts), "%04u-%02u-%02u %02u:%02u:%02u",
             (unsigned)now.year, (unsigned)now.month, (unsigned)now.day,
             (unsigned)now.hour, (unsigned)now.minute, (unsigned)now.second);
    cJSON_AddStringToObject(root, "time", ts);
    cJSON_AddStringToObject(root, "src", time_service_source());
    cJSON_AddStringToObject(root, "ip", net_ip());

    /* SD 卡状态：手机上要能看到"卡在不在、写了多少、文件多大" */
    cJSON_AddBoolToObject(root, "sd_on", sd_card_is_mounted());
    cJSON_AddBoolToObject(root, "sd_rec", sd_logger_is_active());
    cJSON_AddNumberToObject(root, "sd_rows", (double)sd_logger_rows());
    cJSON_AddNumberToObject(root, "sd_data", (double)sd_logger_data_size());
    cJSON_AddNumberToObject(root, "sd_log", (double)sd_logger_log_size());
    cJSON_AddBoolToObject(root, "sd_str", sd_logger_is_streaming());

    char *txt = cJSON_PrintUnformatted(root);
    if (txt) {
        publish_text(s_topic_state, txt);
        cJSON_free(txt);
    }
    cJSON_Delete(root);
}


/* "08:00" 或 "480" -> 一天中的第几分钟 */
static bool parse_hhmm_str(const char *s, int *out_min) {
    if (!s || !out_min || !s[0]) {
        return false;
    }
    int h = 0, m = 0;
    if (sscanf(s, "%d:%d", &h, &m) == 2) {
        if (h < 0 || h > 23 || m < 0 || m > 59) {
            return false;
        }
        *out_min = h * 60 + m;
        return true;
    }
    if (sscanf(s, "%d", &m) == 1) {
        if (m < 0 || m > 1439) {
            return false;
        }
        *out_min = m;
        return true;
    }
    return false;
}


/* ==================== 执行器安全策略 ==================== */

/**
 * @brief 判断某个动作是否会危害当前情况
 *
 * 这些动作一旦执行，计划会立刻把控制权收回去（不让用户手动保持），
 * 同时给用户明确的中文提示。
 *
 * @return true = 危险，已拒绝
 */
static bool refuse_if_dangerous(const char *act, bool want_on, const char **hint) {
    *hint = NULL;

    /* 加热：水温低于目标值时不许关 */
    if (strcmp(act, "heater") == 0 && !want_on) {
        const sensor_sample_t *s = sensor_hub_get(SH_TEMP);
        if (s && s->valid && s->value < param_get(PARAM_IDX_TEMP_TARGET) - 1.0f) {
            *hint = "水温低于目标，关闭加热会掉温，已拒绝";
            return true;
        }
    }

    /* 补水：只在"严重缺水"时禁止关泵（比补水线再低 20 个百分点，
     * 留足操作空间，避免用户想停一下都被拦）。 */
    if (strcmp(act, "refill") == 0 && !want_on) {
        const sensor_sample_t *s = sensor_hub_get(SH_LEVEL);
        const float critical = param_get(PARAM_IDX_LEVEL_MIN_PCT) - 20.0f;
        if (s && s->valid && s->value < critical && critical > 0.0f) {
            *hint = "水位已严重偏低，已拒绝关闭补水泵";
            return true;
        }
    }

    /* CO2：pH 偏低时不许开，会把 pH 压得更低 */
    if (strcmp(act, "co2") == 0 && want_on) {
        const sensor_sample_t *s = sensor_hub_get(SH_PH);
        if (s && s->valid && s->value < param_get(PARAM_IDX_PH_MIN)) {
            *hint = "pH 已偏低，开 CO2 会继续酸化，已拒绝";
            return true;
        }
    }

    return false;
}

/* 用户手动操作后，让计划暂停一段时间再恢复 */
/* 用户单独操作某个执行器后，只让"这个执行器"的计划暂停。
 * 固定 120 秒；不读 NVS 参数（参数可能存着旧值，行为不好预期）。 */
#define MANUAL_HOLD_S   120

static void hold_manual_for_user(const char *act) {
    if (act && act[0]) {
        act_hold_set(act, MANUAL_HOLD_S);
    }
}

/* 带中文提示的 ack */
static void ack_hint(const char *cmd, bool ok, const char *msg, const char *hint) {
    cJSON *o = cJSON_CreateObject();
    if (!o) {
        return;
    }
    cJSON_AddStringToObject(o, "cmd", cmd ? cmd : "?");
    cJSON_AddNumberToObject(o, "ok", ok ? 1 : 0);
    if (msg && msg[0]) {
        cJSON_AddStringToObject(o, "msg", msg);
    }
    if (hint && hint[0]) {
        cJSON_AddStringToObject(o, "hint", hint);
    }
    char *js = cJSON_PrintUnformatted(o);
    if (js) {
        publish_text(s_topic_ack, js);
        cJSON_free(js);
    }
    cJSON_Delete(o);
}

/* ---------------- 命令处理 ---------------- */

/* 喂食：舵机要转到位再回位，中间有 vTaskDelay，必须独立任务跑 */
static void feed_task(void *arg) {
    (void)arg;
    (void)feeder_feed_now();
    publish_state();
    vTaskDelete(NULL);
}

/* 后台落盘：把事件任务里的 NVS 写挪出来，顺便合并 600ms 内的重复修改 */
static void save_task(void *arg) {
    (void)arg;
    int64_t last_write_us = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(600));
        /* 两次落盘至少隔 10 秒：NVS 只有 24KB，公网高频命令会把 flash 写穿 */
        if ((s_need_param_save || s_need_sched_save) && last_write_us != 0 &&
            esp_timer_get_time() - last_write_us < 10000000LL) {
            continue;
        }
        if (s_need_param_save || s_need_sched_save) {
            last_write_us = esp_timer_get_time();
        }
        if (s_need_param_save) {
            s_need_param_save = false;
            if (param_store_save() != ESP_OK) {
                s_need_param_save = true;       /* 失败下次再试 */
            }
        }
        if (s_need_sched_save) {
            s_need_sched_save = false;
            if (schedule_save() != ESP_OK) {
                s_need_sched_save = true;
            }
        }
    }
}

/**
 * @brief 轻量结构校验，挡住能让 cJSON 递归爆栈的报文
 *
 * handle_command 跑在 esp-mqtt 事件任务里（默认栈约 4KB），
 * cJSON_Parse 每层嵌套要 60~100B 栈：一条 500 字节的 "[[[[…" 就能做 500 层，
 * 直接把事件任务打爆 → panic 重启（日志里的 prev=PANIC 很可能就是它）。
 * 正常命令都是单层对象，这里直接白名单掉其它形状。
 */
static bool cmd_shape_ok(const char *s, int len) {
    if (!s || len < 2 || len > 256) {
        return false;
    }
    int depth = 0;
    bool in_str = false, esc = false;
    for (int i = 0; i < len; i++) {
        const char c = s[i];
        if (in_str) {
            if (esc) {
                esc = false;
            } else if (c == '\\') {
                esc = true;
            } else if (c == '"') {
                in_str = false;
            }
            continue;
        }
        if (c == '"') {
            in_str = true;
        } else if (c == '{' || c == '[') {
            if (++depth > 4) {
                return false;
            }
        } else if (c == '}' || c == ']') {
            if (--depth < 0) {
                return false;
            }
        }
    }
    return depth == 0 && !in_str;
}

static void handle_command(const char *json, int len) {
    char buf[320];
    if (len <= 0 || len >= (int)sizeof(buf)) {
        ack("parse", false, "payload too long");
        return;
    }
    if (!cmd_shape_ok(json, len)) {
        ack("parse", false, "bad shape");
        return;
    }
    memcpy(buf, json, (size_t)len);
    buf[len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        ack("parse", false, "invalid json");
        return;
    }

    const cJSON *act = cJSON_GetObjectItem(root, "act");
    const cJSON *cmd = cJSON_GetObjectItem(root, "cmd");

    if (cJSON_IsString(act)) {
        const char *a = act->valuestring;
        const cJSON *on = cJSON_GetObjectItem(root, "on");
        const cJSON *val = cJSON_GetObjectItem(root, "val");
        bool want_on = cJSON_IsNumber(on) ? (on->valueint != 0)
                                          : (cJSON_IsBool(on) ? cJSON_IsTrue(on) : true);

        /* 灯光是连续量，单独处理 */
        if (strcmp(a, "light") == 0) {
            float pct = cJSON_IsNumber(val) ? (float)val->valuedouble : 100.0f;
            (void)light_actuator_set_brightness(pct);
            hold_manual_for_user(a);
            char hint[64];
            snprintf(hint, sizeof(hint), "灯光已设为 %.0f%%，计划暂停", (double)pct);
            ack_hint(a, true, "light set", hint);
        } else if (strcmp(a, "feed") == 0) {
            /* 不能在这里直接 feeder_feed_now()：它内部两次 vTaskDelay，
             * 默认 1.5s 就是阻塞 3 秒（参数上限 20s → 40 秒），
             * 期间 MQTT 事件任务完全停摆，所有命令和 keepalive 都卡住。
             * 投递到独立任务，回调立刻返回。 */
            {
                /* 10 秒内不重复投喂：去重只挡"完全相同报文"，
                 * 手机连点两下就是两次报文，会投两把饲料。 */
                static uint32_t last_feed_ms = 0;
                const uint32_t nowf = (uint32_t)(esp_timer_get_time() / 1000);
                if (last_feed_ms != 0 && (uint32_t)(nowf - last_feed_ms) < 10000u) {
                    ack_hint(a, false, "too_fast", "刚喂过，10 秒内不重复投喂");
                } else if (xTaskCreate(feed_task, "feed_req", 3072, NULL, 3, NULL) != pdPASS) {
                    ack_hint(a, false, "busy", "设备忙，稍后再试");
                } else {
                    last_feed_ms = nowf;
                    ack_hint(a, true, "feeding", "已投喂一次");
                }
            }
        } else if (strcmp(a, "mode") == 0) {
            const cJSON *v = cJSON_GetObjectItem(root, "val");
            if (cJSON_IsString(v) && strcmp(v->valuestring, "auto") == 0) {
                control_mode_exit_manual();
                ack_hint(a, true, "auto mode", "已回到自动，按计划执行");
            } else {
                control_mode_enter_manual(MANUAL_HOLD_S);
                ack_hint(a, true, "manual mode", "已进入手动，计划暂停");
            }
        } else if (strcmp(a, "netlea") == 0) {
            /* 尼特利灯：{"act":"netlea","r":100,"g":0,"b":0,"w":50,"fan":0,
             *               "val":80,"on":1}  —— 只传变化量也行 */
            netlea_state_t cur = {0};
            netlea_lamp_target(&cur);
            uint8_t rr = cur.r, gg = cur.g, bb = cur.b, ww = cur.w;
            bool rgb_touched = false;
            const cJSON *j;
            if ((j = cJSON_GetObjectItem(root, "r")) && cJSON_IsNumber(j)) {
                rr = (uint8_t)j->valueint; rgb_touched = true;
            }
            if ((j = cJSON_GetObjectItem(root, "g")) && cJSON_IsNumber(j)) {
                gg = (uint8_t)j->valueint; rgb_touched = true;
            }
            if ((j = cJSON_GetObjectItem(root, "b")) && cJSON_IsNumber(j)) {
                bb = (uint8_t)j->valueint; rgb_touched = true;
            }
            if ((j = cJSON_GetObjectItem(root, "w")) && cJSON_IsNumber(j)) {
                ww = (uint8_t)j->valueint; rgb_touched = true;
            }
            if (rgb_touched) {
                (void)netlea_lamp_set_rgbw(rr, gg, bb, ww);
            }
            if ((j = cJSON_GetObjectItem(root, "val")) && cJSON_IsNumber(j)) {
                (void)netlea_lamp_set_level((uint8_t)j->valueint);
            }
            if ((j = cJSON_GetObjectItem(root, "fan")) && cJSON_IsNumber(j)) {
                (void)netlea_lamp_set_fan((uint8_t)j->valueint);
            }
            if (cJSON_IsBool(on) || cJSON_IsNumber(on)) {
                (void)netlea_lamp_power(want_on);
            }
            /* 手动动过灯 → 让"灯"的计划暂停 120 秒，与其它执行器一致 */
            hold_manual_for_user("light");
            netlea_state_t now_t = {0};
            netlea_lamp_target(&now_t);
            char hb[110];
            snprintf(hb, sizeof(hb), "灯光 R%u G%u B%u W%u 亮度%u%%%s",
                     (unsigned)now_t.r, (unsigned)now_t.g, (unsigned)now_t.b,
                     (unsigned)now_t.w, (unsigned)now_t.bright,
                     netlea_lamp_ready() ? "" : "（灯未连接会用模拟值）");
            ack_hint(a, true, "netlea", hb);
        } else if (strcmp(a, "pump") == 0 || strcmp(a, "air") == 0 ||
                   strcmp(a, "heater") == 0 || strcmp(a, "uv") == 0 ||
                   strcmp(a, "co2") == 0 || strcmp(a, "refill") == 0) {
            /* 统一查表分发：加新执行器不用改这里 */
            const act_desc_t *ad = act_registry_find(a);
            if (!ad) {
                ack_hint(a, false, "unknown_actuator", "未知的执行器");
            } else {
                int pct = cJSON_IsNumber(val) ? val->valueint
                                              : (want_on ? ad->max_pct : 0);
                if (pct < 0) {
                    pct = 0;
                }
                if (pct > (int)ad->max_pct) {
                    pct = (int)ad->max_pct;
                }
                const bool will_on = (pct > 0);

                const char *hint = NULL;
                if (refuse_if_dangerous(a, will_on, &hint)) {
                    ack_hint(a, false, "blocked_by_safety", hint);
                } else {
                    esp_err_t er = ad->set_level((uint8_t)pct);
                    uint8_t now_pct = ad->get_percent ? ad->get_percent() : 0;
                    if (er == ESP_OK && now_pct == (uint8_t)pct) {
                        /* 循环泵有"常开"策略，会周期性把泵拉回满档。
                         * 用户明确关泵时就把常开关掉，开泵时再打开。 */
                        if (strcmp(a, "pump") == 0) {
                            (void)param_set(PARAM_IDX_PUMP_ALWAYS_ON, pct > 0 ? 1.0f : 0.0f);
                            s_need_param_save = true;   /* 交给后台任务，事件任务里不写 flash */
                        }
                        hold_manual_for_user(a);
                        char hb[96];
                        snprintf(hb, sizeof(hb), "%s已设为 %d%%", ad->label, pct);
                        ack_hint(a, true, "level", hb);
                    } else {
                        ack_hint(a, false, "hardware_fail", "执行器没有响应，请检查接线");
                    }
                }
            }
        } else {
            ack_hint(a, false, "unknown_actuator", "未知的执行器");
        }
    } else if (cJSON_IsString(cmd)) {
        const char *c = cmd->valuestring;
        if (strcmp(c, "state") == 0) {
            publish_state();
            ack(c, true, "state pushed");
        } else if (strcmp(c, "sched_set") == 0) {
            /* 与上位机 SCH,SET 完全等价：只改传进来的字段，其余保持不变 */
            const cJSON *jname = cJSON_GetObjectItem(root, "name");
            int idx = -1;
            if (!cJSON_IsString(jname) || !schedule_find(jname->valuestring, &idx)) {
                ack(c, false, "unknown target (light/air/pump/uv/co2)");
            } else {
                const sched_entry_t *cur = schedule_get(idx);
                if (!cur) {
                    ack(c, false, "bad index");
                } else {
                    sched_entry_t e = *cur;
                    const cJSON *jen = cJSON_GetObjectItem(root, "en");
                    const cJSON *jstart = cJSON_GetObjectItem(root, "start");
                    const cJSON *jdur = cJSON_GetObjectItem(root, "dur");
                    const cJSON *jpct = cJSON_GetObjectItem(root, "pct");

                    if (cJSON_IsNumber(jen)) {
                        e.enabled = (jen->valueint != 0) ? 1 : 0;
                    } else if (cJSON_IsBool(jen)) {
                        e.enabled = cJSON_IsTrue(jen) ? 1 : 0;
                    }

                    if (cJSON_IsString(jstart)) {
                        int mm = 0;
                        if (parse_hhmm_str(jstart->valuestring, &mm)) {
                            e.start_min = (uint16_t)mm;
                        }
                    } else if (cJSON_IsNumber(jstart)) {
                        int mm = jstart->valueint;
                        if (mm >= 0 && mm <= 1439) {
                            e.start_min = (uint16_t)mm;
                        }
                    }

                    if (cJSON_IsNumber(jdur)) {
                        int v = jdur->valueint;
                        if (v >= 1 && v <= 1440) {
                            e.dur_min = (uint16_t)v;
                        }
                    }
                    if (cJSON_IsNumber(jpct)) {
                        int v = jpct->valueint;
                        if (v >= 0 && v <= 100) {
                            e.percent = (uint8_t)v;
                        }
                    }

                    /* 支持"起始 -> 结束"：传 end 时自动换算时长，支持跨午夜 */
                    const cJSON *jend = cJSON_GetObjectItem(root, "end");
                    int end_min = -1;
                    if (cJSON_IsString(jend)) {
                        int mm = 0;
                        if (parse_hhmm_str(jend->valuestring, &mm)) {
                            end_min = mm;
                        }
                    } else if (cJSON_IsNumber(jend)) {
                        int mm = jend->valueint;
                        if (mm >= 0 && mm <= 1439) {
                            end_min = mm;
                        }
                    }
                    if (end_min >= 0) {
                        int d = (end_min - (int)e.start_min + 1440) % 1440;
                        e.dur_min = (uint16_t)((d == 0) ? 1440 : d);
                    }

                    esp_err_t r = schedule_set(idx, &e);
                    if (r == ESP_OK) {
                        s_need_sched_save = true;  /* 落 NVS 交给后台任务 */
                    }
                    ack(c, r == ESP_OK, r == ESP_OK ? "schedule saved" : esp_err_to_name(r));
                }
            }
        } else if (strcmp(c, "set") == 0) {
            /* {"cmd":"set","temp":25.5} 或 {"cmd":"set","level":60} */
            const cJSON *jt = cJSON_GetObjectItem(root, "temp");
            const cJSON *jl = cJSON_GetObjectItem(root, "level");
            char hint[96] = {0};
            bool ok = false;
            if (cJSON_IsNumber(jt)) {
                if (param_set(PARAM_IDX_TEMP_TARGET, (float)jt->valuedouble) == ESP_OK) {
                    s_need_param_save = true;
                    snprintf(hint, sizeof(hint), "目标水温已设为 %.1f℃",
                             (double)jt->valuedouble);
                    ok = true;
                }
            } else if (cJSON_IsNumber(jl)) {
                /* 起始水位必须低于停止水位，否则 water_control 判定配置非法、
                 * 永久关闭自动补水，而界面还显示一切正常。这里留 5% 余量。 */
                float lim = param_get(PARAM_IDX_LEVEL_STOP_PCT) - 5.0f;
                if (lim < 0.0f) {
                    lim = 0.0f;
                }
                float lv = (float)jl->valuedouble;
                if (lv > lim) {
                    lv = lim;
                }
                if (param_set(PARAM_IDX_LEVEL_MIN_PCT, lv) == ESP_OK) {
                    s_need_param_save = true;
                    /* 回显"实际生效值"而不是请求值：被安全边界夹过之后，
                     * 手机要看到真正存进去的数，否则会以为自己设成了 99%。 */
                    if (lv < (float)jl->valuedouble - 0.5f) {
                        snprintf(hint, sizeof(hint),
                                 "补水水位已设为 %.0f%%（不能高于停止水位-5%%）", (double)lv);
                    } else {
                        snprintf(hint, sizeof(hint), "补水水位已设为 %.0f%%", (double)lv);
                    }
                    ok = true;
                }
            }
            ack_hint(c, ok, ok ? "saved" : "bad_arg",
                     ok ? hint : "参数无效");
        } else if (strcmp(c, "hist") == 0) {
            /* 投递到独立任务，避免阻塞命令处理 */
            BaseType_t ok = xTaskCreate(hist_task, "hist_req", 6144, NULL, 3, NULL);
            if (ok != pdPASS) {
                ack_hint(c, false, "busy", "设备忙，稍后再试");
            }
        } else if (strcmp(c, "lamp") == 0) {
            /* {"cmd":"lamp","src":0|1} —— 板载 PWM 灯 / 尼特利 BLE 灯 二选一 */
            const cJSON *jscan = cJSON_GetObjectItem(root, "scan");
            const cJSON *jmac = cJSON_GetObjectItem(root, "mac");
            if (cJSON_IsNumber(jscan) && jscan->valueint) {
                if (atomic_exchange(&lamp_scan_running, true)) {
                    ack_hint(c, true, "scanning", "扫描正在进行");
                } else if (xTaskCreate(lamp_scan_task, "lamp_scan", 4500, NULL, 4, NULL) != pdPASS) {
                    lamp_scan_running = false;
                    ack_hint(c, false, "scan", "任务创建失败");
                } else {
                    ack_hint(c, true, "scan started", "正在搜索附近的灯，约 7 秒后回结果");
                }
            } else if (cJSON_IsString(jmac) && jmac->valuestring && jmac->valuestring[0]) {
                esp_err_t r = netlea_lamp_bind(jmac->valuestring);
                ack_hint(c, r == ESP_OK, "bind",
                         r == ESP_OK ? "已绑定该灯具，正在重连" : "地址无效或保存绑定失败");
            } else {
            const cJSON *jsrc = cJSON_GetObjectItem(root, "src");
            if (!cJSON_IsNumber(jsrc)) {
                ack_hint(c, false, "need src",
                         "src=0 用板载灯，src=1 用尼特利灯");
            } else {
                const int src = jsrc->valueint ? LAMP_SRC_NETLEA : LAMP_SRC_BOARD;
                (void)lamp_source_set(src);
                ack_hint(c, true, "lamp source",
                         src == LAMP_SRC_NETLEA ? "已切到尼特利灯" : "已切到板载灯");
            }
            }
        } else if (strcmp(c, "stream") == 0) {            const cJSON *onj = cJSON_GetObjectItem(root, "on");
            const bool want = cJSON_IsNumber(onj) ? (onj->valueint != 0)
                                                  : (cJSON_IsBool(onj) ? cJSON_IsTrue(onj) : true);
            sd_logger_set_stream(want);
            ack(c, true, want ? "stream on" : "stream off");
        } else if (strcmp(c, "sched") == 0) {
            /* BUGFIX：原来用 schedule_is_active()，它只是 enabled，
             * 界面因此永远显示"生效中"。这里改成真正的窗口判断。 */
            pcf8563_time_t now_t = {0};
            time_service_now(&now_t);
            char out[640];
            size_t off = 0;
            /* 注意：snprintf 返回的是"本该写入的长度"，被截断时 off 会超过缓冲。
             * 原写法直接把返回值累加到 off，一旦越界，后面 sizeof(out)-off 在
             * size_t 上下溢成天文数字 → 以 out+off 为起点越界写栈。
             * 这里每步都夹紧到缓冲内。 */
#define SCHED_APPEND(...)                                                     \
    do {                                                                      \
        if (off < sizeof(out)) {                                              \
            const int _n = snprintf(out + off, sizeof(out) - off, __VA_ARGS__); \
            if (_n > 0) {                                                     \
                off += (size_t)_n;                                            \
                if (off >= sizeof(out)) {                                     \
                    off = sizeof(out) - 1;                                    \
                }                                                             \
            }                                                                 \
        }                                                                     \
    } while (0)

            SCHED_APPEND("{\"type\":\"sched\",\"items\":[");
            for (int i = 0; i < schedule_count() && off < sizeof(out) - 128; ++i) {
                const sched_entry_t *e = schedule_get(i);
                if (!e) {
                    continue;
                }
                bool sched_win = false;
                if (e->enabled) {
                    schedule_window_info(i, &now_t, &sched_win, NULL, NULL);
                }
                SCHED_APPEND("%s{\"name\":\"%s\",\"en\":%d,\"start\":%u,\"dur\":%u,\"end\":%u,\"pct\":%u,\"active\":%d}",
                             i ? "," : "", schedule_name(i), e->enabled ? 1 : 0,
                             (unsigned)e->start_min, (unsigned)e->dur_min,
                             (unsigned)((e->start_min + e->dur_min) % 1440u),
                             (unsigned)e->percent, sched_win ? 1 : 0);
            }
            SCHED_APPEND("]}");
#undef SCHED_APPEND
            out[sizeof(out) - 1] = '\0';
            publish_text(s_topic_ack, out);
        } else {
            ack(c, false, "unknown command");
        }
    } else {
        ack("?", false, "need act or cmd");
    }

    cJSON_Delete(root);

    /* 命令执行完回推一次状态，让界面尽快看到真实值。
     * 手机端已经把 80ms 内的连点合并了，这里再留 60ms 的窗口挡住滑条拖动，
     * 单次点击基本是立即发 —— 原来 200ms 会让公网用户多等一截。 */
    {
        static uint32_t last_cmd_pub_ms = 0;
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (last_cmd_pub_ms == 0u ||
            (uint32_t)(now_ms - last_cmd_pub_ms) >= 60u) {
            last_cmd_pub_ms = now_ms;
            publish_state();
        }
    }
}

/* ---------------- MQTT 事件 ---------------- */


/* ---------------- 灯具搜索（异步，不阻塞 MQTT 事件任务） ---------------- */
static void lamp_scan_task(void *arg) {
    (void)arg;
    (void)netlea_lamp_scan_start();
    for (int i = 0; i < 24 && !netlea_lamp_scan_done(); i++) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    netlea_scan_item_t items[NETLEA_SCAN_MAX];
    const int n = netlea_lamp_scan_items(items, NETLEA_SCAN_MAX);
    cJSON *root = cJSON_CreateObject();
    if (root) {
        cJSON_AddStringToObject(root, "type", "lamp_scan");
        cJSON_AddBoolToObject(root, "done", netlea_lamp_scan_done());
        cJSON_AddNumberToObject(root, "n", n);
        cJSON *arr = cJSON_AddArrayToObject(root, "items");
        for (int i = 0; i < n; i++) {
            cJSON *it = cJSON_CreateObject();
            char ms[18];
            snprintf(ms, sizeof(ms), "%02X:%02X:%02X:%02X:%02X:%02X",
                     items[i].mac[5], items[i].mac[4], items[i].mac[3],
                     items[i].mac[2], items[i].mac[1], items[i].mac[0]);
            cJSON_AddStringToObject(it, "name", items[i].name);
            cJSON_AddStringToObject(it, "mac", ms);
            cJSON_AddNumberToObject(it, "rssi", items[i].rssi);
            cJSON_AddItemToArray(arr, it);
        }
        char topic[128];
        snprintf(topic, sizeof(topic), "%s/%s/lamp/scan", net_prefix(), s_id);
        char *txt = cJSON_PrintUnformatted(root);
        if (txt) {
            publish_text(topic, txt);
            cJSON_free(txt);
        }
        cJSON_Delete(root);
    }
    lamp_scan_running = false;
    vTaskDelete(NULL);
}

static void mqtt_event_handler(void *args, esp_event_base_t base, int32_t event_id, void *data) {
    (void)base;
    const int link = (int)(intptr_t)args;    /* 0 = NAS，1 = 公网直连 */
    esp_mqtt_event_handle_t e = (esp_mqtt_event_handle_t)data;

    taskENTER_CRITICAL(&s_cli_mux);
    s_evt_busy++;
    taskEXIT_CRITICAL(&s_cli_mux);

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED: {
            esp_mqtt_client_handle_t c = s_cli[link];
            if (c == NULL) {
                /* 句柄已经被 stop_link 摘掉了（切换/重启中）：别再去订阅 */
                s_cli_up[link] = false;
                s_connected = any_link_up();
                break;
            }
            s_cli_up[link] = true;
            s_connected = any_link_up();
            s_last_pub_ok_ms = mqtt_now_ms();
            /* 重连后清掉命令去重缓存：断线期间手机可能补发了一条和断线前
             * 一模一样的命令，3 秒窗口会把这次真实操作吞掉。 */
            s_last_len = -1;
            s_last_cmd[0] = '\0';
            snprintf(s_summ, sizeof(s_summ), "link%d up: %s", link, s_uri[link]);
            ESP_LOGI(TAG, "link%d connected %s (sub %s)", link, s_uri[link], s_topic_cmd);
            (void)esp_mqtt_client_subscribe(c, s_topic_cmd, 0);
            publish_discovery();
            publish_caps();
            publish_state();
            break;
        }
        case MQTT_EVENT_DISCONNECTED:
            s_cli_up[link] = false;
            s_connected = any_link_up();
            snprintf(s_summ, sizeof(s_summ), "link%d down: %s", link, s_uri[link]);
            ESP_LOGW(TAG, "link%d disconnected (%s)", link, s_uri[link]);
            break;
        case MQTT_EVENT_DATA:
            /* 必须比"主题长度 + 内容"：e->topic 不带 \0，
             * 只按 e->topic_len 去 strncmp 定长串，短主题会被误判命中，
             * 长主题会读过 s_topic_cmd 的结尾。 */
            if (e->topic && e->topic_len == (int)strlen(s_topic_cmd) &&
                memcmp(e->topic, s_topic_cmd, (size_t)e->topic_len) == 0) {
                /* 公网直连和 NAS 桥接可能各送一份，去重后再执行 */
                if (!cmd_is_dup(e->data, e->data_len)) {
                    handle_command(e->data, e->data_len);
                }
            }
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGW(TAG, "link%d mqtt error", link);
            break;
        default:
            break;
    }

    taskENTER_CRITICAL(&s_cli_mux);
    s_evt_busy--;
    taskEXIT_CRITICAL(&s_cli_mux);
}


/* ---------------- 能力清单（caps） ---------------- */

/**
 * @brief 把自己支持的传感器/执行器广播出去（retain）
 *
 * App 收到后动态生成界面，因此以后加传感器或执行器：
 *   - 只改固件
 *   - 不需要改网页，也不需要重新打 APK
 */
static void publish_caps(void) {
    if (!s_connected) {
        return;
    }
    cJSON *o = cJSON_CreateObject();
    if (!o) {
        return;
    }
    cJSON_AddNumberToObject(o, "ver", 3);

    cJSON *sa = cJSON_AddArrayToObject(o, "sensors");
    for (int i = 0; i < SH_COUNT; i++) {
        cJSON *it = cJSON_CreateObject();
        if (!it) {
            break;
        }
        cJSON_AddStringToObject(it, "id", sensor_hub_id_name((sensor_id_t)i));
        cJSON_AddStringToObject(it, "n", sensor_hub_label((sensor_id_t)i));
        cJSON_AddStringToObject(it, "u", sensor_hub_unit((sensor_id_t)i));
        cJSON_AddNumberToObject(it, "d", sensor_hub_decimals((sensor_id_t)i));
        cJSON_AddNumberToObject(it, "p", (double)sensor_hub_period_ms((sensor_id_t)i));
        cJSON_AddItemToArray(sa, it);
    }

    cJSON *aa = cJSON_AddArrayToObject(o, "actuators");
    for (int i = 0; i < act_registry_count(); i++) {
        const act_desc_t *d = act_registry_at(i);
        if (!d) {
            continue;
        }
        cJSON *it = cJSON_CreateObject();
        if (!it) {
            break;
        }
        cJSON_AddStringToObject(it, "id", d->id);
        cJSON_AddStringToObject(it, "n", d->label);
        cJSON_AddNumberToObject(it, "kind", d->kind);
        cJSON_AddNumberToObject(it, "min", d->min_pct);
        cJSON_AddNumberToObject(it, "max", d->max_pct);
        cJSON_AddItemToArray(aa, it);
    }

    cJSON *fa = cJSON_AddArrayToObject(o, "features");
    const char *feats[] = { "schedule", "sd", "history", "stream", "caps", "pwm",
                            "netlea", "lamp_src" };
    for (size_t i = 0; i < sizeof(feats) / sizeof(feats[0]); i++) {
        cJSON_AddItemToArray(fa, cJSON_CreateString(feats[i]));
    }

    char *js = cJSON_PrintUnformatted(o);
    if (js) {
        publish_ex(s_topic_caps, js, 1);      /* retain */
        cJSON_free(js);
    }
    cJSON_Delete(o);
    ESP_LOGI(TAG, "caps published");
}

/* ---------------- 历史数据（独立任务） ---------------- */

/**
 * @brief 在独立任务里读取 SD 历史并发布
 *
 * history_build_json() 要读 48KB 文件，如果直接放在 MQTT 事件任务里，
 * 读取期间所有命令都要排队 —— 用户的表现就是"点了没反应"。
 */
static SemaphoreHandle_t s_hist_lock = NULL;

static void hist_task(void *arg) {
    (void)arg;
    static char hbuf[8192];          /* 只在持锁期间访问 */
    uint32_t t0 = (uint32_t)(esp_timer_get_time() / 1000);

    /* BUGFIX：原来每条 hist 命令都新建任务、共用同一块 static 缓冲，
     * 两条命令挨得近（多个手机、页签反复切换）就会互相写坏，
     * 手机解析失败。这里用互斥锁串行化，重入直接回 busy。 */
    if (s_hist_lock == NULL) {
        s_hist_lock = xSemaphoreCreateMutex();
    }
    if (s_hist_lock != NULL && xSemaphoreTake(s_hist_lock, 0) != pdTRUE) {
        ack_hint("hist", false, "busy", "正在生成历史数据，请稍后再试");
        vTaskDelete(NULL);
        return;
    }

    esp_err_t hr = history_build_json(hbuf, sizeof(hbuf), 120);
    uint32_t t1 = (uint32_t)(esp_timer_get_time() / 1000);
    if (hr == ESP_OK) {
        publish_text(s_topic_ack, hbuf);
    } else {
        ESP_LOGW(TAG, "history build failed: %s", esp_err_to_name(hr));
        ack_hint("hist", false, "no_history",
                 hr == ESP_ERR_NOT_FOUND ? "没有SD卡或还没有数据"
                 : (hr == ESP_ERR_TIMEOUT ? "正在读取历史，请稍后再试" : "读取历史失败"));
    }
    uint32_t t2 = (uint32_t)(esp_timer_get_time() / 1000);
    ESP_LOGI(TAG, "hist done: build=%ums publish=%ums bytes=%u",
             (unsigned)(t1 - t0), (unsigned)(t2 - t1), (unsigned)strlen(hbuf));

    if (s_hist_lock != NULL) {
        xSemaphoreGive(s_hist_lock);
    }
    vTaskDelete(NULL);
}

/* ---------------- 设备广播 ---------------- */

/**
 * @brief 周期性广播自己的身份，让手机 App 能"搜索到设备"
 *
 * 用 retain=1 发布，这样 App 稍后订阅也能立刻收到最后一条，
 * 不用等到下一个 30 秒周期。
 */
static void publish_discovery(void) {
    if (!s_connected) {
        return;
    }
    cJSON *o = cJSON_CreateObject();
    if (!o) {
        return;
    }
    cJSON_AddStringToObject(o, "id", s_id);
    cJSON_AddStringToObject(o, "name", SYSTEM_NAME);
    cJSON_AddStringToObject(o, "ip", net_ip());
    cJSON_AddStringToObject(o, "mode", control_mode_name());
    char *js = cJSON_PrintUnformatted(o);
    if (js) {
        publish_ex(s_topic_disc, js, 1);      /* retain=1 */
        cJSON_free(js);
    }
    cJSON_Delete(o);
}

static void pub_task(void *arg) {
    (void)arg;
    uint32_t disc_tick = 0;

    while (1) {
        /* 曲线缓存：不管有没有连上 MQTT 都要记，掉线期间的曲线才是连续的 */
        {
            const sensor_sample_t *st = sensor_hub_get(SH_TEMP);
            const sensor_sample_t *sp = sensor_hub_get(SH_PH);
            const sensor_sample_t *sl = sensor_hub_get(SH_LEVEL);
            const sensor_sample_t *si = sensor_hub_get(SH_LIGHT);
            history_note_sample(
                (st && st->err == ESP_OK) ? st->value : -1.0f,
                (sp && sp->err == ESP_OK) ? sp->value : -1.0f,
                (sl && sl->err == ESP_OK) ? sl->value : -1.0f,
                (si && si->err == ESP_OK) ? si->value : -1.0f);
        }
        if (s_connected) {
            publish_state();
            /* 每 15 个上报周期（30 秒）广播一次身份，供手机 App 发现设备 */
            if ((disc_tick++ % 15u) == 0u) {
                publish_discovery();
                publish_caps();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(PUB_PERIOD_MS));
    }
    s_pub_task = NULL;
    vTaskDelete(NULL);
}

/* 开机 20 秒后在后台把 SD 历史读进缓存：
 * SD 读 48KB 要 9 秒，放在手机请求的路径上就是"曲线半天不出来"。
 * 这里提前预热，用户打开 App 时 hist 就是毫秒级响应。
 *
 * 注意要重试：刚开机那会儿 SD 可能还没挂载完（实测 6.8s 才挂上），
 * 一次失败就放弃的话缓存里永远只剩实时点，曲线就只有最近 1 分钟。 */
static void hist_warm_task(void *arg) {
    (void)arg;
    static char wbuf[8192];
    vTaskDelay(pdMS_TO_TICKS(15000));
    for (int i = 0; i < 8; i++) {
        esp_err_t r = history_build_json(wbuf, sizeof(wbuf), 120);
        if (r == ESP_OK) {
            ESP_LOGI(TAG, "history prewarm ok at try %d (%u bytes)",
                     i + 1, (unsigned)strlen(wbuf));
            break;
        }
        ESP_LOGW(TAG, "history prewarm try %d: %s", i + 1, esp_err_to_name(r));
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
    vTaskDelete(NULL);
}

/* ---------------- 单活链路管理 ---------------- */

static void start_link(int i) {
    if (i < 0 || i >= MQTT_LINKS || s_uri[i][0] == '\0') {
        return;
    }
    if (s_cli[i]) {
        return;
    }
    /* 两条链路的 client id 必须不同，否则同一个 broker 上会互相踢下线 */
    char client_id[40];
    snprintf(client_id, sizeof(client_id), "fishtank-%s%s", s_id, i ? "-b" : "");

    /* 只有 NAS 那条需要账号密码；公网隧道转发到同一台 NAS，也要账号密码 */
    const char *mu = net_mqtt_user();
    const char *mp = net_mqtt_pass();

    /* ws:// 走 WebSocket 传输：板子在学校/外面时，
     * 用它连 NAS 上那台 broker 的 8083 端口（就是手机用的同一条 PassNAT 隧道），
     * 路径固定 /mqtt。这样不需要额外开一条裸 TCP 隧道。 */
    static char s_ws_path[] = "/mqtt";
    const bool is_ws = (strncmp(s_uri[i], "ws://", 6) == 0) ||
                       (strncmp(s_uri[i], "wss://", 7) == 0);

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = s_uri[i],
        .broker.address.path = is_ws ? s_ws_path : NULL,
        .credentials.client_id = client_id,
        .credentials.username = (mu && mu[0]) ? mu : NULL,
        .credentials.authentication.password = (mp && mp[0]) ? mp : NULL,
        /* keepalive 由 60s 降到 30s：手机/隧道断掉时，MQTT 靠 PINGRESP 超时
         * 才发现链路已死。60s 意味着最长要 90s 才触发切换，手机上就是
         * "明明没网了，界面还挂着已连接"。30s 把最坏情况压到 ~45s，
         * 每 15s 一个 2 字节 PINGREQ，对 WiFi 流量没有影响。 */
        .session.keepalive = 30,
        .network.reconnect_timeout_ms = 2000,
        .network.timeout_ms = 10000,
        .buffer.size = 4096,
        .buffer.out_size = 4096,
        /* 事件任务里要跑命令解析 + 组 JSON，6144 的默认栈在深层回调下偏紧，
         * 这里显式给 8KB（每路一份，最多两路）。 */
        .task.stack_size = 8192,
        .task.priority = 6,
    };

    esp_mqtt_client_handle_t cli = esp_mqtt_client_init(&cfg);
    if (!cli) {
        ESP_LOGE(TAG, "link%d init failed (%s)", i, s_uri[i]);
        return;
    }
    esp_mqtt_client_register_event(cli, ESP_EVENT_ANY_ID, mqtt_event_handler,
                                   (void *)(intptr_t)i);
    /* 先挂进数组再 start：start 会马上起任务，局域网下 CONNECTED 事件
     * 可能在几毫秒内就到来，那时如果 s_cli[i] 还是 NULL，
     * 事件回调里的 subscribe(主题) 会直接失败 —— 之后再也收不到命令了。 */
    taskENTER_CRITICAL(&s_cli_mux);
    s_cli[i] = cli;
    taskEXIT_CRITICAL(&s_cli_mux);
    esp_err_t ret = esp_mqtt_client_start(cli);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "link%d start failed: %s", i, esp_err_to_name(ret));
        taskENTER_CRITICAL(&s_cli_mux);
        if (s_cli[i] == cli) {
            s_cli[i] = NULL;
        }
        taskEXIT_CRITICAL(&s_cli_mux);
        esp_mqtt_client_destroy(cli);
        return;
    }
    s_last_pub_ok_ms = mqtt_now_ms();
    ESP_LOGI(TAG, "link%d starting: %s (id=%s)", i, s_uri[i], client_id);
}

static void stop_link(int i) {
    if (i < 0 || i >= MQTT_LINKS) {
        return;
    }
    esp_mqtt_client_handle_t c = NULL;
    taskENTER_CRITICAL(&s_cli_mux);
    c = s_cli[i];
    s_cli[i] = NULL;          /* 新发布者立刻看不到它 */
    s_cli_up[i] = false;
    taskEXIT_CRITICAL(&s_cli_mux);
    if (!c) {
        s_connected = any_link_up();
        return;
    }
    /* 等还在路上的 publish / 事件回调走完再释放句柄（单次 publish 最长会卡
     * network.timeout_ms=10s，事件回调里也可能正在解析这条报文，所以给 12s）。 */
    for (int k = 0; k < 1200 && (s_pub_busy[i] > 0 || s_evt_busy > 0); k++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_pub_busy[i] > 0 || s_evt_busy > 0) {
        ESP_LOGW(TAG, "link%d still busy after 12s (pub=%d evt=%d), destroy anyway",
                 i, (int)s_pub_busy[i], (int)s_evt_busy);
    }
    esp_mqtt_client_stop(c);
    esp_mqtt_client_destroy(c);
    s_connected = any_link_up();
}

/**
 * @brief 链路看门狗：局域网连不上就切公网隧道，隧道断了再试着切回来
 *
 * 为什么需要：板子可能在家（能直连 NAS）也可能在学校（只能走公网）。
 * 判断依据不只是"MQTT 有没有连上"，还要看"最近有没有真的发出去"：
 * PassNAT 隧道假活（TCP 在、数据不通）时 s_cli_up 一直是 true，
 * 只看连接状态会永远卡在一条发不出数据的链路上。
 */
static void link_watch_task(void *arg) {
    (void)arg;
    uint32_t down_ms = 0;
    uint32_t wan_ticks = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(3000));
        if (!s_started) {
            continue;
        }
        if (s_uri[1][0] == '\0') {
            continue;                       /* 没配第二路，不用管 */
        }

        const bool healthy = s_connected &&
                             (uint32_t)(mqtt_now_ms() - s_last_pub_ok_ms) < 20000u;
        if (healthy) {
            down_ms = 0;
            /* 双路都在线：优先用家里的局域网（少两跳，延迟 16ms vs 44ms）。
             * 只在"隧道也正常"的时候切，避免刚切到 LAN 又立刻掉回去。 */
            if (s_active_link == 1 && s_cli_up[0]) {
                ESP_LOGW(TAG, "LAN link is back, switching off the tunnel");
                stop_link(1);
                s_active_link = 0;
            } else if (s_active_link == 1 && (wan_ticks++ % 200u) == 0u) {
                /* 待在隧道上每 10 分钟探一次局域网：
                 * 用户从学校回到家后，不该一直绕公网走。 */
                ESP_LOGI(TAG, "probing LAN link while on tunnel");
                start_link(0);
            }
            continue;
        }
        wan_ticks = 0;
        down_ms += 3000;
        if (s_active_link == 0 && down_ms >= 12000) {
            ESP_LOGW(TAG, "局域网 broker 12s 不可用，切到公网隧道");
            stop_link(0);
            s_active_link = 1;
            start_link(1);
            down_ms = 0;
        } else if (s_active_link == 1 && down_ms >= 30000) {
            ESP_LOGW(TAG, "公网隧道 30s 不可用，切回局域网试试");
            stop_link(1);
            s_active_link = 0;
            start_link(0);
            down_ms = 0;
        }
    }
}

esp_err_t mqtt_app_start(void) {
    if (s_started) {
        return ESP_OK;
    }

    /* client id / 主题里用 MAC 后 3 字节，避免多台设备撞车 */
    uint8_t mac[6] = {0};
    (void)esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_id, sizeof(s_id), "ft%02x%02x%02x", mac[3], mac[4], mac[5]);
    build_topics();

    /* 两路 broker：0 = NAS（局域网），1 = 公网直连 */
    snprintf(s_uri[0], sizeof(s_uri[0]), "%s", net_mqtt_uri());
    snprintf(s_uri[1], sizeof(s_uri[1]), "%s", net_mqtt2_uri());
    if (s_uri[1][0] == '\0' || strcmp(s_uri[0], s_uri[1]) == 0) {
        s_uri[1][0] = '\0';        /* 没配第二条，或者两条其实是同一个 broker */
    }

    /* 只起一路：先试局域网地址；连不上由 link_watch 切到公网隧道。
     * （两个地址是同一台 broker，同时连会重复发布状态） */
    start_link(0);
    s_active_link = 0;
    int started = s_cli[0] ? 1 : 0;

    if (started == 0) {
        ESP_LOGE(TAG, "no mqtt link could start");
        return ESP_FAIL;
    }
    s_started = true;

    sd_logger_set_line_cb(sd_line_publisher);

    snprintf(s_summ, sizeof(s_summ), "starting -> %s", s_uri[0]);
    ESP_LOGI(TAG, "mqtt started: %d link(s), first=%s, state topic=%s",
             started, s_uri[0], s_topic_state);

    if (!s_pub_task) {
        xTaskCreate(pub_task, "mqtt_pub", 4096, NULL, 3, &s_pub_task);
    }
    /* 预热曲线缓存（一次性任务，跑完自己退出） */
    xTaskCreate(hist_warm_task, "hist_warm", 8192, NULL, 2, NULL);
    /* 后台 NVS 落盘（常驻） */
    xTaskCreate(save_task, "nvs_save", 4096, NULL, 1, NULL);
    /* 链路看门狗：局域网不通自动切公网隧道 */
    xTaskCreate(link_watch_task, "mqtt_watch", 3072, NULL, 2, NULL);
    return ESP_OK;
}

void mqtt_app_stop(void) {
    /* 走 stop_link()：它会等 in-flight 的 publish/事件回调退出再 destroy，
     * 直接在这里 stop+destroy 会和 sd_logger / mqtt_pub 抢句柄。 */
    s_started = false;
    for (int i = 0; i < MQTT_LINKS; i++) {
        stop_link(i);
    }
    s_connected = false;
}

bool mqtt_app_connected(void) {
    return s_connected;
}

const char *mqtt_app_status(void) {
    return s_summ;
}

void mqtt_app_publish_now(void) {
    publish_state();
}
