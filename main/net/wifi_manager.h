/**
 * @file wifi_manager.h
 * @brief WiFi STA 连接管理（PSK / WPA2-Enterprise 双模式），配置存 NVS
 *
 * 为什么单独抽出来：
 *   - 凭据不能写死在固件里（换热点/改密码就得重烧）；
 *   - 学校网是 WPA2-Enterprise(PEAP)，家里/热点是 WPA2-PSK，两种认证要走不同分支；
 *   - 断线要自动重连，且不能阻塞事件循环。
 */
#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 从 NVS 加载配置并初始化 WiFi（不自动连接） */
esp_err_t net_init(void);

/** 连接（需要有 ssid） */
esp_err_t net_connect(void);

/**
 * @brief 后台连接：立刻返回，WiFi 在独立任务里慢慢连
 *
 * 开机流程必须用这个：net_connect() 内部最多要扫 6 个 AP × 8 秒，
 * 放在 app_main 里会把自动控制任务一起拖住几十秒。
 */
esp_err_t net_connect_async(void);

/* ---------------- 多 WiFi 记忆 ----------------
 * 板子可以记住多组 WiFi 账密（家里路由、手机热点、公司…）。
 * 上电/掉线后先扫一遍，只在"当前看得见的"里挑信号最强的连，
 * 一个都看不见就按上次成功的优先顺序逐个试。
 */
int net_wifi_count(void);
const char *net_wifi_ssid_at(int i);
/** 当前正在用/正在试的槽位，-1 表示没匹配上 */
int net_wifi_active_index(void);
esp_err_t net_wifi_add(const char *ssid, const char *pass);
/**
 * @brief 增加一条 WiFi（支持企业网：账号 + 密码）
 * @param user 企业网账号（PEAP 用户名），普通 PSK 传 NULL/""
 * @param ent  1 = WPA2-Enterprise（校园网/企业网），0 = 普通密码
 */
esp_err_t net_wifi_add_ex(const char *ssid, const char *pass,
                          const char *user, int ent);
/** 只改这条的认证类型（0=密码 1=企业网账号），密码不变 */
esp_err_t net_wifi_set_ent(int idx, int ent);
/** 交换两条的顺序（调优先级用，不动密码） */
esp_err_t net_wifi_swap(int a, int b);
/** 该条是否企业网 */
int net_wifi_ent_at(int i);
/** 该条的企业网账号（PSK 时为空串） */
const char *net_wifi_user_at(int i);
esp_err_t net_wifi_del(int idx);
void net_wifi_clear(void);
/** 扫描并挑一个连上；成功返回 true。调用方负责持有 WiFi 状态 */
bool net_wifi_pick_and_connect(void);

/** 主动断开 */
void net_disconnect(void);

/** 是否已拿到 IP */
bool net_wifi_up(void);

/** 当前 IP 字符串（未连接时 "0.0.0.0"） */
const char *net_ip(void);

/** 是否已经配置了 SSID */
bool net_has_config(void);

/**
 * @brief 修改一项网络配置
 * @param key  "ssid" / "user" / "pass" / "ent"(0|1) / "uri" / "prefix"
 * @param val  字符串值
 */
esp_err_t net_set(const char *key, const char *val);

/**
 * @brief 扫描附近 WiFi
 * @param cb  每个非隐藏 AP 调一次
 * @param max 最多回调几个
 * @return 扫到的 AP 总数（<0 表示失败）
 */
int net_scan(void (*cb)(const char *ssid, int rssi, int ch, int auth), int max);

/** 保存到 NVS */
esp_err_t net_save(void);

/** 读取某项配置（返回静态缓冲，仅供立即使用） */
const char *net_get(const char *key);

/** 供 NET,STATUS 输出的一行摘要 */
const char *net_summary(void);

/** MQTT 账号（空表示匿名连接） */
const char *net_mqtt_user(void);
const char *net_mqtt_pass(void);

/** 主题前缀（如 "fishtank"） */
const char *net_prefix(void);

/** MQTT broker URI（如 "mqtt://broker.emqx.io:1883"） */
const char *net_mqtt_uri(void);

/** 第二路 broker URI（公网直连，空字符串表示只走一路） */
const char *net_mqtt2_uri(void);

#ifdef __cplusplus
}
#endif

#endif // WIFI_MANAGER_H
