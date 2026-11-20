/**
 * @file netlea_transport.h
 * @brief 尼特利灯的传输层抽象
 *
 * 真正的灯是 BLE GATT 外设（FFFA/FF02/FF01），但为了"没有实体灯也能把
 * 整条控制链路跑起来/测起来"，这里把传输层抽出来：
 *   - netlea_ble.c  ：真实 BLE（需要 CONFIG_BT_ENABLED=y）
 *   - netlea_sim.c  ：内置模拟灯（解析收到的帧，回一帧 CMD20 状态）
 *
 * netlea_lamp.c 只跟本接口打交道，不关心底下是蓝牙还是模拟器。
 */
#ifndef NETLEA_TRANSPORT_H
#define NETLEA_TRANSPORT_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*netlea_rx_cb_t)(const uint8_t *frame, size_t len);

#define NETLEA_SCAN_MAX 24
/** 可连接的 BLE 候选设备 */
typedef struct {
    uint8_t mac[6];
    int8_t rssi;
    char name[24];
} netlea_scan_item_t;

/** 设置/读取要连接的灯（掉电由上层保存） */
void netlea_transport_set_target(const uint8_t mac[6]);
void netlea_transport_get_target(uint8_t mac[6]);
/** 开始一次扫描（会先断开当前连接，因为灯"一灯一主机"） */
void netlea_transport_scan_start(void);
/** 取扫描结果；返回条数 */
int netlea_transport_scan_items(netlea_scan_item_t *out, int max);
/** 扫描是否已结束（BLE 需要等它自己停） */
bool netlea_transport_scan_done(void);

/** 初始化传输层（BLE 初始化失败不算错，会回退到模拟器） */
esp_err_t netlea_transport_init(void);
/** 是否可用（BLE 已连上 / 模拟器已启用） */
bool netlea_transport_ready(void);
/** "ble" / "sim" / "off" */
const char *netlea_transport_name(void);
/** 发一帧（整帧，含 0x5A 与校验和） */
esp_err_t netlea_transport_write(const uint8_t *buf, size_t len);
/** 注册接收回调（收到 FF01 通知 / 模拟器的回声） */
void netlea_transport_set_rx(netlea_rx_cb_t cb);
/** 周期驱动（BLE 扫描/重连；模拟器下为空） */
void netlea_transport_tick(void);
/** 强制使用模拟器（1）或尝试真实 BLE（0） */
void netlea_transport_force_sim(bool on);
bool netlea_transport_is_sim(void);

#ifdef __cplusplus
}
#endif

#endif // NETLEA_TRANSPORT_H
