/* greeterscale_test.c — the login screen's scale (#242): lord-sessiond's parser of the session's request, the file
 * and the environment (src/remote/greeterscale.c), with a fuzz pass of random and mutated input. */
#include "../src/remote/greeterscale.h"
#include "../src/outputs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int pass, failc;
#define CHECK(x) do { if (x) pass++; else { failc++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)

static int msg(const char *j, const char *want_conn, int want_pct) {
    char c[GSCALE_CONN_MAX + 1] = "untouched";
    int p = -7;
    int r = gscale_from_msg(j, c, sizeof c, &p);
    if (!want_conn) return r == -1 && !strcmp(c, "untouched") && p == -7;
    return r == 1 && !strcmp(c, want_conn) && p == want_pct;
}

static void t_msg(void) {
    CHECK(msg("{\"a\":\"display\",\"scaleOn\":\"eDP-1\",\"scale\":125}", "eDP-1", 125));
    CHECK(msg("{\"a\":\"display\",\"scaleOn\":\"HDMI-A-1\",\"scale\":0}", "HDMI-A-1", 0));      /* back to the recommended one */
    CHECK(msg("{\"a\":\"display\",\"scaleOn\":\"DP-10\",\"scale\":300}", "DP-10", 300));
    CHECK(msg("{\"a\":\"display\",\"scaleOn\":\"DP-2\",\"scale\":75}", "DP-2", 75));
    CHECK(msg("{\"a\":\"display\",\"scaleOn\":\"DP-2\",\"scale\":80}", "DP-2", 80));
    /* scales out_scale_valid refuses */
    CHECK(msg("{\"a\":\"display\",\"scaleOn\":\"DP-2\",\"scale\":101}", NULL, 0));
    CHECK(msg("{\"a\":\"display\",\"scaleOn\":\"DP-2\",\"scale\":325}", NULL, 0));
    CHECK(msg("{\"a\":\"display\",\"scaleOn\":\"DP-2\",\"scale\":50}", NULL, 0));
    CHECK(msg("{\"a\":\"display\",\"scaleOn\":\"DP-2\",\"scale\":1}", NULL, 0));
    CHECK(msg("{\"a\":\"display\",\"scaleOn\":\"DP-2\",\"scale\":-125}", NULL, 0));
    CHECK(msg("{\"a\":\"display\",\"scaleOn\":\"DP-2\",\"scale\":0125}", NULL, 0));
    CHECK(msg("{\"a\":\"display\",\"scaleOn\":\"DP-2\",\"scale\":00}", NULL, 0));
    CHECK(msg("{\"a\":\"display\",\"scaleOn\":\"DP-2\",\"scale\":1250}", NULL, 0));
    CHECK(msg("{\"a\":\"display\",\"scaleOn\":\"DP-2\",\"scale\":99999999999999999999}", NULL, 0));
    CHECK(msg("{\"a\":\"display\",\"scaleOn\":\"DP-2\",\"scale\":125.5}", NULL, 0));
    CHECK(msg("{\"a\":\"display\",\"scaleOn\":\"DP-2\",\"scale\":1e2}", NULL, 0));
    CHECK(msg("{\"a\":\"display\",\"scaleOn\":\"DP-2\",\"scale\":\"125\"}", NULL, 0));
    CHECK(msg("{\"a\":\"display\",\"scaleOn\":\"DP-2\",\"scale\":}", NULL, 0));
    /* connectors that aren't one */
    CHECK(msg("{\"a\":\"display\",\"scaleOn\":\"\",\"scale\":125}", NULL, 0));
    CHECK(msg("{\"a\":\"display\",\"scaleOn\":\"../../etc\",\"scale\":125}", NULL, 0));
    CHECK(msg("{\"a\":\"display\",\"scaleOn\":\"DP 2\",\"scale\":125}", NULL, 0));
    CHECK(msg("{\"a\":\"display\",\"scaleOn\":\"DP-2\\n\",\"scale\":125}", NULL, 0));
    CHECK(msg("{\"a\":\"display\",\"scaleOn\":\"DP-2\\\"\",\"scale\":125}", NULL, 0));
    CHECK(msg("{\"a\":\"display\",\"scaleOn\":\"DP-2:1\",\"scale\":125}", NULL, 0));
    CHECK(msg("{\"a\":\"display\",\"scaleOn\":\"\xc3\xa9\",\"scale\":125}", NULL, 0));
    CHECK(msg("{\"a\":\"display\",\"scaleOn\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\",\"scale\":125}", "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", 125));   /* 31 */
    CHECK(msg("{\"a\":\"display\",\"scaleOn\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\",\"scale\":125}", NULL, 0));                            /* 32 */
    /* other shapes: extra fields, spaces, another order, a forged user, truncated, trailing data */
    CHECK(msg("{\"a\":\"display\",\"scaleOn\":\"DP-2\",\"scale\":125,\"user\":\"root\"}", NULL, 0));
    CHECK(msg("{\"a\":\"display\",\"user\":\"admin\",\"scaleOn\":\"DP-2\",\"scale\":125}", NULL, 0));
    CHECK(msg("{\"a\":\"display\", \"scaleOn\":\"DP-2\",\"scale\":125}", NULL, 0));
    CHECK(msg("{\"a\":\"display\",\"scale\":125,\"scaleOn\":\"DP-2\"}", NULL, 0));
    CHECK(msg("{\"a\":\"display\",\"scaleOn\":\"DP-2\",\"scale\":125} ", NULL, 0));
    CHECK(msg("{\"a\":\"display\",\"scaleOn\":\"DP-2\",\"scale\":125}}", NULL, 0));
    CHECK(msg("{\"a\":\"display\",\"scaleOn\":\"DP-2\",\"scale\":125", NULL, 0));
    CHECK(msg("{\"a\":\"display\",\"scaleOn\":\"DP-2", NULL, 0));
    CHECK(msg("{\"a\":\"display\",\"main\":\"DP-2\"}", NULL, 0));
    CHECK(msg("{\"a\":\"lock\",\"scaleOn\":\"DP-2\",\"scale\":125}", NULL, 0));
    CHECK(msg("", NULL, 0));
    CHECK(msg(NULL, NULL, 0));
    char c[GSCALE_CONN_MAX + 1];
    int p;
    CHECK(gscale_from_msg("{\"a\":\"display\",\"scaleOn\":\"DP-2\",\"scale\":125}", c, GSCALE_CONN_MAX, &p) == -1);   /* too small a buffer */
    CHECK(gscale_from_msg("{\"a\":\"display\",\"scaleOn\":\"DP-2\",\"scale\":125}", c, sizeof c, NULL) == -1);
    /* lord-server's side writes exactly what the root side takes */
    char m[128];
    CHECK(!gscale_msg(m, sizeof m, "eDP-1", 150) && msg(m, "eDP-1", 150));
    CHECK(!gscale_msg(m, sizeof m, "eDP-1", 0) && msg(m, "eDP-1", 0));
    CHECK(gscale_msg(m, sizeof m, "eDP-1", 140) == -1 && gscale_msg(m, sizeof m, "e\"DP", 150) == -1 && gscale_msg(m, sizeof m, NULL, 150) == -1);
    CHECK(gscale_msg(m, 20, "eDP-1", 150) == -1);
    for (int v = -5; v <= 1000; v++) {
        int ok = v == 0 || out_scale_valid(v);
        CHECK((gscale_msg(m, sizeof m, "DP-1", v) == 0) == ok);
        if (ok) CHECK(msg(m, "DP-1", v));
    }
}

