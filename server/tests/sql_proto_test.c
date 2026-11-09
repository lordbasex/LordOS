/* sql_proto_test.c — the MySQL protocol code of lord-sql (src/sql/mysqlproto.c) against hostile and broken
 * servers: length-encoded integers and strings past the packet, packets out of sequence, oversized and split
 * packets, greetings cut at every byte, column-count bombs, rows with too few or too many values, ERR in the
 * middle of rows, truncation by rows and by bytes, LOAD DATA LOCAL requests, and a mutation fuzzer over a whole
 * result set. Every server byte comes from memory: no network, no lord-sql.
 *
 *   make -C server build/sql-proto-test        (also in `make test`; build/sql-proto-test-asan: under ASan) */
#include "../src/sql/mysqlproto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- a server in memory ---- */

typedef struct { uint8_t *p; size_t n, cap; } Buf;

static void put(Buf *b, const void *d, size_t n) {
    if (b->n + n > b->cap) { b->cap = (b->n + n) * 2 + 64; b->p = realloc(b->p, b->cap); }
    memcpy(b->p + b->n, d, n);
    b->n += n;
}
static void put8(Buf *b, unsigned v) { uint8_t x = (uint8_t)v; put(b, &x, 1); }
static void put16(Buf *b, unsigned v) { put8(b, v & 255); put8(b, v >> 8); }
static void put32(Buf *b, uint32_t v) { put16(b, v & 0xffff); put16(b, v >> 16); }
static void lenenc(Buf *b, uint64_t v) {
    if (v < 251) put8(b, (unsigned)v);
    else if (v < 65536) { put8(b, 0xfc); put16(b, (unsigned)v); }
    else if (v < (1u << 24)) { put8(b, 0xfd); put8(b, v & 255); put16(b, (unsigned)(v >> 8)); }
    else { put8(b, 0xfe); put32(b, (uint32_t)v); put32(b, (uint32_t)(v >> 32)); }
}
static void lstr(Buf *b, const char *s) { lenenc(b, strlen(s)); put(b, s, strlen(s)); }

/* wraps payload into a packet with sequence seq */
static void packet(Buf *out, uint8_t seq, const Buf *pl) {
    put8(out, pl->n & 255);
    put8(out, (pl->n >> 8) & 255);
    put8(out, (pl->n >> 16) & 255);
    put8(out, seq);
    put(out, pl->p, pl->n);
}

typedef struct { const uint8_t *in; size_t n, pos; Buf out; } Mem;
static int mem_rd(void *io, void *buf, size_t n) {
    Mem *m = io;
    if (m->n - m->pos < n) return -1;
    memcpy(buf, m->in + m->pos, n);
    m->pos += n;
    return 0;
}
static int mem_wr(void *io, const void *buf, size_t n) { put(&((Mem *)io)->out, buf, n); return 0; }

static void conn_on(MyConn *c, Mem *m, const Buf *in, uint32_t caps) {
    memset(m, 0, sizeof *m);
    m->in = in->p;
    m->n = in->n;
    my_init(c);
    c->rd = mem_rd;
    c->wr = mem_wr;
    c->io = m;
    c->caps = caps;
}

static void column(Buf *pl, const char *name, uint8_t type, uint16_t charset, uint16_t flags) {
    pl->n = 0;
    lstr(pl, "def"); lstr(pl, "shop"); lstr(pl, "t"); lstr(pl, "orders"); lstr(pl, name); lstr(pl, name);
    lenenc(pl, 12);
    put16(pl, charset); put32(pl, 255); put8(pl, type); put16(pl, flags); put8(pl, 0); put16(pl, 0);
}

static void ok_packet(Buf *pl, uint8_t head, uint64_t affected, uint64_t insert_id, uint16_t status, uint16_t warnings, const char *info) {
    pl->n = 0;
    put8(pl, head); lenenc(pl, affected); lenenc(pl, insert_id); put16(pl, status); put16(pl, warnings);
    if (info) put(pl, info, strlen(info));
}

