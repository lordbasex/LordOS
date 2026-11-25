/* testbench_globals_test.c — lordos-testbench's registry comparison (src/testbench/globtable.c) against the table
 * generated from docs/wayland-protocols.json: every state, wl_output advertised twice, extras counted once, and
 * hostile interface names that must not break the JSON. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/testbench/globtable.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static int find_row(const char *iface) {
    for (int i = 0; i < gt_expected_count(); i++) if (!strcmp(gt_expected(i)->iface, iface)) return i;
    return -1;
}

/* every expected global at its version, as lord-server advertises them to an ordinary client */
static int lordos_like(Global *g, int max, int with_restricted) {
    int n = 0;
    for (int i = 0; i < gt_expected_count() && n < max; i++) {
        const TbExpect *e = gt_expected(i);
        if (!with_restricted && e->offered[0]) continue;
        g[n].name = (uint32_t)n + 1;
        g[n].version = e->version;
        snprintf(g[n].iface, sizeof g[n].iface, "%s", e->iface);
        n++;
    }
    return n;
}

static char *json_of(const Global *g, int n) {
    char *buf = NULL;
    size_t len = 0;
    FILE *o = open_memstream(&buf, &len);
    if (!o) return NULL;
    gt_json(o, g, n);
    fclose(o);
    return buf;
}