static void t_file_env(void) {
    char c[GSCALE_CONN_MAX + 1];
    int p = 0;
    CHECK(!gscale_parse_file("eDP-1 125\n", c, sizeof c, &p) && !strcmp(c, "eDP-1") && p == 125);
    CHECK(!gscale_parse_file("HDMI-A-1 200", c, sizeof c, &p) && !strcmp(c, "HDMI-A-1") && p == 200);
    CHECK(gscale_parse_file("eDP-1 125\n\n", c, sizeof c, &p) == -1);
    CHECK(gscale_parse_file("eDP-1 0\n", c, sizeof c, &p) == -1);          /* the file never holds 0: it is removed */
    CHECK(gscale_parse_file("eDP-1  125\n", c, sizeof c, &p) == -1);
    CHECK(gscale_parse_file("eDP-1 125 x\n", c, sizeof c, &p) == -1);
    CHECK(gscale_parse_file("eDP-1:125\n", c, sizeof c, &p) == -1);
    CHECK(gscale_parse_file(" 125\n", c, sizeof c, &p) == -1);
    CHECK(gscale_parse_file("", c, sizeof c, &p) == -1);
    CHECK(!gscale_parse_env("eDP-1:175", c, sizeof c, &p) && !strcmp(c, "eDP-1") && p == 175);
    CHECK(gscale_parse_env("eDP-1:175\n", c, sizeof c, &p) == -1);
    CHECK(gscale_parse_env("eDP-1 175", c, sizeof c, &p) == -1);
    CHECK(gscale_parse_env("eDP-1:", c, sizeof c, &p) == -1);
    CHECK(gscale_parse_env(":175", c, sizeof c, &p) == -1);
    CHECK(gscale_parse_env("eDP-1:176", c, sizeof c, &p) == -1);
    CHECK(gscale_parse_env(NULL, c, sizeof c, &p) == -1);
    CHECK(gscale_conn_ok("DP-1") && !gscale_conn_ok("") && !gscale_conn_ok(NULL) && !gscale_conn_ok("DP_1") && !gscale_conn_ok("DP/1"));
}

