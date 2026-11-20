#include "debug/debug_protocol.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* 行接收                                                              */
/* ------------------------------------------------------------------ */

void dbg_rx_reset(dbg_rx_t *rx) {
    if (!rx) {
        return;
    }
    rx->len = 0;
    rx->overflow = false;
    rx->buf[0] = '\0';
}

dbg_rx_result_t dbg_rx_push(dbg_rx_t *rx, char c) {
    if (!rx) {
        return DBG_RX_MORE;
    }

    if (c == '\r') {
        return DBG_RX_MORE;
    }

    if (c == '\n') {
        const bool overflowed = rx->overflow;
        rx->buf[rx->len < DBG_LINE_MAX ? rx->len : (DBG_LINE_MAX - 1)] = '\0';
        rx->len = 0;
        rx->overflow = false;
        return overflowed ? DBG_RX_OVERFLOW : DBG_RX_LINE;
    }

    if (rx->overflow) {
        /* 已经超长，丢弃到行尾为止 */
        return DBG_RX_MORE;
    }

    if (rx->len >= (DBG_LINE_MAX - 1)) {
        rx->overflow = true;
        /* 保留已收到的内容用于报错提示，但不再追加 */
        rx->buf[DBG_LINE_MAX - 1] = '\0';
        return DBG_RX_MORE;
    }

    rx->buf[rx->len++] = c;
    return DBG_RX_MORE;
}

/* ------------------------------------------------------------------ */
/* 字段切分                                                            */
/* ------------------------------------------------------------------ */

static char *trim_inplace(char *s) {
    if (!s) {
        return s;
    }
    while (*s != '\0' && isspace((unsigned char)*s)) {
        s++;
    }
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) {
        s[--n] = '\0';
    }
    return s;
}

void dbg_fields_split(char *line, dbg_fields_t *out) {
    if (!out) {
        return;
    }
    out->count = 0;
    for (int i = 0; i < DBG_MAX_FIELDS; i++) {
        out->field[i] = NULL;
    }
    if (!line) {
        return;
    }

    char *p = line;
    while (p && *p != '\0' && out->count < DBG_MAX_FIELDS) {
        char *comma = strchr(p, ',');
        if (comma) {
            *comma = '\0';
        }
        out->field[out->count] = trim_inplace(p);
        out->count++;
        p = comma ? (comma + 1) : NULL;
    }

    /* 支持省略 '@' 前缀：首字段以 '@' 开头则去掉 */
    if (out->count > 0 && out->field[0] && out->field[0][0] == '@') {
        out->field[0] = trim_inplace(out->field[0] + 1);
    }
}

const char *dbg_fields_at(const dbg_fields_t *f, int idx) {
    if (!f || idx < 0 || idx >= f->count) {
        return NULL;
    }
    return f->field[idx];
}

bool dbg_kv_split(const char *field, char *key, size_t key_len, char *val, size_t val_len) {
    if (!field || !key || !val || key_len == 0 || val_len == 0) {
        return false;
    }
    const char *eq = strchr(field, '=');
    if (!eq) {
        return false;
    }

    size_t klen = (size_t)(eq - field);
    while (klen > 0 && isspace((unsigned char)field[klen - 1])) {
        klen--;
    }
    if (klen >= key_len) {
        klen = key_len - 1;
    }
    memcpy(key, field, klen);
    key[klen] = '\0';

    const char *v = eq + 1;
    while (*v != '\0' && isspace((unsigned char)*v)) {
        v++;
    }
    size_t vlen = strlen(v);
    while (vlen > 0 && isspace((unsigned char)v[vlen - 1])) {
        vlen--;
    }
    if (vlen >= val_len) {
        vlen = val_len - 1;
    }
    memcpy(val, v, vlen);
    val[vlen] = '\0';

    return (klen > 0);
}

const char *dbg_kv_find(const dbg_fields_t *f, const char *key) {
    if (!f || !key) {
        return NULL;
    }
    static char s_val[DBG_VAL_MAX];   /* 单线程调用（调试控制台任务） */
    char k[DBG_KEY_MAX];
    for (int i = 0; i < f->count; i++) {
        if (!f->field[i]) {
            continue;
        }
        if (dbg_kv_split(f->field[i], k, sizeof(k), s_val, sizeof(s_val)) && dbg_streq_ci(k, key)) {
            return s_val;
        }
    }
    return NULL;
}

bool dbg_streq_ci(const char *a, const char *b) {
    if (a == NULL || b == NULL) {
        return false;
    }
    while (*a != '\0' && *b != '\0') {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return false;
        }
        a++;
        b++;
    }
    return (*a == '\0' && *b == '\0');
}

/* ------------------------------------------------------------------ */
/* 数值解析                                                            */
/* ------------------------------------------------------------------ */

bool dbg_parse_float(const char *s, float *out) {
    if (!s || !out || *s == '\0') {
        return false;
    }
    char *end = NULL;
    const double v = strtod(s, &end);
    if (end == s) {
        return false;
    }
    while (end && *end != '\0' && isspace((unsigned char)*end)) {
        end++;
    }
    if (end && *end != '\0') {
        return false;
    }
    if (!(v == v)) {   /* NaN */
        return false;
    }
    if (v > 1e12 || v < -1e12) {
        return false;
    }
    *out = (float)v;
    return true;
}