/* SELECT id, name, bin with rows, then an OK; DEPRECATE_EOF style unless eof */
static void two_results(Buf *in, int eof, int rows) {
    Buf pl = { 0 };
    uint8_t seq = 1;
    pl.n = 0; lenenc(&pl, 3); packet(in, seq++, &pl);
    column(&pl, "id", 3, 63, MY_NOT_NULL | MY_PRI_KEY | MY_AUTO_INCREMENT); packet(in, seq++, &pl);
    column(&pl, "name", 253, 45, 0); packet(in, seq++, &pl);
    column(&pl, "bin", 252, 63, MY_BLOB | MY_BINARY); packet(in, seq++, &pl);
    if (eof) { pl.n = 0; put8(&pl, 0xfe); put16(&pl, 0); put16(&pl, 0); packet(in, seq++, &pl); }
    for (int i = 0; i < rows; i++) {
        char id[16];
        snprintf(id, sizeof id, "%d", i + 1);
        pl.n = 0; lstr(&pl, id);
        if (i == 1) put8(&pl, 0xfb); else lstr(&pl, "a\"b\n");
        lenenc(&pl, 2); put8(&pl, 0); put8(&pl, 0xff);
        packet(in, seq++, &pl);
    }
    if (eof) { pl.n = 0; put8(&pl, 0xfe); put16(&pl, 1); put16(&pl, MY_SERVER_MORE_RESULTS); }
    else ok_packet(&pl, 0xfe, 0, 0, MY_SERVER_MORE_RESULTS, 1, NULL);
    packet(in, seq++, &pl);
    ok_packet(&pl, 0, 3, 7, 0, 0, "Records: 3");
    packet(in, seq++, &pl);
    free(pl.p);
}

static int run_query(const Buf *in, uint32_t caps, long max_rows, size_t max_bytes, MyBuf *out, MyConn *cc) {
    Mem m;
    MyConn c;
    conn_on(&c, &m, in, caps);
    mb_init(out, (size_t)80 << 20);
    int rc = my_query(&c, "SELECT 1", 8, max_rows, max_bytes, out);
    if (cc) *cc = c;
    else my_free(&c);
    free(m.out.p);
    return rc;
}

static const char *EXPECT_COLS =
    "{\"columns\":[{\"name\":\"id\",\"orgName\":\"id\",\"table\":\"t\",\"orgTable\":\"orders\",\"db\":\"shop\",\"type\":3,\"typeName\":\"INT\","
    "\"length\":255,\"decimals\":0,\"charset\":63,\"flags\":{\"notNull\":true,\"pk\":true,\"unique\":false,\"multiKey\":false,"
    "\"unsigned\":false,\"zerofill\":false,\"binary\":false,\"autoInc\":true,\"blob\":false}},";

static void test_lenenc(void) {
    uint8_t a[] = { 0xfa };
    MyRd r = { a, 1, 0 };
    uint64_t v;
    int null;
    CHECK(!my_rd_lenenc(&r, &v, NULL) && v == 250, "1-byte lenenc");
    uint8_t b[] = { 0xfb };
    r = (MyRd){ b, 1, 0 };
    CHECK(my_rd_lenenc(&r, &v, NULL), "NULL marker refused outside rows");
    r = (MyRd){ b, 1, 0 };
    CHECK(!my_rd_lenenc(&r, &v, &null) && null, "NULL marker in a row");
    uint8_t c[] = { 0xfc, 0x01 };
    r = (MyRd){ c, 2, 0 };
    CHECK(my_rd_lenenc(&r, &v, NULL), "cut 2-byte lenenc");
    uint8_t d[] = { 0xfd, 1, 2, 3 };
    r = (MyRd){ d, 4, 0 };
    CHECK(!my_rd_lenenc(&r, &v, NULL) && v == 0x030201, "3-byte lenenc");
    uint8_t e[] = { 0xfe, 1, 0, 0, 0, 0, 0, 0 };
    r = (MyRd){ e, 8, 0 };
    CHECK(my_rd_lenenc(&r, &v, NULL), "cut 8-byte lenenc");
    uint8_t f[] = { 0xff };
    r = (MyRd){ f, 1, 0 };
    CHECK(my_rd_lenenc(&r, &v, NULL), "0xff is not an integer");
    /* strings whose length points past the packet, also by 2^64 - 1 */
    uint8_t g[] = { 0x05, 'a', 'b' };
    MySlice s;
    r = (MyRd){ g, 3, 0 };
    CHECK(my_rd_lenenc_str(&r, &s, NULL), "string past the packet");
    uint8_t h[] = { 0xfe, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 'x' };
    r = (MyRd){ h, sizeof h, 0 };
    CHECK(my_rd_lenenc_str(&r, &s, NULL), "2^64-1 length");
    uint8_t i2[] = { 0xfe, 0x01, 0, 0, 0, 0x01, 0, 0, 0, 'x' };
    r = (MyRd){ i2, sizeof i2, 0 };
    CHECK(my_rd_lenenc_str(&r, &s, NULL), "2^32+1 length (no wrap to 1)");
    uint8_t j[] = { 'a', 'b' };
    r = (MyRd){ j, 2, 0 };
    CHECK(my_rd_nulstr(&r, &s), "string without its NUL");
    uint8_t k[] = { 1, 2, 3 };
    r = (MyRd){ k, 3, 0 };
    uint32_t u;
    CHECK(my_rd_u32(&r, &u) && r.pos == 0, "u32 past the end, cursor kept");
}