/* random bytes and mutations of a good message: never accepted unless the result is itself valid, never past a buffer
 * (ASan), and what is accepted always round-trips through gscale_msg */
static void t_fuzz(void) {
    static const char good[] = "{\"a\":\"display\",\"scaleOn\":\"HDMI-A-1\",\"scale\":125}";
    static const char alpha[] = "{}\":,-0123456789aAzZ \n\\/.\x01\xff\xc3\xa9";
    unsigned seed = 242;
    int accepted = 0, bad = 0;
    for (int it = 0; it < 200000; it++) {
        char buf[96];
        size_t n;
        seed = seed * 1103515245u + 12345u;
        if (it & 1) {
            n = (seed >> 8) % (sizeof buf - 1);
            for (size_t i = 0; i < n; i++) { seed = seed * 1103515245u + 12345u; buf[i] = alpha[(seed >> 16) % (sizeof alpha - 1)]; }
        } else {
            memcpy(buf, good, sizeof good);
            n = sizeof good - 1;
            int k = 1 + (int)((seed >> 12) % 4);
            for (int x = 0; x < k; x++) {
                seed = seed * 1103515245u + 12345u;
                size_t at = (seed >> 8) % (n + 1);
                int op = (int)((seed >> 20) % 3);
                char ch = alpha[(seed >> 4) % (sizeof alpha - 1)];
                if (op == 0 && at < n) buf[at] = ch;
                else if (op == 1 && n + 1 < sizeof buf) { memmove(buf + at + 1, buf + at, n - at); buf[at] = ch; n++; }
                else if (n && at < n) { memmove(buf + at, buf + at + 1, n - at - 1); n--; }
            }
        }
        buf[n] = 0;
        char c[GSCALE_CONN_MAX + 1], m[128];
        int p;
        if (gscale_from_msg(buf, c, sizeof c, &p) == 1) {
            accepted++;
            if (!gscale_conn_ok(c) || (p && !out_scale_valid(p)) || gscale_msg(m, sizeof m, c, p) || strcmp(m, buf)) bad++;
        }
        if (!gscale_parse_file(buf, c, sizeof c, &p) && (!gscale_conn_ok(c) || !out_scale_valid(p))) bad++;
        if (!gscale_parse_env(buf, c, sizeof c, &p) && (!gscale_conn_ok(c) || !out_scale_valid(p))) bad++;
    }
    CHECK(!bad);
    CHECK(accepted > 0);            /* some mutations land on another valid message (a connector letter) */
    printf("greeterscale-test: fuzz 200000 inputs, %d accepted, all valid and canonical\n", accepted);
}

int main(void) {
    t_msg();
    t_file_env();
    t_fuzz();
    printf("greeterscale-test: %d passed, %d failed\n", pass, failc);
    return failc ? 1 : 0;
}
