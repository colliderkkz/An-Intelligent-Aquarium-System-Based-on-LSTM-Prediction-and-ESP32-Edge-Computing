/**
 * @file debug_protocol.h
 * @brief 上位机调试串口协议 —— 纯 C 的帧解析 / 字段切分 / key=value 编码
 *
 * 本文件不依赖 ESP-IDF，可以在 PC 上直接编译做单元测试（见 tools/test）。
 *
 * ## 帧格式
 *   PC  -> ESP32 : '@' <VERB> [',' <ARG>]*  '\n'
 *   ESP -> PC    : '#' <VERB> [',' <KEY=VALUE>]*  '\n'      (数据/应答)
 *   ESP -> PC    : '!' <CODE> ',' <MESSAGE>  '\n'           (错误)
 *
 * - 行内字段用逗号分隔，允许字段两侧有空格（会被裁剪）。
 * - 除 '#' 与 '!' 开头的行，其余（例如 ESP_LOG 输出）上位机应忽略。
 * - 上行允许省略 '@' 前缀（方便用串口助手手敲），下行一定带 '#'。
 * - 采用 key=value 而不是纯位置参数，便于后续加字段而不破坏旧版上位机。
 */
#ifndef DEBUG_PROTOCOL_H
#define DEBUG_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DBG_LINE_MAX      192   /**< 单行命令最大长度（不含换行） */
#define DBG_MAX_FIELDS    12    /**< 单行最多字段数 */
#define DBG_KEY_MAX       24
#define DBG_VAL_MAX       48

/** 行接收状态机 */
typedef struct {
    char buf[DBG_LINE_MAX];
    size_t len;
    bool overflow;
} dbg_rx_t;

typedef enum {
    DBG_RX_MORE = 0,     /**< 还没收到完整一行 */
    DBG_RX_LINE = 1,     /**< 收到完整一行，可解析 */
    DBG_RX_OVERFLOW = 2, /**< 该行超长，已丢弃，调用方应回错误 */
} dbg_rx_result_t;

/** 字段视图（指向 rx.buf 内部，原地切分） */
typedef struct {
    char *field[DBG_MAX_FIELDS];
    int count;
} dbg_fields_t;

void dbg_rx_reset(dbg_rx_t *rx);

/**
 * @brief 喂入一个字节
 * @return DBG_RX_LINE 表示收到完整行（rx->buf 为 NUL 结尾的正文）
 */
dbg_rx_result_t dbg_rx_push(dbg_rx_t *rx, char c);

/** 原地按逗号切分（会修改 line，去掉字段首尾空格） */
void dbg_fields_split(char *line, dbg_fields_t *out);

/** 取第 idx 个字段，越界返回 NULL */
const char *dbg_fields_at(const dbg_fields_t *f, int idx);

/** 解析 "key=value"，成功返回 true */
bool dbg_kv_split(const char *field, char *key, size_t key_len, char *val, size_t val_len);

/** 在字段里按 key 找值 */
const char *dbg_kv_find(const dbg_fields_t *f, const char *key);

/** 数值解析（带合法性检查） */
bool dbg_parse_float(const char *s, float *out);
bool dbg_parse_i32(const char *s, int32_t *out);
bool dbg_parse_u32(const char *s, uint32_t *out);
bool dbg_parse_bool(const char *s, bool *out);   /**< on/off 1/0 true/false yes/no */

/** 大小写不敏感比较 */
bool dbg_streq_ci(const char *a, const char *b);

/** 输出行构造器（避免到处 snprintf 拼字符串） */
typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    int fields;        /**< 已写入字段数，用于决定是否加逗号 */
    bool truncated;
} dbg_out_t;

/** 初始化并写入 '#' + verb；verb 可为 NULL/空串（用于续行） */
void dbg_out_init(dbg_out_t *o, char *buf, size_t cap, const char *verb);

/** 以 key=value 追加（自动加分隔逗号） */
void dbg_out_kv_str(dbg_out_t *o, const char *key, const char *value);
void dbg_out_kv_i(dbg_out_t *o, const char *key, long value);
void dbg_out_kv_u(dbg_out_t *o, const char *key, unsigned long value);
void dbg_out_kv_f(dbg_out_t *o, const char *key, float value, int decimals);
void dbg_out_kv_bool(dbg_out_t *o, const char *key, bool value);
/** 追加一段带逗号的原始文本（已含 key=value 语义） */
void dbg_out_raw(dbg_out_t *o, const char *text);
/** 追加格式化文本（作为独立字段） */
void dbg_out_fmt(dbg_out_t *o, const char *fmt, ...);

/** 结果缓冲区内容，保证 NUL 结尾 */
const char *dbg_out_str(const dbg_out_t *o);
/** 协议里用 "1"/"0"，比 "true"/"false" 短 */
const char *dbg_bool_str(bool v);

#ifdef __cplusplus
}
#endif

#endif // DEBUG_PROTOCOL_H