static void test_packets(void) {
    Buf in = { 0 }, pl = { 0 };
    MyConn c;
    Mem m;
    /* sequence */
    put(&pl, "\x00\x00\x00\x02\x00\x00\x00", 7);
    packet(&in, 5, &pl);
    conn_on(&c, &m, &in, 0);
    c.seq = 0;
    CHECK(my_read_packet(&c) == MY_E_SEQ, "sequence error");
    my_free(&c);
    /* header cut, payload cut */
    in.n = 0;
    put(&in, "\x05\x00", 2);
    conn_on(&c, &m, &in, 0);
    CHECK(my_read_packet(&c) == MY_E_IO, "cut header");
    my_free(&c);
    in.n = 0;
    put(&in, "\x05\x00\x00\x00" "ab", 6);
    conn_on(&c, &m, &in, 0);
    CHECK(my_read_packet(&c) == MY_E_IO, "cut payload");
    my_free(&c);
    /* a packet larger than the limit is read to its end and dropped; the next one is fine */
    in.n = 0;
    pl.n = 0;
    for (int i = 0; i < 300; i++) put8(&pl, 'x');
    packet(&in, 0, &pl);
    pl.n = 0;
    put(&pl, "ok", 2);
    packet(&in, 1, &pl);
    conn_on(&c, &m, &in, 0);
    c.max = 100;
    CHECK(my_read_packet(&c) == MY_E_BIG, "oversized packet");
    CHECK(my_read_packet(&c) == MY_OK && c.len == 2 && !memcmp(c.pkt, "ok", 2), "in step after the oversized one");
    my_free(&c);
    /* a split packet: 0xffffff + 5 bytes, reassembled; and one of exactly 0xffffff with its empty end */
    in.n = 0;
    size_t big = 0xffffff;
    uint8_t *blob = calloc(1, big);
    blob[big - 1] = 'z';
    put(&in, "\xff\xff\xff\x00", 4);
    put(&in, blob, big);
    put(&in, "\x05\x00\x00\x01" "hello", 9);
    put(&in, "\xff\xff\xff\x02", 4);
    put(&in, blob, big);
    put(&in, "\x00\x00\x00\x03", 4);
    conn_on(&c, &m, &in, 0);
    CHECK(my_read_packet(&c) == MY_OK && c.len == big + 5 && c.pkt[big - 1] == 'z' && !memcmp(c.pkt + big, "hello", 5), "split packet");
    CHECK(my_read_packet(&c) == MY_OK && c.len == big, "exactly 0xffffff");
    my_free(&c);
    /* writing: the same split */
    conn_on(&c, &m, &in, 0);
    CHECK(my_write_packet(&c, blob, big) == MY_OK && m.out.n == 4 + big + 4 && m.out.p[3] == 0 && m.out.p[4 + big + 3] == 1 &&
          m.out.p[4 + big] == 0, "writing 0xffffff adds an empty packet");
    free(m.out.p);
    my_free(&c);
    free(blob);
    free(in.p);
    free(pl.p);
}

