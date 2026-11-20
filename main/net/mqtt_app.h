/**
 * @file mqtt_app.h
 * @brief MQTT 远程访问：状态上报 + 远程控制
 *
 * 主题（<prefix> 默认 fishtank，<id> 是芯片 MAC 后 3 字节）：
 *   <prefix>/<id>/state   板子 -> 云端，每 5 秒一份 JSON
 *   <prefix>/<id>/cmd     云端 -> 板子，控制命令 JSON
 *   <prefix>/<id>/ack     板子 -> 云端，命令执行结果
 *
 * 这样手机端只要能连 MQTT（App 或网页版 MQTT.js）就能在外网查看和控制，
 * 不需要公网 IP、不用改路由器、不用内网穿透。
 */
#ifndef MQTT_APP_H
#define MQTT_APP_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化并启动（WiFi 还没连上也会启动，连上后自动重连 broker） */
esp_err_t mqtt_app_start(void);
void mqtt_app_stop(void);

/** 是否已连上 broker */
bool mqtt_app_connected(void);

/** 状态摘要（给 NET,STATUS 用） */
const char *mqtt_app_status(void);

/** 立即发布一次状态 */
void mqtt_app_publish_now(void);

#ifdef __cplusplus
}
#endif

#endif // MQTT_APP_H
