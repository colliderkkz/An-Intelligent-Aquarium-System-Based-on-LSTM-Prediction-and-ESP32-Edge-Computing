/**
 * @file test_debug_protocol.c
 * @brief debug_protocol.c 的主机侧单元测试（不依赖 ESP-IDF）
 *
 * 编译运行（Windows, MSVC）：
 *   cl /nologo /std:c11 /W4 /I..\..\main test_debug_protocol.c ..\..\main\debug\debug_protocol.c
 *   test_debug_protocol.exe
 */
#include "debug/debug_protocol.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                              \
    do {                                                                          \
        g_checks++;                                                               \
        if (!(cond)) {                                                            \
            g_failures++;                                                         \
            printf("  [FAIL] %s:%d  %s\n", __FILE__, __LINE__, #cond);            \
        }                                                                         \
    } while (0)

#define CHECK_STR(actual, expected)                                              \
    do {                                                                          \
        g_checks++;                                                               \
        const char *a_ = (actual);                                                \
        const char *e_ = (expected);                                              \
        if (strcmp(a_, e_) != 0) {                                                \
            g_failures++;                                                         \
            printf("  [FAIL] %s:%d  got \"%s\" want \"%s\"\n", __FILE__, __LINE__, \
                   a_, e_);                                                       \
        }                                                                         \
    } while (0)

static dbg_rx_result_t feed_line(dbg_rx_t *rx, const char *text) {
    dbg_rx_reset(rx);
    dbg_rx_result_t res = DBG_RX_MORE;
    for (const char *p = text; *p != '\0'; p++) {
        res = dbg_rx_push(rx, *p);
    }
    if (res != DBG_RX_OVERFLOW) {
        res = dbg_rx_push(rx, '\n');
    }
    return res;
}

static void test_frame_parsing(void) {
    printf("[test] frame parsing\n");
    static dbg_rx_t rx;
    dbg_fields_t f;

    CHECK(feed_line(&rx, "@PING") == DBG_RX_LINE);
    dbg_fields_split(rx.buf, &f);
    CHECK(f.count == 1);
    CHECK_STR(f.field[0], "PING");

    CHECK(feed_line(&rx, "@ACT,LIGHT,PWM,50") == DBG_RX_LINE);
    dbg_fields_split(rx.buf, &f);
    CHECK(f.count == 4);
    CHECK_STR(f.field[0], "ACT");
    CHECK_STR(f.field[1], "LIGHT");
    CHECK_STR(f.field[2], "PWM");
    CHECK_STR(f.field[3], "50");

    /* 允许省略 '@' 并带空格 */
    CHECK(feed_line(&rx, " PARAM , GET , temp_target_c ") == DBG_RX_LINE);
    dbg_fields_split(rx.buf, &f);
    CHECK(f.count == 3);
    CHECK_STR(f.field[0], "PARAM");
    CHECK_STR(f.field[2], "temp_target_c");

    /* \r\n 行尾只当一行 */
    dbg_rx_reset(&rx);
    const char *crlf = "@RTC,GET\r\n";
    dbg_rx_result_t last = DBG_RX_MORE;
    for (const char *p = crlf; *p; p++) {
        last = dbg_rx_push(&rx, *p);
    }
    CHECK(last == DBG_RX_LINE);
    CHECK_STR(rx.buf, "@RTC,GET");
}

static void test_overflow(void) {
    printf("[test] overflow\n");
    static dbg_rx_t rx;
    dbg_rx_reset(&rx);
    dbg_rx_result_t res = DBG_RX_MORE;
    for (int i = 0; i < DBG_LINE_MAX + 40; i++) {
        res = dbg_rx_push(&rx, 'A');
    }
    CHECK(res == DBG_RX_MORE);
    res = dbg_rx_push(&rx, '\n');
    CHECK(res == DBG_RX_OVERFLOW);

    /* 溢出后状态要复位，能继续处理下一行 */
    dbg_rx_result_t res2 = DBG_RX_MORE;
    const char *next = "@PING\n";
    for (const char *p = next; *p; p++) {
        res2 = dbg_rx_push(&rx, *p);
    }
    CHECK(res2 == DBG_RX_LINE);
    CHECK_STR(rx.buf, "@PING");
}

static void test_kv(void) {
    printf("[test] key=value\n");
    char key[DBG_KEY_MAX];
    char val[DBG_VAL_MAX];

    CHECK(dbg_kv_split("val = 25.30", key, sizeof(key), val, sizeof(val)));
    CHECK_STR(key, "val");
    CHECK_STR(val, "25.30");

    CHECK(!dbg_kv_split("novalue", key, sizeof(key), val, sizeof(val)));
    CHECK(!dbg_kv_split("=x", key, sizeof(key), val, sizeof(val)));

    static dbg_rx_t rx;
    dbg_fields_t f;
    CHECK(feed_line(&rx, "@SET,id=ph,val=7.02,ok=1") == DBG_RX_LINE);
    dbg_fields_split(rx.buf, &f);
    CHECK(dbg_kv_find(&f, "ID") != NULL);
    CHECK_STR(dbg_kv_find(&f, "id"), "ph");
    CHECK_STR(dbg_kv_find(&f, "val"), "7.02");
    CHECK(dbg_kv_find(&f, "missing") == NULL);
}

static void test_parse(void) {
    printf("[test] numeric parsing\n");
    float fv = 0.0f;
    int32_t iv = 0;
    uint32_t uv = 0;
    bool bv = false;

    CHECK(dbg_parse_float("26.5", &fv) && fv > 26.4f && fv < 26.6f);
    CHECK(dbg_parse_float("-5.887", &fv) && fv < -5.8f && fv > -5.9f);
    CHECK(!dbg_parse_float("abc", &fv));
    CHECK(!dbg_parse_float("1.0.0", &fv));
    CHECK(!dbg_parse_float("", &fv));

    CHECK(dbg_parse_i32("-120", &iv) && iv == -120);
    CHECK(!dbg_parse_i32("1x", &iv));

    CHECK(dbg_parse_u32("4294967295", &uv) && uv == 4294967295u);
    CHECK(!dbg_parse_u32("-1", &uv));

    CHECK(dbg_parse_bool("on", &bv) && bv);
    CHECK(dbg_parse_bool("OFF", &bv) && !bv);
    CHECK(dbg_parse_bool("1", &bv) && bv);
    CHECK(dbg_parse_bool("0", &bv) && !bv);
    CHECK(!dbg_parse_bool("maybe", &bv));
}

static void test_output(void) {
    printf("[test] output builder\n");
    char buf[256];
    dbg_out_t o;

    dbg_out_init(&o, buf, sizeof(buf), "PONG");
    dbg_out_kv_u(&o, "uptime_ms", 12345);
    CHECK_STR(dbg_out_str(&o), "#PONG,uptime_ms=12345");
    CHECK(!o.truncated);

    dbg_out_init(&o, buf, sizeof(buf), "SENSOR");
    dbg_out_kv_str(&o, "id", "ph");
    dbg_out_kv_f(&o, "val", 7.02f, 2);
    dbg_out_kv_bool(&o, "ok", true);
    CHECK_STR(dbg_out_str(&o), "#SENSOR,id=ph,val=7.02,ok=1");

    /* 无 verb（续行） */
    dbg_out_init(&o, buf, sizeof(buf), NULL);
    dbg_out_kv_i(&o, "n", -7);
    CHECK_STR(dbg_out_str(&o), "#n=-7");

    /* 截断保护 */
    char small[20];
    dbg_out_init(&o, small, sizeof(small), "ABCDEFG");
    dbg_out_kv_str(&o, "key", "0123456789abcdef");
    CHECK(o.truncated);
    CHECK(strlen(small) < sizeof(small));

    dbg_out_init(&o, buf, sizeof(buf), "E");
    dbg_out_fmt(&o, "count=%d", 3);
    CHECK_STR(dbg_out_str(&o), "#E,count=3");
}

int main(void) {
    printf("== debug_protocol unit tests ==\n");
    test_frame_parsing();
    test_overflow();
    test_kv();
    test_parse();
    test_output();

    printf("== %d checks, %d failures ==\n", g_checks, g_failures);
    return (g_failures == 0) ? 0 : 1;
}