static void handshake(Buf *pl, const char *version, uint32_t caps, const char *plugin) {
    pl->n = 0;
    put8(pl, 10);
    put(pl, version, strlen(version) + 1);
    put32(pl, 42);
    put(pl, "abcdefgh", 8);
    put8(pl, 0);
    put16(pl, caps & 0xffff);
    put8(pl, 45);
    put16(pl, 2);
    put16(pl, caps >> 16);
    put8(pl, 21);
    put(pl, "\0\0\0\0\0\0\0\0\0\0", 10);
    put(pl, "ijklmnopqrst", 12);
    put8(pl, 0);
    if (plugin) put(pl, plugin, strlen(plugin) + 1);
}

#define CAPS (MY_CLIENT_PROTOCOL_41 | MY_CLIENT_SECURE_CONNECTION | MY_CLIENT_PLUGIN_AUTH | MY_CLIENT_PLUGIN_AUTH_LENENC | \
              MY_CLIENT_DEPRECATE_EOF | MY_CLIENT_MULTI_RESULTS | MY_CLIENT_MULTI_STATEMENTS | MY_CLIENT_CONNECT_WITH_DB | \
              MY_CLIENT_LONG_PASSWORD | MY_CLIENT_TRANSACTIONS)

static void test_handshake(void) {
    Buf pl = { 0 };
    MyHandshake h;
    handshake(&pl, "8.0.36", CAPS, "caching_sha2_password");
    CHECK(my_parse_handshake(pl.p, pl.n, &h) == MY_OK && !strcmp(h.version, "8.0.36") && h.thread_id == 42 && !h.mariadb &&
          !memcmp(h.scramble, "abcdefghijklmnopqrst", 20) && !strcmp(h.plugin, "caching_sha2_password") && h.caps == CAPS,
          "MySQL greeting");
    handshake(&pl, "5.5.5-10.11.6-MariaDB-0+deb12u1", CAPS, "mysql_native_password");
    CHECK(my_parse_handshake(pl.p, pl.n, &h) == MY_OK && h.mariadb && !strcmp(h.version, "10.11.6-MariaDB-0+deb12u1"), "MariaDB greeting");
    /* every cut of the greeting: no crash, no read past the end (ASan build), no garbage accepted silently */
    handshake(&pl, "8.0.36", CAPS, "caching_sha2_password");
    int okcuts = 0;
    for (size_t n = 0; n < pl.n; n++) {
        uint8_t *cut = malloc(n ? n : 1);
        memcpy(cut, pl.p, n);
        if (my_parse_handshake(cut, n, &h) == MY_OK) okcuts++;
        free(cut);
    }
    /* only the cuts after the 4.0-style fields (no capabilities above 16 bits) or inside the plugin name parse */
    CHECK(okcuts <= 25, "greeting cuts that parse: %d", okcuts);
    pl.p[0] = 9;
    CHECK(my_parse_handshake(pl.p, pl.n, &h) == MY_E_UNSUPPORTED, "protocol 9");
    /* the version without its NUL */
    pl.n = 0;
    put8(&pl, 10);
    put(&pl, "8.0.36", 6);
    CHECK(my_parse_handshake(pl.p, pl.n, &h) == MY_E_PROTO, "version without NUL");
    /* a long version and control characters are cut and replaced */
    char lv[300];
    memset(lv, 'v', sizeof lv - 1);
    lv[sizeof lv - 1] = 0;
    lv[3] = '\n';
    handshake(&pl, lv, CAPS, "x");
    CHECK(my_parse_handshake(pl.p, pl.n, &h) == MY_OK && strlen(h.version) == 63 && h.version[3] == '?', "long version");
    /* the ERR greeting */
    pl.n = 0;
    put8(&pl, 0xff); put16(&pl, 1130); put(&pl, "Host 'x' is not allowed", 23);
    MyErr e;
    CHECK(my_parse_handshake(pl.p, pl.n, &h) == MY_E_SQL && !my_parse_err(pl.p, pl.n, &e) && e.code == 1130 && !e.state[0] &&
          !strcmp(e.msg, "Host 'x' is not allowed"), "ERR greeting");
    pl.n = 0;
    put8(&pl, 0xff); put16(&pl, 1064); put(&pl, "#42000syntax", 12);
    CHECK(!my_parse_err(pl.p, pl.n, &e) && !strcmp(e.state, "42000") && !strcmp(e.msg, "syntax"), "ERR with SQL state");
    pl.n = 0;
    put8(&pl, 0xff); put8(&pl, 1);
    CHECK(my_parse_err(pl.p, pl.n, &e), "cut ERR");
    pl.n = 0;
    put8(&pl, 0xff); put16(&pl, 1); for (int i = 0; i < 2000; i++) put8(&pl, 'm');
    CHECK(!my_parse_err(pl.p, pl.n, &e) && strlen(e.msg) == sizeof e.msg - 1, "long message cut");
    free(pl.p);
}