bool dbg_parse_i32(const char *s, int32_t *out) {
    if (!s || !out || *s == '\0') {
        return false;
    }
    char *end = NULL;
    const long v = strtol(s, &end, 10);
    if (end == s) {
        return false;
    }
    while (end && *end != '\0' && isspace((unsigned char)*end)) {
        end++;
    }
    if (end && *end != '\0') {
        return false;
    }
    *out = (int32_t)v;
    return true;
}

bool dbg_parse_u32(const char *s, uint32_t *out) {
    if (!s || !out || *s == '\0') {
        return false;
    }
    while (*s != '\0' && isspace((unsigned char)*s)) {
        s++;
    }
    /* strtoul("-1") 会回绕成 4294967295，这里显式拒绝负号 */
    if (*s == '-') {
        return false;
    }
    char *end = NULL;
    const unsigned long v = strtoul(s, &end, 10);
    if (end == s) {
        return false;
    }
    while (end && *end != '\0' && isspace((unsigned char)*end)) {
        end++;
    }
    if (end && *end != '\0') {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

bool dbg_parse_bool(const char *s, bool *out) {
    if (!s || !out) {
        return false;
    }
    if (dbg_streq_ci(s, "1") || dbg_streq_ci(s, "on") || dbg_streq_ci(s, "true") ||
        dbg_streq_ci(s, "yes") || dbg_streq_ci(s, "enable") || dbg_streq_ci(s, "open")) {
        *out = true;
        return true;
    }
    if (dbg_streq_ci(s, "0") || dbg_streq_ci(s, "off") || dbg_streq_ci(s, "false") ||
        dbg_streq_ci(s, "no") || dbg_streq_ci(s, "disable") || dbg_streq_ci(s, "close")) {
        *out = false;
        return true;
    }
    /* 允许直接写数字 */
    float v = 0.0f;
    if (dbg_parse_float(s, &v)) {
        *out = (v != 0.0f);
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* 输出行构造                                                          */
/* ------------------------------------------------------------------ */

const char *dbg_bool_str(bool v) {
    return v ? "1" : "0";
}

static void out_putc(dbg_out_t *o, char c) {
    if (!o || !o->buf || o->cap == 0) {
        return;
    }
    if (o->len + 1 >= o->cap) {
        o->truncated = true;
        o->buf[o->cap - 1] = '\0';
        return;
    }
    o->buf[o->len++] = c;
    o->buf[o->len] = '\0';
}

static void out_puts(dbg_out_t *o, const char *s) {
    if (!o || !s) {
        return;
    }
    while (*s != '\0') {
        out_putc(o, *s++);
    }
}

static void out_begin_field(dbg_out_t *o) {
    if (!o) {
        return;
    }
    if (o->fields > 0) {
        out_putc(o, ',');
    }
    o->fields++;
}

void dbg_out_init(dbg_out_t *o, char *buf, size_t cap, const char *verb) {
    if (!o) {
        return;
    }
    o->buf = buf;
    o->cap = cap;
    o->len = 0;
    o->fields = 0;
    o->truncated = false;
    if (buf && cap > 0) {
        buf[0] = '\0';
    }
    out_putc(o, '#');
    if (verb && verb[0] != '\0') {
        out_puts(o, verb);
        o->fields = 1;
    }
}

void dbg_out_raw(dbg_out_t *o, const char *text) {
    if (!o || !text) {
        return;
    }
    out_begin_field(o);
    out_puts(o, text);
}

void dbg_out_kv_str(dbg_out_t *o, const char *key, const char *value) {
    if (!o || !key) {
        return;
    }
    out_begin_field(o);
    out_puts(o, key);
    out_putc(o, '=');
    out_puts(o, value ? value : "");
}

void dbg_out_kv_i(dbg_out_t *o, const char *key, long value) {
    if (!o || !key) {
        return;
    }
    char tmp[DBG_VAL_MAX];
    snprintf(tmp, sizeof(tmp), "%ld", value);
    dbg_out_kv_str(o, key, tmp);
}

void dbg_out_kv_u(dbg_out_t *o, const char *key, unsigned long value) {
    if (!o || !key) {
        return;
    }
    char tmp[DBG_VAL_MAX];
    snprintf(tmp, sizeof(tmp), "%lu", value);
    dbg_out_kv_str(o, key, tmp);
}

void dbg_out_kv_f(dbg_out_t *o, const char *key, float value, int decimals) {
    if (!o || !key) {
        return;
    }
    if (decimals < 0) {
        decimals = 0;
    }
    if (decimals > 6) {
        decimals = 6;
    }
    char tmp[DBG_VAL_MAX];
    snprintf(tmp, sizeof(tmp), "%.*f", decimals, (double)value);
    dbg_out_kv_str(o, key, tmp);
}

void dbg_out_kv_bool(dbg_out_t *o, const char *key, bool value) {
    dbg_out_kv_str(o, key, dbg_bool_str(value));
}

void dbg_out_fmt(dbg_out_t *o, const char *fmt, ...) {
    if (!o || !fmt) {
        return;
    }
    char tmp[DBG_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    dbg_out_raw(o, tmp);
}

const char *dbg_out_str(const dbg_out_t *o) {
    if (!o || !o->buf) {
        return "";
    }
    return o->buf;
}
