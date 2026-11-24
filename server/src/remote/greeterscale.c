/* greeterscale.c — the login screen's display scale: strict parsers (see greeterscale.h). */
#include "greeterscale.h"
#include "../outputs.h"

#include <stdio.h>
#include <string.h>

int gscale_conn_ok(const char *c) {
    if (!c) return 0;
    size_t n = 0;
    for (; c[n]; n++) {
        if (n >= GSCALE_CONN_MAX) return 0;
        char x = c[n];
        if (!((x >= 'A' && x <= 'Z') || (x >= 'a' && x <= 'z') || (x >= '0' && x <= '9') || x == '-')) return 0;
    }
    return n > 0;
}

/* a connector at *p, up to the character stop: copied to conn, *p past it */
static int take_conn(const char **p, char stop, char *conn, size_t cn) {
    const char *s = *p, *e = strchr(s, stop);
    if (!e || e == s || (size_t)(e - s) > GSCALE_CONN_MAX || (size_t)(e - s) >= cn) return -1;
    memcpy(conn, s, (size_t)(e - s));
    conn[e - s] = 0;
    if (!gscale_conn_ok(conn)) return -1;
    *p = e + 1;
    return 0;
}

/* a whole number at *p: 0, or 1 to 3 digits without a leading zero; *p past it */
static int take_pct(const char **p, int *pct) {
    const char *s = *p;
    int v = 0, d = 0;
    while (s[d] >= '0' && s[d] <= '9') { if (d >= 3) return -1; v = v * 10 + (s[d] - '0'); d++; }
    if (!d || (d > 1 && s[0] == '0')) return -1;
    *pct = v;
    *p = s + d;
    return 0;
}

int gscale_from_msg(const char *json, char *conn, size_t cn, int *pct) {
    static const char head[] = "{\"a\":\"display\",\"scaleOn\":\"", mid[] = ",\"scale\":";
    if (!json || !conn || !pct || cn <= GSCALE_CONN_MAX) return -1;
    if (strncmp(json, head, sizeof head - 1)) return -1;
    const char *p = json + sizeof head - 1;
    char c[GSCALE_CONN_MAX + 1];
    int v;
    if (take_conn(&p, '"', c, sizeof c)) return -1;
    if (strncmp(p, mid, sizeof mid - 1)) return -1;
    p += sizeof mid - 1;
    if (take_pct(&p, &v) || strcmp(p, "}")) return -1;
    if (v != 0 && !out_scale_valid(v)) return -1;
    snprintf(conn, cn, "%s", c);
    *pct = v;
    return 1;
}

static int parse_pair(const char *s, char sep, int nl, char *conn, size_t cn, int *pct) {
    if (!s || !conn || !pct || cn <= GSCALE_CONN_MAX) return -1;
    const char *p = s;
    char c[GSCALE_CONN_MAX + 1];
    int v;
    if (take_conn(&p, sep, c, sizeof c) || take_pct(&p, &v)) return -1;
    if (nl && *p == '\n') p++;
    if (*p || !out_scale_valid(v)) return -1;
    snprintf(conn, cn, "%s", c);
    *pct = v;
    return 0;
}

int gscale_parse_file(const char *s, char *conn, size_t cn, int *pct) { return parse_pair(s, ' ', 1, conn, cn, pct); }
int gscale_parse_env(const char *s, char *conn, size_t cn, int *pct) { return parse_pair(s, ':', 0, conn, cn, pct); }

int gscale_msg(char *out, size_t n, const char *conn, int pct) {
    if (!out || !gscale_conn_ok(conn) || (pct != 0 && !out_scale_valid(pct))) return -1;
    int r = snprintf(out, n, "{\"a\":\"display\",\"scaleOn\":\"%s\",\"scale\":%d}", conn, pct);
    return r > 0 && (size_t)r < n ? 0 : -1;
}