static void test_column(void) {
    Buf pl = { 0 };
    MyColumn col;
    column(&pl, "name", 253, 45, MY_NOT_NULL);
    CHECK(my_parse_column(pl.p, pl.n, &col) == MY_OK && col.name.n == 4 && col.type == 253 && col.charset == 45, "column");
    int okcuts = 0;
    for (size_t n = 0; n < pl.n; n++) if (my_parse_column(pl.p, n, &col) == MY_OK) okcuts++;
    CHECK(okcuts <= 2, "column cuts that parse: %d", okcuts);       /* only the filler may be missing */
    /* fixed-length field shorter than 12 */
    pl.n = 0;
    lstr(&pl, "def"); lstr(&pl, ""); lstr(&pl, ""); lstr(&pl, ""); lstr(&pl, "a"); lstr(&pl, "a"); lenenc(&pl, 4);
    put32(&pl, 0); put32(&pl, 0); put32(&pl, 0);
    CHECK(my_parse_column(pl.p, pl.n, &col) == MY_E_PROTO, "short fixed fields");
    CHECK(!strcmp(my_type_name(253, 0, 63), "VARBINARY") && !strcmp(my_type_name(252, 0, 45), "TEXT") &&
          !strcmp(my_type_name(254, MY_ENUM, 45), "ENUM") && !strcmp(my_type_name(3, 0, 63), "INT") &&
          my_is_binary(252, 63) && !my_is_binary(3, 63) && !my_is_binary(253, 45), "type names and binary columns");
    free(pl.p);
}