int main(void) {
    Global g[128];
    GtRow rows[64];
    GtSummary s;
    int N = gt_expected_count();
    CHECK(N >= 48 && N <= 64, "the table has %d globals", N);
    /* the table's own shape: names, versions, rows; the globals only some clients get */
    for (int i = 0; i < N; i++) {
        const TbExpect *e = gt_expected(i);
        CHECK(e->iface[0] && e->version > 0 && e->protocol[0], "row %d is empty", i);
        for (int j = 0; j < i; j++) CHECK(strcmp(gt_expected(j)->iface, e->iface), "%s twice", e->iface);
    }
    CHECK(gt_expected(N) == NULL && gt_expected(-1) == NULL, "out of range rows");
    int xw = find_row("xwayland_shell_v1"), dl = find_row("wp_drm_lease_device_v1"), wm = find_row("xdg_wm_base");
    CHECK(xw >= 0 && !strcmp(gt_expected(xw)->offered, "xwayland"), "xwayland_shell_v1 is Xwayland's only");
    CHECK(dl >= 0 && !strcmp(gt_expected(dl)->offered, "headset"), "drm-lease needs a headset");
    CHECK(wm >= 0 && !gt_expected(wm)->offered[0], "xdg_wm_base is everyone's");

    /* lord-server without a headset, seen by an ordinary client: no missing global, the restricted ones "not here" */
    int n = lordos_like(g, 128, 0);
    gt_compare(g, n, rows, &s);
    CHECK(s.missing == 0 && s.older == 0 && s.newer == 0 && s.extra == 0, "ordinary client: missing %d older %d newer %d extra %d", s.missing, s.older, s.newer, s.extra);
    CHECK(s.offered + s.not_here == N && s.not_here >= 2, "ordinary client: offered %d not here %d", s.offered, s.not_here);
    CHECK(rows[xw].state == GT_NOT_HERE && rows[xw].advertised == 0, "xwayland_shell_v1 state %s", gt_state_name(rows[xw].state));
    /* everything (the conformance setup: a headset plugged in, Xwayland's connection): all offered */
    n = lordos_like(g, 128, 1);
    gt_compare(g, n, NULL, &s);
    CHECK(s.offered == N && s.not_here == 0, "all: offered %d of %d", s.offered, N);

    /* xdg_wm_base one version lower, wl_seat one higher, wl_compositor gone, wl_output twice (two displays), two extras
     * (one advertised twice) */
    n = lordos_like(g, 120, 1);
    int c = -1, seat = -1, out = -1;
    for (int i = 0; i < n; i++) {
        if (!strcmp(g[i].iface, "xdg_wm_base")) g[i].version--;
        if (!strcmp(g[i].iface, "wl_seat")) { g[i].version++; seat = find_row("wl_seat"); }
        if (!strcmp(g[i].iface, "wl_compositor")) c = i;
        if (!strcmp(g[i].iface, "wl_output")) out = i;
    }
    CHECK(c >= 0 && out >= 0 && seat >= 0, "fixture");
    g[c] = g[--n];
    g[n] = g[out];
    g[n].version = 2;                   /* the second display's global, older: the highest one counts */
    n++;
    snprintf(g[n].iface, sizeof g[n].iface, "zwlr_layer_shell_v1"); g[n++].version = 4;
    snprintf(g[n].iface, sizeof g[n].iface, "zwlr_layer_shell_v1"); g[n++].version = 4;
    snprintf(g[n].iface, sizeof g[n].iface, "%s", "x\"},{\"y\\\n<script>"); g[n++].version = 1;
    gt_compare(g, n, rows, &s);
    CHECK(rows[wm].state == GT_OLDER && rows[wm].advertised == gt_expected(wm)->version - 1, "xdg_wm_base: %s", gt_state_name(rows[wm].state));
    CHECK(rows[seat].state == GT_NEWER, "wl_seat: %s", gt_state_name(rows[seat].state));
    CHECK(rows[find_row("wl_compositor")].state == GT_MISSING, "wl_compositor missing");
    CHECK(rows[find_row("wl_output")].state == GT_OFFERED, "wl_output twice, highest version counts");
    CHECK(s.missing == 1 && s.older == 1 && s.newer == 1 && s.extra == 2 && s.offered == N - 3, "summary: %d %d %d %d %d", s.missing, s.older, s.newer, s.extra, s.offered);

    char *j = json_of(g, n);
    CHECK(j && strstr(j, "\"missing\":1,") && strstr(j, "\"older\":1,") && strstr(j, "\"extra\":[\"zwlr_layer_shell_v1\",\"x?????y???script?\"]"), "json: %.300s", j ? j : "(null)");
    CHECK(j && !strchr(j, '<') && !strchr(j, '\n') && !strstr(j, "\\\""), "hostile name reached the JSON");
    CHECK(j && strstr(j, "{\"interface\":\"xdg_wm_base\",\"protocol\":\"xdg-shell\""), "row");
    CHECK(j && strstr(j, "\"state\":\"older\""), "older row");
    CHECK(j && strstr(j, "\"offeredTo\":\"xwayland\""), "offeredTo");
    /* balanced braces and brackets, every quote paired: a cheap well-formedness check */
    int br = 0, sq = 0, q = 0;
    for (const char *p = j; p && *p; p++) {
        if (*p == '"') q ^= 1;
        else if (!q) { br += (*p == '{') - (*p == '}'); sq += (*p == '[') - (*p == ']'); CHECK(br >= 0 && sq >= 0, "unbalanced"); }
    }
    CHECK(j && !br && !sq && !q, "json not balanced");
    free(j);

    /* the globals array */
    char *buf = NULL;
    size_t len = 0;
    FILE *o = open_memstream(&buf, &len);
    gt_json_globals(o, g, 2);
    fclose(o);
    CHECK(buf && buf[0] == '[' && buf[len - 1] == ']' && strstr(buf, "\"version\":"), "globals array: %s", buf ? buf : "");
    free(buf);

    /* nothing advertised (a broken connection) */
    gt_compare(g, 0, rows, &s);
    CHECK(s.offered == 0 && s.missing + s.not_here == N && s.extra == 0, "empty registry");
    CHECK(!strcmp(gt_state_name(99), "?") && !strcmp(gt_offered_why("xwayland"), "offered only to Xwayland"), "names");

    if (fails) { fprintf(stderr, "testbench_globals_test: %d failed\n", fails); return 1; }
    printf("testbench_globals_test: ok (%d expected globals)\n", N);
    return 0;
}
