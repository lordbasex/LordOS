/* geocities.c — searching and looking up assets/geo/cities.tsv (see geocities.h). */
#include "geocities.h"
#include "../sec/json.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int geo_query_ok(const char *q) {
    size_t l = q ? strlen(q) : 0;
    if (l < 2 || l > 40) return 0;
    for (const char *p = q; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == ' ' || *p == '\'' || *p == '-' || *p == '.')) return 0;
    return 1;
}

/* the fields of one line: id, name, ascii (may be empty), cc, adm1, lat, lon */
typedef struct { long id; char *name, *ascii, *cc, *adm1; double lat, lon; } Row;
static int split(char *line, Row *r) {
    char *f[7];
    int k = 0;
    f[k++] = line;
    for (char *p = line; *p && k < 7; p++) if (*p == '\t') { *p = 0; f[k++] = p + 1; }
    if (k != 7) return -1;
    char *e;
    r->id = strtol(f[0], &e, 10);
    if (*e || r->id <= 0) return -1;
    r->name = f[1]; r->ascii = f[2][0] ? f[2] : f[1]; r->cc = f[3]; r->adm1 = f[4];
    r->lat = strtod(f[5], &e);
    if (*e || r->lat < -90 || r->lat > 90) return -1;
    r->lon = strtod(f[6], &e);
    if ((*e && *e != '\n' && *e != '\r') || r->lon < -180 || r->lon > 180) return -1;
    if (strlen(r->cc) != 2 || strlen(r->name) > 80) return -1;
    return 0;
}

int geo_lookup(const char *path, long id, double *lat, double *lon, char *name, size_t n) {
    FILE *f = fopen(path, "re");
    if (!f) return -1;
    char line[512];
    int found = -1;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        if (strtol(line, NULL, 10) != id) continue;
        line[strcspn(line, "\r\n")] = 0;
        Row r;
        if (split(line, &r)) break;
        *lat = r.lat; *lon = r.lon;
        if (name && n) snprintf(name, n, "%s", r.name);
        found = 0;
        break;
    }
    fclose(f);
    return found;
}

static void lower(const char *s, char *out, size_t n) {
    size_t i = 0;
    for (; s[i] && i + 1 < n; i++) out[i] = (char)tolower((unsigned char)s[i]);
    out[i] = 0;
}

size_t geo_search_json(const char *path, const char *query, int max, char *out, size_t n) {
    if (n < 3) return 0;
    snprintf(out, n, "[]");
    if (!geo_query_ok(query) || max <= 0) return 2;
    FILE *f = fopen(path, "re");
    if (!f) return 2;
    if (max > 30) max = 30;
    size_t o = 1;
    out[0] = '[';
    int got = 0;
    /* two passes: names that start with the query, then the ones that contain it */
    for (int pass = 0; pass < 2 && got < max; pass++) {
        rewind(f);
        char line[512];
        while (got < max && fgets(line, sizeof line, f)) {
            if (line[0] == '#') continue;
            line[strcspn(line, "\r\n")] = 0;
            Row r;
            if (split(line, &r)) continue;
            char a[96];
            lower(r.ascii, a, sizeof a);
            char *hit = strstr(a, query);
            if (!hit || (pass == 0 && hit != a) || (pass == 1 && hit == a)) continue;
            char nm[200], ad[48];
            json_quote(nm, sizeof nm, r.name);
            json_quote(ad, sizeof ad, r.adm1);
            int w = snprintf(out + o, n - o, "%s{\"id\":%ld,\"name\":%s,\"cc\":\"%s\",\"adm1\":%s}", got ? "," : "", r.id, nm, r.cc, ad);
            if (w < 0 || (size_t)w >= n - o - 1) break;
            o += (size_t)w;
            got++;
        }
    }
    fclose(f);
    out[o++] = ']';
    out[o] = 0;
    return o;
}