static void test_query(void) {
    Buf in = { 0 }, pl = { 0 };
    MyBuf out;
    MyConn c;
    two_results(&in, 0, 3);
    int rc = run_query(&in, CAPS, 1000, 1 << 20, &out, &c);
    CHECK(rc == MY_OK, "two results: rc %d (%s)", rc, c.why);
    my_free(&c);
    CHECK(out.p && !strncmp(out.p, "[", 1) && strstr(out.p, EXPECT_COLS) &&
          strstr(out.p, "\"rows\":[[\"1\",\"a\\\"b\\n\",\"\\u0000hex:00ff\"],[\"2\",null,\"\\u0000hex:00ff\"],[\"3\",\"a\\\"b\\n\",\"\\u0000hex:00ff\"]]") &&
          strstr(out.p, "\"affected\":0,\"insertId\":0,\"warnings\":1,\"info\":\"\",\"truncated\":false},{\"columns\":[],\"rows\":[],"
                        "\"affected\":3,\"insertId\":7,\"warnings\":0,\"info\":\"Records: 3\",\"truncated\":false}]"),
          "two results JSON: %s", out.p ? out.p : "(null)");
    mb_free(&out);
    /* the same with EOF packets (servers without CLIENT_DEPRECATE_EOF) */
    in.n = 0;
    two_results(&in, 1, 3);
    rc = run_query(&in, CAPS & ~MY_CLIENT_DEPRECATE_EOF, 1000, 1 << 20, &out, NULL);
    CHECK(rc == MY_OK && strstr(out.p, "\"warnings\":1,\"info\":\"\",\"truncated\":false},{\"columns\":[]"), "EOF style");
    mb_free(&out);
    /* EOF style expected, none sent */
    in.n = 0;
    two_results(&in, 0, 3);
    CHECK(run_query(&in, CAPS & ~MY_CLIENT_DEPRECATE_EOF, 1000, 1 << 20, &out, NULL) == MY_E_PROTO, "missing EOF");
    mb_free(&out);
    /* truncation by rows: the rest is read and dropped, the next result still comes */
    in.n = 0;
    two_results(&in, 0, 5);
    rc = run_query(&in, CAPS, 2, 1 << 20, &out, NULL);
    CHECK(rc == MY_OK && strstr(out.p, "[\"2\",null,\"\\u0000hex:00ff\"]],\"affected\":0,\"insertId\":0,\"warnings\":1,\"info\":\"\",\"truncated\":true}") &&
          strstr(out.p, "\"info\":\"Records: 3\""), "maxRows: %s", out.p);
    mb_free(&out);
    /* truncation by bytes */
    in.n = 0;
    two_results(&in, 0, 50);
    rc = run_query(&in, CAPS, 1000, 1200, &out, NULL);
    CHECK(rc == MY_OK && strstr(out.p, "\"truncated\":true") && out.n < 1600, "byte limit: %zu", out.n);
    mb_free(&out);
    /* ERR in the middle of the rows: what came is kept, the array is complete */
    in.n = 0;
    {
        uint8_t seq = 1;
        pl.n = 0; lenenc(&pl, 1); packet(&in, seq++, &pl);
        column(&pl, "a", 3, 63, 0); packet(&in, seq++, &pl);
        pl.n = 0; lstr(&pl, "1"); packet(&in, seq++, &pl);
        pl.n = 0; put8(&pl, 0xff); put16(&pl, 1317); put(&pl, "#70100Query execution was interrupted", 38); packet(&in, seq++, &pl);
    }
    rc = run_query(&in, CAPS, 1000, 1 << 20, &out, &c);
    CHECK(rc == MY_E_SQL && c.err.code == 1317 && !strcmp(c.err.state, "70100") && out.p[out.n - 1] == ']' &&
          strstr(out.p, "\"rows\":[[\"1\"]],\"affected\":0"), "ERR mid rows: %s", out.p);
    my_free(&c);
    mb_free(&out);
    /* hostile column counts */
    uint64_t counts[] = { 0, MY_MAX_COLUMNS + 1, 100000, 0xffffffffu, (uint64_t)-1 };
    for (size_t i = 0; i < sizeof counts / sizeof counts[0]; i++) {
        in.n = 0; pl.n = 0; lenenc(&pl, counts[i]); packet(&in, 1, &pl);
        CHECK(run_query(&in, CAPS, 1000, 1 << 20, &out, NULL) == MY_E_PROTO, "column count %llu", (unsigned long long)counts[i]);
        mb_free(&out);
    }
    /* column count with bytes after it */
    in.n = 0; pl.n = 0; lenenc(&pl, 1); put8(&pl, 7); packet(&in, 1, &pl);
    CHECK(run_query(&in, CAPS, 1000, 1 << 20, &out, NULL) == MY_E_PROTO, "column count with trailing bytes");
    mb_free(&out);
    /* a promised column that never comes */
    in.n = 0; pl.n = 0; lenenc(&pl, 4000); packet(&in, 1, &pl);
    CHECK(run_query(&in, CAPS, 1000, 1 << 20, &out, NULL) == MY_E_IO, "missing columns");
    mb_free(&out);
    /* rows with the wrong number of values, or values past the packet */
    const char *rows[][2] = { { "\x01" "1", "short row" }, { "\x01" "1\x01" "2\x01" "3", "long row" }, { "\x01" "1\x09" "ab", "value past the packet" },
                              { "\x01" "1\xfe\xff\xff\xff\xff\xff\xff\xff\x7f", "huge lenenc" }, { "\x01" "1\xff", "0xff value" } };
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        in.n = 0;
        uint8_t seq = 1;
        pl.n = 0; lenenc(&pl, 2); packet(&in, seq++, &pl);
        column(&pl, "a", 3, 63, 0); packet(&in, seq++, &pl);
        column(&pl, "b", 3, 63, 0); packet(&in, seq++, &pl);
        pl.n = 0;
        size_t rl = strlen(rows[i][0]);
        if (i == 3) rl = 11;
        put(&pl, rows[i][0], rl);
        packet(&in, seq++, &pl);
        ok_packet(&pl, 0xfe, 0, 0, 0, 0, NULL); packet(&in, seq++, &pl);
        CHECK(run_query(&in, CAPS, 1000, 1 << 20, &out, NULL) == MY_E_PROTO, "%s", rows[i][1]);
        mb_free(&out);
    }
    /* sequence error between rows */
    in.n = 0;
    {
        uint8_t seq = 1;
        pl.n = 0; lenenc(&pl, 1); packet(&in, seq++, &pl);
        column(&pl, "a", 3, 63, 0); packet(&in, seq++, &pl);
        pl.n = 0; lstr(&pl, "1"); packet(&in, (uint8_t)(seq + 3), &pl);
    }
    CHECK(run_query(&in, CAPS, 1000, 1 << 20, &out, NULL) == MY_E_SEQ, "sequence error in rows");
    mb_free(&out);
    /* LOAD DATA LOCAL: answered with an empty file, then the server's answer */
    in.n = 0;
    pl.n = 0; put8(&pl, 0xfb); put(&pl, "/etc/passwd", 11); packet(&in, 1, &pl);
    ok_packet(&pl, 0, 0, 0, 0, 0, NULL); packet(&in, 3, &pl);
    {
        Mem m;
        conn_on(&c, &m, &in, CAPS);
        mb_init(&out, 1 << 20);
        rc = my_query(&c, "LOAD DATA LOCAL INFILE 'x' INTO TABLE t", 39, 1000, 1 << 20, &out);
        /* what we wrote: the query (seq 0) and an empty packet (seq 2) */
        CHECK(rc == MY_OK && m.out.n == 4 + 40 + 4 && !memcmp(m.out.p + 44, "\x00\x00\x00\x02", 4), "LOCAL INFILE refused: rc %d", rc);
        free(m.out.p);
        my_free(&c);
        mb_free(&out);
    }
    /* invalid UTF-8 and control characters in text values */
    in.n = 0;
    {
        uint8_t seq = 1;
        pl.n = 0; lenenc(&pl, 1); packet(&in, seq++, &pl);
        column(&pl, "a", 253, 45, 0); packet(&in, seq++, &pl);
        pl.n = 0; lenenc(&pl, 6); put(&pl, "\xc3\x28\x01\x00\xe2\x82", 6); packet(&in, seq++, &pl);
        ok_packet(&pl, 0xfe, 0, 0, 0, 0, NULL); packet(&in, seq++, &pl);
    }
    rc = run_query(&in, CAPS, 1000, 1 << 20, &out, NULL);
    CHECK(rc == MY_OK && strstr(out.p, "[[\"\xef\xbf\xbd(\\u0001\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd\"]]"), "bad UTF-8: %s", out.p);
    mb_free(&out);
    free(in.p);
    free(pl.p);
}

