/* adminline_test.c — the privileged helper's reply lines (src/adminline.h), GitHub #280.
 *
 * install-disks describes each disk in one LOG line of JSON. With seven partitions that line was longer than the
 * helper's 1200-byte lines: it was cut, JSON.parse failed in the installer and a healthy disk vanished ("No disk to
 * install on was found"). Here: a long line goes whole through the builder and a reader fed in small pieces; a text
 * longer than ADMIN_LINE_MAX is cut but stays one line; a reader drops a line too long for its buffer whole and
 * the next line arrives intact, never the long line's tail as a line of its own. */
#include "../src/adminline.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

typedef struct { int n; char *line[16]; size_t len[16]; } Got;

static void collect(char *line, size_t n, void *arg) {
    Got *g = arg;
    if (g->n >= 16) return;
    CHECK(strlen(line) == n);
    g->line[g->n] = strdup(line);
    g->len[g->n++] = n;
}

static void got_free(Got *g) { for (int i = 0; i < g->n; i++) free(g->line[i]); g->n = 0; }

/* feeds data to a reader with a buffer of cap bytes, piece bytes at a time, as read() would */
static void feed(Got *g, size_t cap, const char *data, size_t len, size_t piece) {
    char *buf = malloc(cap);
    AdminLineRd rd = { buf, cap, 0, 0 };
    size_t off = 0;
    while (off < len) {
        size_t room = cap - 1 - rd.len, k = len - off < piece ? len - off : piece;
        if (k > room) k = room;
        memcpy(buf + rd.len, data + off, k);
        rd.len += k;
        off += k;
        adminline_scan(&rd, collect, g);
    }
    free(buf);
}

/* the JSON install-disks writes for a disk like #280's: seven partitions, each with its table entry */
static size_t disk_json(char *out, size_t cap, int parts) {
    size_t n = (size_t)snprintf(out, cap, "{\"name\":\"sda\",\"path\":\"/dev/sda\",\"size\":256060514304,\"model\":\"SanDisk SD7SN6S-256G-1006\","
                                "\"vendor\":\"ATA\",\"transport\":\"sata\",\"removable\":false,\"usable\":true,\"ptable\":\"gpt\",\"esp\":true,"
                                "\"espPart\":\"/dev/sda1\",\"freeStart\":0,\"freeEnd\":0,\"freeBytes\":0,\"freeOk\":false,"
                                "\"freeWhy\":\"install.noFreeSpace\",\"lsec\":512,\"first\":34,\"last\":500118158,"
                                "\"tableTag\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\",\"manualOk\":true,"
                                "\"manualWhy\":\"\",\"parts\":[");
    for (int i = 1; i <= parts; i++)
        n += (size_t)snprintf(out + n, cap - n, "%s{\"name\":\"sda%d\",\"fstype\":\"ext4\",\"label\":\"a label of some length\","
                              "\"size\":40000000000,\"num\":%d,\"start\":2048,\"end\":78124999,\"esp\":false,\"fs\":\"ext4\"}",
                              i > 1 ? "," : "", i, i);
    n += (size_t)snprintf(out + n, cap - n, "]}");
    return n;
}

int main(void) {
    static char line[ADMIN_LINE_MAX], json[ADMIN_LINE_MAX];
    Got g = {0};

    /* #280: the seven-partition description is over the old 1200 bytes and arrives whole */
    size_t jl = disk_json(json, sizeof json, 7);
    CHECK(jl > 1200);
    size_t n = adminline_build(line, sizeof line, 42, "LOG", "%s", json);
    CHECK(n == jl + strlen("42 LOG ") + 1);
    CHECK(line[n - 1] == '\n');
    feed(&g, ADMIN_LINE_MAX, line, n, 1000);
    CHECK(g.n == 1);
    CHECK(g.n == 1 && !strncmp(g.line[0], "42 LOG {", 8) && !strcmp(g.line[0] + 7, json));
    got_free(&g);

    /* 128 partitions (FSP_MAX_PARTS) of this size still fit a line */
    jl = disk_json(json, sizeof json, 128);
    CHECK(jl + 64 < ADMIN_LINE_MAX);

    /* \n and \r in the text never make a second line */
    n = adminline_build(line, sizeof line, 7, "OK", "a\nb\rc");
    CHECK(n == strlen("7 OK a b c\n") && !memcmp(line, "7 OK a b c\n", n));

    /* a text longer than the line is cut and stays one line */
    char *big = malloc(ADMIN_LINE_MAX * 2);
    memset(big, 'x', ADMIN_LINE_MAX * 2 - 1);
    big[ADMIN_LINE_MAX * 2 - 1] = 0;
    n = adminline_build(line, sizeof line, 3, "LOG", "%s", big);
    CHECK(n == ADMIN_LINE_MAX - 1);
    CHECK(line[n - 1] == '\n' && memchr(line, '\n', n - 1) == NULL);
    feed(&g, ADMIN_LINE_MAX, line, n, 4096);
    CHECK(g.n == 1 && g.len[0] == n - 1);
    got_free(&g);

    /* a reader with a small buffer drops a line too long for it whole; the lines around it arrive intact */
    char stream[4096];
    size_t sl = (size_t)snprintf(stream, sizeof stream, "1 LOG first\n");
    sl += (size_t)snprintf(stream + sl, sizeof stream - sl, "2 LOG ");
    memset(stream + sl, '9', 1500);                     /* digits: a tail read as a line would get a bogus id */
    sl += 1500;
    sl += (size_t)snprintf(stream + sl, sizeof stream - sl, "\n3 OK done\n");
    for (size_t piece = 1; piece <= 700; piece += 233) {
        feed(&g, 256, stream, sl, piece);
        CHECK(g.n == 2);
        CHECK(g.n == 2 && !strcmp(g.line[0], "1 LOG first") && !strcmp(g.line[1], "3 OK done"));
        got_free(&g);
    }

    /* a line exactly as long as the buffer allows (cap - 2 bytes and its newline) is kept */
    memset(stream, 'a', 254);
    stream[254] = '\n';
    feed(&g, 256, stream, 255, 255);
    CHECK(g.n == 1 && g.len[0] == 254);
    got_free(&g);

    /* the header must fit, or nothing is built */
    CHECK(adminline_build(line, 6, 123456, "LOG", "x") == 0);
    free(big);
    if (fails) { fprintf(stderr, "adminline-test: %d failures\n", fails); return 1; }
    printf("adminline-test: ok\n");
    return 0;
}
