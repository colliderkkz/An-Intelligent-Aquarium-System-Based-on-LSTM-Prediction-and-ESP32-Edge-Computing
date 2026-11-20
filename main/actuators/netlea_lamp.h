/**
 * @file netlea_lamp.h
 * @brief 尼特利（Netlea）BLE 水草灯控制
 *
 * 灯的协议是明文 BLE GATT（详见 docs/ESP32_尼特利蓝牙_交接文档.md）：
 *   服务 FFFA / 写 FF02(write-no-response) / 通知 FF01
 *   帧 5A | LEN | DEV(01) | CMD | 00 | B5 | payload | SUM
 *   通道 R W G B F（前四路 0-100，F=风扇 0-255）
 *
 * 设计要点：
 *  - 本模块只提供"灯"的语义（开关 / 亮度 / 颜色 / 风扇 / 上报状态）；
 *  - 传输层可切换：真实 BLE（CONFIG_BT_ENABLED）或内置模拟器；
 *    没有灯/没开蓝牙时自动走模拟器，整套控制链路照样可测；
 *  - 与板载 PWM 灯（GPIO16）二选一，由 lamp_source 决定谁亮；
 *    light_actuator_set_brightness() 会自动路由到当前光源，所以
 *    计划表、手动保持、App 滑条等**原有逻辑一行都不用改**。
 */
#ifndef NETLEA_LAMP_H
#define NETLEA_LAMP_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include "actuators/netlea_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 光源选择 */
#define LAMP_SRC_BOARD   0   /**< 板载 PWM 灯（GPIO16，只有亮度） */
#define LAMP_SRC_NETLEA  1   /**< 尼特利 BLE 灯（RGB+W+F） */

typedef struct {
    bool on;
    uint8_t bright;     /**< 总亮度 0-100（映射到四路） */
    uint8_t r, g, b, w; /**< 单通道 0-100（bright = 100 时的目标色） */
    uint8_t fan;        /**< 风扇 0-255 */
    uint8_t mode_id;    /**< 光谱模式，手动=1，自定义=12 */
    uint8_t grade;      /**< 档位 */
} netlea_state_t;

/** 初始化：读 NVS、选传输层、起任务。失败不致命（会退化为模拟器）。 */
esp_err_t netlea_lamp_init(void);

/** 当前选中的光源（0=板载 1=尼特利） */
int  lamp_source_get(void);
/** 切换光源：会把另一路关掉，并保存到 NVS */
esp_err_t lamp_source_set(int src);

/** 传输层是否就绪（BLE 已连上，或模拟器启用） */
bool netlea_lamp_ready(void);
/** 当前传输层名字："ble" / "sim" / "off" */
const char *netlea_lamp_transport(void);
/** 是否强制使用模拟器（1）——没有实体灯时用来做全链路测试 */
esp_err_t netlea_lamp_set_sim(int on);

/** 亮度（0-100）；由 light_actuator 委托调用，也可单独用 */
esp_err_t netlea_lamp_set_level(uint8_t pct);
/** 颜色（四路各自 0-100），自动开灯 */
esp_err_t netlea_lamp_set_rgbw(uint8_t r, uint8_t g, uint8_t b, uint8_t w);
/** 风扇 0-255 */
esp_err_t netlea_lamp_set_fan(uint8_t f);
/** 开关 */
esp_err_t netlea_lamp_power(bool on);

/** 最近一次"灯回报"的状态（CMD20 解析结果或模拟器回声） */
void netlea_lamp_reported(netlea_state_t *out);
/** 最近一次"我们下发"的状态 */
void netlea_lamp_target(netlea_state_t *out);

/** 主动查询一次（发 CMD27） */
esp_err_t netlea_lamp_query(void);
/** 保存到 NVS */
esp_err_t netlea_lamp_save(void);
/** 连接/重连（BLE 掉线后调用；模拟器下是空操作） */
esp_err_t netlea_lamp_reconnect(void);

#ifdef __cplusplus
}
#endif


/* ---------------- 灯具搜索与绑定 ---------------- */
esp_err_t netlea_lamp_scan_start(void);
int netlea_lamp_scan_items(netlea_scan_item_t *out, int max);
bool netlea_lamp_scan_done(void);
esp_err_t netlea_lamp_bind(const char *mac);
const char *netlea_lamp_mac_str(void);

#endif // NETLEA_LAMP_H