/* mutations of a whole result set: whatever comes back, no crash and no read past a buffer */
static void test_fuzz(void) {
    Buf in = { 0 };
    two_results(&in, 0, 4);
    uint8_t *orig = malloc(in.n);
    memcpy(orig, in.p, in.n);
    unsigned seed = 12345;
    int okn = 0;
    for (int it = 0; it < 30000; it++) {
        memcpy(in.p, orig, in.n);
        int muts = 1 + it % 4;
        for (int k = 0; k < muts; k++) {
            seed = seed * 1103515245u + 12345u;
            size_t at = (seed >> 8) % in.n;
            seed = seed * 1103515245u + 12345u;
            in.p[at] = (uint8_t)(seed >> 16);
        }
        Buf cut = in;
        if (it % 7 == 0) { seed = seed * 1103515245u + 12345u; cut.n = (seed >> 8) % in.n; }
        MyBuf out;
        if (run_query(&cut, it % 2 ? CAPS : CAPS & ~MY_CLIENT_DEPRECATE_EOF, 1000, 1 << 20, &out, NULL) == MY_OK) okn++;
        mb_free(&out);
    }
    CHECK(1, "fuzz");
    printf("  fuzz: 30000 mutated result sets, %d still parsed\n", okn);
    free(orig);
    free(in.p);
}

