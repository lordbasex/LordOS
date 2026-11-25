/* globtable.c — a compositor's registry against LordOS's protocol table (globtable.h). */
#include "globtable.h"

#include <string.h>

#include "expected_globals.h"

int gt_expected_count(void) { return TB_EXPECTED_N; }
const TbExpect *gt_expected(int i) { return i >= 0 && i < TB_EXPECTED_N ? &tb_expected[i] : NULL; }

/* the highest version an interface is advertised at, 0 when it isn't */
static uint32_t advertised(const Global *g, int n, const char *iface) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++)
        if (!strncmp(g[i].iface, iface, sizeof g[i].iface) && g[i].version > v) v = g[i].version;
    return v;
}

static int expected_iface(const char *iface) {
    for (int i = 0; i < TB_EXPECTED_N; i++) if (!strncmp(tb_expected[i].iface, iface, 64)) return 1;
    return 0;
}

void gt_compare(const Global *g, int n, GtRow *rows, GtSummary *s) {
    GtSummary z = { 0 };
    z.expected = TB_EXPECTED_N;
    for (int i = 0; i < TB_EXPECTED_N; i++) {
        const TbExpect *e = &tb_expected[i];
        uint32_t v = advertised(g, n, e->iface);
        int st = !v ? (e->offered[0] ? GT_NOT_HERE : GT_MISSING) : v == e->version ? GT_OFFERED : v > e->version ? GT_NEWER : GT_OLDER;
        switch (st) {
        case GT_OFFERED: z.offered++; break;
        case GT_NEWER: z.newer++; break;
        case GT_OLDER: z.older++; break;
        case GT_MISSING: z.missing++; break;
        default: z.not_here++; break;
        }
        if (rows) { rows[i].state = st; rows[i].advertised = v; }
    }
    /* advertised, not in the table (each interface once) */
    for (int i = 0; i < n; i++) {
        if (expected_iface(g[i].iface)) continue;
        int dup = 0;
        for (int j = 0; j < i && !dup; j++) dup = !strncmp(g[j].iface, g[i].iface, sizeof g[i].iface);
        if (!dup) z.extra++;
    }
    if (s) *s = z;
}

const char *gt_state_name(int state) {
    static const char *n[] = { "offered", "newer", "older", "missing", "not-here" };
    return state >= 0 && state <= GT_NOT_HERE ? n[state] : "?";
}

const char *gt_offered_why(const char *offered) {
    if (!offered || !offered[0]) return "offered to every client";
    if (!strcmp(offered, "xwayland")) return "offered only to Xwayland";
    if (!strcmp(offered, "headset")) return "offered only while a VR headset is plugged in";
    if (!strcmp(offered, "render-node")) return "offered only with a render node that has timeline syncobjs";
    if (!strcmp(offered, "unsandboxed")) return "hidden from sandboxed apps";
    return "offered only to some clients";
}

/* a compositor's interface name: only [A-Za-z0-9_], at most 63 characters */
static void name_json(FILE *o, const char *s) {
    fputc('"', o);
    for (size_t i = 0; i < 63 && s[i]; i++) {
        char c = s[i];
        fputc((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ? c : '?', o);
    }
    fputc('"', o);
}

void gt_json_globals(FILE *o, const Global *g, int n) {
    fputc('[', o);
    for (int i = 0; i < n; i++) {
        fprintf(o, "%s{\"interface\":", i ? "," : "");
        name_json(o, g[i].iface);
        fprintf(o, ",\"version\":%u}", g[i].version);
    }
    fputc(']', o);
}

void gt_json(FILE *o, const Global *g, int n) {
    GtRow rows[TB_EXPECTED_N];
    GtSummary s;
    gt_compare(g, n, rows, &s);
    fprintf(o, "{\"source\":\"docs/wayland-protocols.json\",\"expected\":%d,\"offered\":%d,\"newer\":%d,\"older\":%d,\"missing\":%d,\"notHere\":%d,\"extra\":[",
            s.expected, s.offered, s.newer, s.older, s.missing, s.not_here);
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (expected_iface(g[i].iface)) continue;
        int dup = 0;
        for (int j = 0; j < i && !dup; j++) dup = !strncmp(g[j].iface, g[i].iface, sizeof g[i].iface);
        if (dup) continue;
        if (k++) fputc(',', o);
        name_json(o, g[i].iface);
    }
    fprintf(o, "],\"rows\":[");
    for (int i = 0; i < TB_EXPECTED_N; i++) {
        const TbExpect *e = &tb_expected[i];
        fprintf(o, "%s{\"interface\":\"%s\",\"protocol\":\"%s\",\"expected\":%u,\"advertised\":%u,\"state\":\"%s\",\"offeredTo\":\"%s\",\"partial\":%s}",
                i ? "," : "", e->iface, e->protocol, e->version, rows[i].advertised, gt_state_name(rows[i].state), e->offered[0] ? e->offered : "all",
                e->partial ? "true" : "false");
    }
    fprintf(o, "]}");
}