static void test_connect(void) {
    Buf in = { 0 }, pl = { 0 };
    MyConn c;
    Mem m;
    MyHandshake h;
    MyLogin lg = { "shop", "secret", "shop", 0 };
    /* native: the response carries the user, the scramble and the database */
    handshake(&pl, "8.0.36", CAPS, "mysql_native_password");
    packet(&in, 0, &pl);
    ok_packet(&pl, 0, 0, 0, 2, 0, NULL); packet(&in, 2, &pl);
    conn_on(&c, &m, &in, 0);
    int rc = my_connect(&c, &lg, &h);
    uint8_t want[20];
    my_scramble_native((const uint8_t *)"abcdefghijklmnopqrst", 20, "secret", want);
    CHECK(rc == MY_OK && m.out.n > 36 + 5 && !memcmp(m.out.p + 4 + 32, "shop\0", 5) && m.out.p[4 + 37] == 20 &&
          !memcmp(m.out.p + 4 + 38, want, 20) && !memcmp(m.out.p + 4 + 58, "shop\0mysql_native_password\0", 27), "native login: rc %d", rc);
    CHECK(!(c.caps & MY_CLIENT_LOCAL_FILES) && !(c.caps & MY_CLIENT_SSL), "no LOCAL FILES, no SSL");
    free(m.out.p);
    my_free(&c);
    /* TLS required, server offers none */
    lg.tls = 2;
    conn_on(&c, &m, &in, 0);
    CHECK(my_connect(&c, &lg, &h) == MY_E_TLS, "TLS required, not offered");
    free(m.out.p);
    my_free(&c);
    lg.tls = 0;
    /* switch to an unknown plugin; two switches; 0x01 to the native plugin; bad password */
    struct { const char *pkt; size_t n; int want; const char *what; } cases[] = {
        { "\xfe" "sha256_password\0abcdefghijklmnopqrst\0", 37, MY_E_UNSUPPORTED, "unknown plugin" },
        { "\xfe", 1, MY_E_PROTO, "old password switch" },
        { "\xfe" "mysql_native_password\0abc\0", 26, MY_E_PROTO, "short scramble" },
        { "\x01\x03", 2, MY_E_PROTO, "fast auth packet to native" },
        { "\xff\x15\x04#28000Access denied", 22, MY_E_AUTH, "access denied" },
        { "\x07", 1, MY_E_PROTO, "unknown packet" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        in.n = 0;
        handshake(&pl, "8.0.36", CAPS, "mysql_native_password");
        packet(&in, 0, &pl);
        pl.n = 0; put(&pl, cases[i].pkt, cases[i].n); packet(&in, 2, &pl);
        conn_on(&c, &m, &in, 0);
        rc = my_connect(&c, &lg, &h);
        CHECK(rc == cases[i].want, "%s: rc %d", cases[i].what, rc);
        if (cases[i].want == MY_E_AUTH) CHECK(c.err.code == 1045 && !strcmp(c.err.state, "28000"), "denied details");
        free(m.out.p);
        my_free(&c);
    }
    /* two switches */
    in.n = 0;
    handshake(&pl, "8.0.36", CAPS, "mysql_native_password");
    packet(&in, 0, &pl);
    pl.n = 0; put(&pl, "\xfe" "mysql_native_password\0abcdefghijklmnopqrst\0", 43); packet(&in, 2, &pl);
    pl.n = 0; put(&pl, "\xfe" "mysql_native_password\0abcdefghijklmnopqrst\0", 43); packet(&in, 4, &pl);
    conn_on(&c, &m, &in, 0);
    CHECK(my_connect(&c, &lg, &h) == MY_E_PROTO, "second auth switch");
    free(m.out.p);
    my_free(&c);
    /* a 4.1-less server */
    in.n = 0;
    handshake(&pl, "4.0", MY_CLIENT_LONG_PASSWORD, NULL);
    packet(&in, 0, &pl);
    conn_on(&c, &m, &in, 0);
    CHECK(my_connect(&c, &lg, &h) == MY_E_UNSUPPORTED, "old server");
    free(m.out.p);
    my_free(&c);
    free(in.p);
    free(pl.p);
}

int main(void) {
    test_lenenc();
    test_packets();
    test_handshake();
    test_column();
    test_query();
    test_fuzz();
    test_connect();
    printf("sql-proto-test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
