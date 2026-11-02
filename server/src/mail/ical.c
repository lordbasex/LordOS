/* ical.c — see ical.h. */
#include "ical.h"
#include "../ui/tzif.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Bounded integer from untrusted text (strtol with range and end checks; never atoi).
 * Leading sign allowed; parsing stops at the first non-digit (reported in *end if given). */
static int bnum(const char *s, long lo, long hi, long dflt, const char **end) {
    char *e;
    errno = 0;
    long v = strtol(s ? s : "", &e, 10);
    if (end) *end = e;
    if (!s || e == s || errno == ERANGE || v < lo || v > hi) return (int)dflt;
    return (int)v;
}

/* ---- content lines ---- */

static IcComp *comp_new(const char *name, IcComp *parent) {
    IcComp *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    snprintf(c->name, sizeof c->name, "%s", name);
    for (char *p = c->name; *p; p++) *p = (char)toupper((unsigned char)*p);
    c->parent = parent;
    if (parent) { if (parent->last) parent->last->next = c; else parent->kid = c; parent->last = c; }
    return c;
}

void ic_free(IcComp *root) {
    IcComp *n = root;
    while (n) {
        if (n->kid) { IcComp *k = n->kid; n->kid = NULL; n = k; continue; }
        IcComp *next = n->next, *parent = n->parent;
        int last = n == root;
        for (int i = 0; i < n->nprops; i++) { free(n->props[i].params); free(n->props[i].value); }
        free(n->props);
        free(n);
        if (last) break;
        n = next ? next : parent;
    }
}

static int add_prop(IcComp *c, const char *line, size_t n, long *total) {
    if (++*total > IC_MAX_PROPS * 50) return -1;
    if (c->nprops >= IC_MAX_PROPS) return 0;
    /* name[;params]:value (':' inside a quoted parameter does not count) */
    size_t i = 0;
    while (i < n && line[i] != ';' && line[i] != ':') i++;
    if (i == n || i == 0) return 0;
    size_t name_end = i;
    size_t params_start = i, colon = i;
    if (line[i] == ';') {
        int q = 0;
        for (colon = i; colon < n; colon++) { if (line[colon] == '"') q = !q; else if (line[colon] == ':' && !q) break; }
        if (colon >= n) return 0;
        params_start = i + 1;
    }
    if (c->nprops == c->capprops) {
        int nc = c->capprops ? c->capprops * 2 : 16;
        IcProp *np = realloc(c->props, (size_t)nc * sizeof *np);
        if (!np) return -1;
        c->props = np;
        c->capprops = nc;
    }
    IcProp *p = &c->props[c->nprops];
    memset(p, 0, sizeof *p);
    /* group prefix ("item1.EMAIL") */
    const char *nm = line;
    size_t nl = name_end;
    const char *dot = memchr(line, '.', name_end);
    if (dot) {
        size_t gl = (size_t)(dot - line);
        if (gl >= sizeof p->group) gl = sizeof p->group - 1;
        memcpy(p->group, line, gl);
        nm = dot + 1;
        nl = name_end - (size_t)(nm - line);
    }
    if (nl >= sizeof p->name) nl = sizeof p->name - 1;
    for (size_t k = 0; k < nl; k++) p->name[k] = (char)toupper((unsigned char)nm[k]);
    p->name[nl] = 0;
    size_t pl = colon > params_start ? colon - params_start : 0;
    p->params = malloc(pl + 1);
    p->value = malloc(n - colon);
    if (!p->params || !p->value) { free(p->params); free(p->value); return -1; }
    memcpy(p->params, line + params_start, pl);
    p->params[pl] = 0;
    memcpy(p->value, line + colon + 1, n - colon - 1);
    p->value[n - colon - 1] = 0;
    c->nprops++;
    return 0;
}

IcComp *ic_parse(const char *s, size_t len, const char **err) {
    IcComp *root = comp_new("", NULL);
    if (!root) { if (err) *err = "memory"; return NULL; }
    IcComp *cur = root;
    int depth = 0;
    long lines = 0, total = 0;
    char *logical = NULL;
    size_t ll = 0, lcap = 0;
    size_t i = 0;
    const char *e = NULL;
    if (len >= 3 && (unsigned char)s[0] == 0xef && (unsigned char)s[1] == 0xbb && (unsigned char)s[2] == 0xbf) i = 3;
    for (;;) {
        /* next physical line */
        int have = i < len;
        size_t a = i, b = i;
        if (have) {
            while (b < len && s[b] != '\n') b++;
            i = b < len ? b + 1 : len;
            if (b > a && s[b - 1] == '\r') b--;
            if (++lines > IC_MAX_LINES) { e = "too many lines"; goto fail; }
        }
        int cont = have && b > a && (s[a] == ' ' || s[a] == '\t');
        /* vCard 2.1: quoted-printable soft line break ("=" at the end) */
        int qp = 0;
        if (have && !cont && ll && logical[ll - 1] == '=') {
            size_t cl = 0;
            while (cl < ll && logical[cl] != ':') cl++;
            for (size_t k = 0; k + 16 <= cl; k++) if (!strncasecmp(logical + k, "QUOTED-PRINTABLE", 16)) { qp = 1; break; }
        }
        if (cont || qp) {
            size_t skip = cont ? 1 : 0;
            size_t add = b - a - skip;
            if (qp) ll--;          /* drop the "=" */
            if (ll + add + 1 > lcap) { size_t nc = (ll + add + 1) * 2; char *nl = realloc(logical, nc); if (!nl) { e = "memory"; goto fail; } logical = nl; lcap = nc; }
            memcpy(logical + ll, s + a + skip, add);
            ll += add;
            continue;
        }
        /* the previous logical line is complete */
        if (ll) {
            if (ll >= 6 && !strncasecmp(logical, "BEGIN:", 6)) {
                if (depth >= IC_MAX_DEPTH) { e = "too deep"; goto fail; }
                char nm[24];
                size_t n = ll - 6;
                while (n && (logical[6 + n - 1] == ' ')) n--;
                if (n >= sizeof nm) n = sizeof nm - 1;
                memcpy(nm, logical + 6, n);
                nm[n] = 0;
                IcComp *c = comp_new(nm, cur);
                if (!c) { e = "memory"; goto fail; }
                cur = c;
                depth++;
            } else if (ll >= 4 && !strncasecmp(logical, "END:", 4)) {
                char nm[24];
                size_t n = ll - 4;
                while (n && logical[4 + n - 1] == ' ') n--;
                if (n >= sizeof nm) n = sizeof nm - 1;
                memcpy(nm, logical + 4, n);
                nm[n] = 0;
                /* close up to the matching component (tolerates missing END lines) */
                IcComp *m = cur;
                while (m && m != root && strcasecmp(m->name, nm)) m = m->parent;
                if (m && m != root) {
                    while (cur != m) { cur = cur->parent; depth--; }
                    cur = cur->parent;
                    depth--;
                }
            } else if (add_prop(cur, logical, ll, &total)) { e = "too many properties"; goto fail; }
        }
        ll = 0;
        if (!have) break;
        size_t add = b - a;
        if (add + 1 > lcap) { size_t nc = (add + 1) * 2; char *nl = realloc(logical, nc); if (!nl) { e = "memory"; goto fail; } logical = nl; lcap = nc; }
        memcpy(logical, s + a, add);
        ll = add;
    }
    free(logical);
    if (err) *err = NULL;
    return root;
fail:
    free(logical);
    ic_free(root);
    if (err) *err = e;
    return NULL;
}

IcProp *ic_prop(const IcComp *c, const char *name) {
    for (int i = 0; c && i < c->nprops; i++) if (!strcmp(c->props[i].name, name)) return &c->props[i];
    return NULL;
}

IcProp *ic_prop_next(const IcComp *c, const IcProp *after, const char *name) {
    if (!c) return NULL;
    int start = after ? (int)(after - c->props) + 1 : 0;
    for (int i = start; i < c->nprops; i++) if (!strcmp(c->props[i].name, name)) return &c->props[i];
    return NULL;
}

int ic_param(const IcProp *p, const char *name, char *out, size_t cap) {
    if (cap) out[0] = 0;
    if (!p || !p->params) return 0;
    size_t nl = strlen(name);
    const char *s = p->params;
    while (*s) {
        const char *a = s;
        while (*s && *s != '=' && *s != ';') s++;
        size_t kl = (size_t)(s - a);
        int match = kl == nl && !strncasecmp(a, name, nl);
        /* vCard 2.1: bare parameters ("TEL;WORK;VOICE:") count as TYPE */
        if (*s != '=') {
            if (!strcasecmp(name, "TYPE") && kl && cap) {
                size_t cur = strlen(out);
                if (cur + kl + 2 < cap) { if (cur) out[cur++] = ','; memcpy(out + cur, a, kl); out[cur + kl] = 0; }
                if (*s == ';') s++;
                continue;
            }
            if (*s == ';') s++;
            continue;
        }
        s++;
        size_t o = match && cap ? strlen(out) : 0;
        if (match && o && o + 1 < cap) out[o++] = ',';
        int q = 0;
        while (*s && (q || *s != ';')) {
            if (*s == '"') { q = !q; s++; continue; }
            if (match && cap && o + 1 < cap) out[o++] = *s;
            s++;
        }
        if (match && cap) { out[o] = 0; if (strcasecmp(name, "TYPE")) return 1; }
        if (*s == ';') s++;
    }
    return cap && out[0];
}

void ic_unescape(const char *in, char *out, size_t cap) {
    if (!cap) return;
    size_t o = 0;
    for (; in && *in && o + 1 < cap; in++) {
        if (*in == '\\' && in[1]) {
            in++;
            if (*in == 'n' || *in == 'N') out[o++] = '\n';
            else out[o++] = *in;
        } else out[o++] = *in;
    }
    out[o] = 0;
    /* do not cut a UTF-8 character */
    while (o && ((unsigned char)out[o - 1] & 0xc0) == 0x80 && o + 1 >= cap) out[--o] = 0;
}

char *ic_unescape_dup(const char *in) {
    size_t n = in ? strlen(in) : 0;
    char *o = malloc(n + 1);
    if (o) ic_unescape(in ? in : "", o, n + 1);
    return o;
}

char *ic_escape(const char *in) {
    size_t n = in ? strlen(in) : 0;
    char *o = malloc(n * 2 + 1);
    if (!o) return NULL;
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        char c = in[i];
        if (c == '\r') continue;
        if (c == '\n') { o[k++] = '\\'; o[k++] = 'n'; continue; }
        if (c == '\\' || c == ',' || c == ';') o[k++] = '\\';
        o[k++] = c;
    }
    o[k] = 0;
    return o;
}

void ic_line(JBuf *b, const char *line) {
    size_t n = strlen(line), col = 0;
    for (size_t i = 0; i < n;) {
        size_t k = 1;
        unsigned char c = (unsigned char)line[i];
        if (c >= 0xf0) k = 4; else if (c >= 0xe0) k = 3; else if (c >= 0xc0) k = 2;
        if (i + k > n) k = n - i;
        if (col + k > 75) { jb_raw(b, "\r\n "); col = 1; }
        {
            char t[5];
            memcpy(t, line + i, k);
            t[k] = 0;
            jb_raw(b, t);
        }
        col += k;
        i += k;
    }
    jb_raw(b, "\r\n");
}

/* ---- time ---- */

int64_t ic_days_from_civil(int64_t y, int m, int d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

void ic_civil_from_days(int64_t z, int *y, int *m, int *d) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yy = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int dd = (int)(doy - (153 * mp + 2) / 5 + 1);
    int mm = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yy + (mm <= 2));
    *m = mm;
    *d = dd;
}

static char local_zone[64] = "UTC";
void ic_set_local_zone(const char *z) { if (z && tz_zone_valid(z)) snprintf(local_zone, sizeof local_zone, "%s", z); }
const char *ic_local_zone(void) { return local_zone; }

typedef struct { char name[64]; uint8_t *buf; size_t n; int bad; } ZoneCache;
static ZoneCache zones[24];

static const struct { const char *win, *iana; } winzones[] = {
    { "Pacific Standard Time", "America/Los_Angeles" }, { "Mountain Standard Time", "America/Denver" }, { "Central Standard Time", "America/Chicago" },
    { "Eastern Standard Time", "America/New_York" }, { "GMT Standard Time", "Europe/London" }, { "Greenwich Standard Time", "Atlantic/Reykjavik" },
    { "W. Europe Standard Time", "Europe/Berlin" }, { "Romance Standard Time", "Europe/Paris" }, { "Central Europe Standard Time", "Europe/Budapest" },
    { "Central European Standard Time", "Europe/Warsaw" }, { "E. Europe Standard Time", "Europe/Chisinau" }, { "FLE Standard Time", "Europe/Kiev" },
    { "GTB Standard Time", "Europe/Bucharest" }, { "Russian Standard Time", "Europe/Moscow" }, { "Argentina Standard Time", "America/Argentina/Buenos_Aires" },
    { "E. South America Standard Time", "America/Sao_Paulo" }, { "SA Pacific Standard Time", "America/Bogota" }, { "Pacific SA Standard Time", "America/Santiago" },
    { "Central America Standard Time", "America/Guatemala" }, { "Central Standard Time (Mexico)", "America/Mexico_City" }, { "Mexico Standard Time", "America/Mexico_City" },
    { "Venezuela Standard Time", "America/Caracas" }, { "Montevideo Standard Time", "America/Montevideo" }, { "SA Western Standard Time", "America/La_Paz" },
    { "Paraguay Standard Time", "America/Asuncion" }, { "Tokyo Standard Time", "Asia/Tokyo" }, { "China Standard Time", "Asia/Shanghai" },
    { "India Standard Time", "Asia/Kolkata" }, { "AUS Eastern Standard Time", "Australia/Sydney" }, { "Arabian Standard Time", "Asia/Dubai" },
    { "Israel Standard Time", "Asia/Jerusalem" }, { "Korea Standard Time", "Asia/Seoul" }, { "Singapore Standard Time", "Asia/Singapore" },
    { "UTC", "UTC" }, { "Coordinated Universal Time", "UTC" }, { "Atlantic Standard Time", "America/Halifax" }, { "Hawaiian Standard Time", "Pacific/Honolulu" },
    { "Alaskan Standard Time", "America/Anchorage" }, { "US Mountain Standard Time", "America/Phoenix" }, { "SE Asia Standard Time", "Asia/Bangkok" },
    { "New Zealand Standard Time", "Pacific/Auckland" }, { "South Africa Standard Time", "Africa/Johannesburg" }, { "Turkey Standard Time", "Europe/Istanbul" },
};

static ZoneCache *zone_load(const char *zone) {
    char z[64];
    if (!zone || !*zone) zone = local_zone;
    snprintf(z, sizeof z, "%s", zone);
    for (size_t i = 0; i < sizeof winzones / sizeof winzones[0]; i++) if (!strcasecmp(z, winzones[i].win)) { snprintf(z, sizeof z, "%s", winzones[i].iana); break; }
    /* "/mozilla.org/20050126_1/Europe/Madrid", "/freeassociation.sourceforge.net/Tzfile/Europe/Madrid" */
    if (z[0] == '/') {
        char *last = strrchr(z, '/'), *prev = NULL;
        if (last) { *last = 0; prev = strrchr(z, '/'); *last = '/'; }
        if (prev) memmove(z, prev + 1, strlen(prev + 1) + 1);
        else if (last) memmove(z, last + 1, strlen(last + 1) + 1);
    }
    if (!strcasecmp(z, "Z") || !strcasecmp(z, "GMT") || !strcasecmp(z, "Etc/UTC")) snprintf(z, sizeof z, "UTC");
    for (size_t i = 0; i < sizeof zones / sizeof zones[0]; i++) if (zones[i].name[0] && !strcmp(zones[i].name, z)) return zones[i].bad ? NULL : &zones[i];
    size_t slot = 0;
    for (; slot < sizeof zones / sizeof zones[0]; slot++) if (!zones[slot].name[0]) break;
    if (slot == sizeof zones / sizeof zones[0]) { slot = 0; free(zones[0].buf); memset(&zones[0], 0, sizeof zones[0]); }
    ZoneCache *c = &zones[slot];
    snprintf(c->name, sizeof c->name, "%s", z);
    c->bad = 1;
    if (!tz_zone_valid(z)) return NULL;
    char path[160];
    snprintf(path, sizeof path, "/usr/share/zoneinfo/%s", z);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > (1 << 20)) { close(fd); return NULL; }
    c->buf = malloc((size_t)st.st_size);
    if (!c->buf) { close(fd); return NULL; }
    ssize_t r = read(fd, c->buf, (size_t)st.st_size);
    close(fd);
    if (r != st.st_size) { free(c->buf); c->buf = NULL; return NULL; }
    c->n = (size_t)r;
    c->bad = 0;
    return c;
}

int32_t ic_zone_offset(const char *zone, int64_t t) {
    ZoneCache *c = zone_load(zone);
    if (!c) c = zone_load(local_zone);
    if (!c) return 0;
    TzInfo ti;
    if (tzif_lookup(c->buf, c->n, t, &ti)) return 0;
    return ti.utoff;
}

int64_t ic_wall_to_utc(int64_t wall, const char *zone) {
    int32_t o1 = ic_zone_offset(zone, wall);
    int64_t t = wall - o1;
    int32_t o2 = ic_zone_offset(zone, t);
    if (o2 != o1) t = wall - o2;
    return t;
}

static int digits(const char *s, int n, int *out) {
    int v = 0;
    for (int i = 0; i < n; i++) { if (!isdigit((unsigned char)s[i])) return 0; v = v * 10 + (s[i] - '0'); }
    *out = v;
    return 1;
}

/* "20251018", "20251018T103000", "20251018T103000Z"; returns wall/UTC seconds */
static int parse_dt(const char *v, int64_t *t, int *allday, int *utc) {
    while (*v == ' ') v++;
    int y, mo, d, h = 0, mi = 0, s = 0;
    size_t n = strlen(v);
    if (n >= 10 && v[4] == '-' && v[7] == '-') {         /* vCard: 1990-05-17 */
        if (!digits(v, 4, &y) || !digits(v + 5, 2, &mo) || !digits(v + 8, 2, &d)) return 0;
        *allday = 1; *utc = 0;
        *t = ic_days_from_civil(y, mo, d) * 86400;
        return mo >= 1 && mo <= 12 && d >= 1 && d <= 31;
    }
    if (n < 8 || !digits(v, 4, &y) || !digits(v + 4, 2, &mo) || !digits(v + 6, 2, &d)) return 0;
    if (mo < 1 || mo > 12 || d < 1 || d > 31) return 0;
    *allday = 1;
    *utc = 0;
    if (n >= 15 && (v[8] == 'T' || v[8] == 't')) {
        if (!digits(v + 9, 2, &h) || !digits(v + 11, 2, &mi) || !digits(v + 13, 2, &s)) return 0;
        if (h > 23 || mi > 59 || s > 60) return 0;
        *allday = 0;
        *utc = n >= 16 && (v[15] == 'Z' || v[15] == 'z');
    }
    *t = ic_days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s;
    return 1;
}

int ic_time(const IcProp *p, int64_t *t, int *allday, char *tzid, size_t tzcap) {
    if (tzcap) tzid[0] = 0;
    if (!p || !p->value) return 0;
    char vt[32] = "";
    ic_param(p, "VALUE", vt, sizeof vt);
    int utc = 0;
    int64_t w;
    if (!parse_dt(p->value, &w, allday, &utc)) return 0;
    if (!strcasecmp(vt, "DATE")) *allday = 1;
    if (*allday) { *t = ic_wall_to_utc(w - w % 86400, NULL); return 1; }
    if (utc) { *t = w; return 1; }
    char z[64] = "";
    ic_param(p, "TZID", z, sizeof z);
    if (tzcap) snprintf(tzid, tzcap, "%s", z);
    *t = ic_wall_to_utc(w, z[0] ? z : NULL);
    return 1;
}

/* ISO 8601 duration ("-PT15M", "P1DT2H", "P2W") in seconds. */
static int parse_dur(const char *v, long long *out) {
    while (*v == ' ') v++;
    int neg = 0;
    if (*v == '-') { neg = 1; v++; } else if (*v == '+') v++;
    if (*v != 'P' && *v != 'p') return 0;
    v++;
    long long tot = 0;
    int time = 0, any = 0;
    while (*v) {
        if (*v == 'T' || *v == 't') { time = 1; v++; continue; }
        if (!isdigit((unsigned char)*v)) return 0;
        long long x = 0;
        while (isdigit((unsigned char)*v)) { x = x * 10 + (*v - '0'); if (x > 100000000) return 0; v++; }
        char u = (char)toupper((unsigned char)*v);
        if (!u) return 0;
        v++;
        any = 1;
        if (tot > 100000000000LL) return 0;
        if (u == 'W') tot += x * 604800;
        else if (u == 'D') tot += x * 86400;
        else if (u == 'H' && time) tot += x * 3600;
        else if (u == 'M' && time) tot += x * 60;
        else if (u == 'S' && time) tot += x;
        else return 0;
    }
    if (!any || tot > 100000000000LL) return 0;
    *out = neg ? -tot : tot;
    return 1;
}

/* ---- events ---- */

static void text_prop(const IcComp *c, const char *name, char *out, size_t cap) {
    IcProp *p = ic_prop(c, name);
    if (cap) out[0] = 0;
    if (p) ic_unescape(p->value, out, cap);
}

static void mailto(const char *v, char *out, size_t cap) {
    while (*v == ' ') v++;
    if (!strncasecmp(v, "mailto:", 7)) v += 7;
    snprintf(out, cap, "%s", v);
    for (char *p = out; *p; p++) *p = (char)tolower((unsigned char)*p);
}

static void fill_event(const IcComp *c, IcEvent *e) {
    memset(e, 0, sizeof *e);
    e->is_todo = !strcmp(c->name, "VTODO");
    text_prop(c, "UID", e->uid, sizeof e->uid);
    text_prop(c, "SUMMARY", e->summary, sizeof e->summary);
    text_prop(c, "LOCATION", e->location, sizeof e->location);
    text_prop(c, "URL", e->url, sizeof e->url);
    text_prop(c, "STATUS", e->status, sizeof e->status);
    for (char *p = e->status; *p; p++) *p = (char)toupper((unsigned char)*p);
    text_prop(c, "CATEGORIES", e->categories, sizeof e->categories);
    IcProp *d = ic_prop(c, "DESCRIPTION");
    e->description = ic_unescape_dup(d ? d->value : "");
    IcProp *rr = ic_prop(c, "RRULE");
    if (rr) snprintf(e->rrule, sizeof e->rrule, "%s", rr->value);
    IcProp *seq = ic_prop(c, "SEQUENCE");
    if (seq) e->sequence = bnum(seq->value, 0, 1000000000, 0, NULL);
    int ad;
    char tz[64];
    int64_t t;
    if (ic_time(ic_prop(c, "DTSTAMP"), &t, &ad, tz, sizeof tz)) e->dtstamp = t;
    if (ic_time(ic_prop(c, "LAST-MODIFIED"), &t, &ad, tz, sizeof tz)) e->modified = t;
    IcProp *ds = ic_prop(c, "DTSTART");
    if (ic_time(ds, &e->start, &e->allday, e->tzid, sizeof e->tzid)) {
        if (e->allday) {
            int64_t w; int u, a2;
            if (parse_dt(ds->value, &w, &a2, &u)) e->sdate = (int)(w / 86400);
        }
    }
    IcProp *de = ic_prop(c, e->is_todo ? "DUE" : "DTEND");
    if (e->is_todo) {
        if (de && ic_time(de, &e->due, &e->due_allday, tz, sizeof tz)) { /* ok */ }
        if (!ds) { e->start = e->due; e->allday = e->due_allday; }
    }
    if (!e->is_todo && de && ic_time(de, &e->end, &ad, tz, sizeof tz)) {
        if (e->allday) { int64_t w; int u, a2; if (parse_dt(de->value, &w, &a2, &u)) e->edate = (int)(w / 86400); }
    } else if (!e->is_todo) {
        long long dur = 0;
        IcProp *du = ic_prop(c, "DURATION");
        if (du && parse_dur(du->value, &dur) && dur >= 0) e->end = e->start + dur;
        else e->end = e->start + (e->allday ? 86400 : 0);
        if (e->allday) e->edate = e->sdate + (int)((e->end - e->start + 86399) / 86400);
    }
    if (e->allday && e->edate <= e->sdate) e->edate = e->sdate + 1;
    if (e->end < e->start) e->end = e->start;
    IcProp *rid = ic_prop(c, "RECURRENCE-ID");
    if (rid) ic_time(rid, &e->recurrence_id, &ad, tz, sizeof tz);
    for (IcProp *x = ic_prop(c, "EXDATE"); x; x = ic_prop_next(c, x, "EXDATE")) {
        /* comma separated, with the same parameters */
        char *v = x->value;
        while (*v && e->nexdate < 64) {
            char one[40];
            size_t n = strcspn(v, ",");
            if (n < sizeof one) {
                memcpy(one, v, n);
                one[n] = 0;
                IcProp tmp = *x;
                tmp.value = one;
                int64_t et;
                if (ic_time(&tmp, &et, &ad, tz, sizeof tz)) e->exdate[e->nexdate++] = et;
            }
            v += n;
            if (*v == ',') v++;
        }
    }
    IcProp *org = ic_prop(c, "ORGANIZER");
    if (org) { mailto(org->value, e->organizer, sizeof e->organizer); char cn[160]; if (ic_param(org, "CN", cn, sizeof cn)) snprintf(e->organizer_name, sizeof e->organizer_name, "%s", cn); }
    for (IcProp *a = ic_prop(c, "ATTENDEE"); a && e->natt < IC_MAX_ATT; a = ic_prop_next(c, a, "ATTENDEE")) {
        IcAttendee *na = realloc(e->att, (size_t)(e->natt + 1) * sizeof *na);
        if (!na) break;
        e->att = na;
        IcAttendee *x = &e->att[e->natt++];
        memset(x, 0, sizeof *x);
        mailto(a->value, x->email, sizeof x->email);
        ic_param(a, "CN", x->name, sizeof x->name);
        if (!ic_param(a, "PARTSTAT", x->partstat, sizeof x->partstat)) snprintf(x->partstat, sizeof x->partstat, "NEEDS-ACTION");
        for (char *p = x->partstat; *p; p++) *p = (char)toupper((unsigned char)*p);
        ic_param(a, "ROLE", x->role, sizeof x->role);
        char rs[16];
        x->rsvp = ic_param(a, "RSVP", rs, sizeof rs) && !strcasecmp(rs, "TRUE");
    }
    IcProp *pr = ic_prop(c, "PRIORITY");
    if (pr) e->priority = bnum(pr->value, 0, 9, 0, NULL);
    IcProp *co = ic_prop(c, "COMPLETED");
    if (co) ic_time(co, &e->completed, &ad, tz, sizeof tz);
    if (!e->completed && !strcmp(e->status, "COMPLETED")) e->completed = e->modified ? e->modified : 1;
    IcProp *fl = ic_prop(c, "X-LORDOS-FLAGGED");
    if (fl) e->flagged = bnum(fl->value, 0, 1, 0, NULL) != 0;
    IcProp *rel = ic_prop(c, "RELATED-TO");
    if (rel) ic_unescape(rel->value, e->parent_uid, sizeof e->parent_uid);
    /* alarms */
    for (IcComp *k = c->kid; k && e->nalarms < 8; k = k->next) {
        if (strcmp(k->name, "VALARM")) continue;
        IcProp *tr = ic_prop(k, "TRIGGER");
        if (!tr) continue;
        char vt[24] = "";
        ic_param(tr, "VALUE", vt, sizeof vt);
        long long dur;
        if (!strcasecmp(vt, "DATE-TIME")) {
            int64_t at;
            if (ic_time(tr, &at, &ad, tz, sizeof tz)) { int64_t ref = e->is_todo && e->due ? e->due : e->start; int64_t m = (ref - at) / 60; e->alarms[e->nalarms++] = (int)(m < -5000000 ? -5000000 : m > 5000000 ? 5000000 : m); }
        } else if (parse_dur(tr->value, &dur)) {
            char rel2[16] = "";
            ic_param(tr, "RELATED", rel2, sizeof rel2);
            long long before = -dur;
            if (!strcasecmp(rel2, "END") && !e->is_todo) before -= (e->end - e->start);
            long long m = before / 60;
            e->alarms[e->nalarms++] = (int)(m < -5000000 ? -5000000 : m > 5000000 ? 5000000 : m);
        }
    }
}

int ical_events(const char *text, size_t len, IcEvent **out, char *method, size_t mcap) {
    *out = NULL;
    if (mcap) method[0] = 0;
    const char *err;
    IcComp *root = ic_parse(text, len, &err);
    if (!root) return -1;
    int n = 0, cap = 0;
    IcEvent *ev = NULL;
    for (IcComp *cal = root->kid; cal; cal = cal->next) {
        if (strcmp(cal->name, "VCALENDAR")) continue;
        IcProp *m = ic_prop(cal, "METHOD");
        if (m && mcap && !method[0]) { snprintf(method, mcap, "%s", m->value); for (char *p = method; *p; p++) *p = (char)toupper((unsigned char)*p); }
        for (IcComp *c = cal->kid; c; c = c->next) {
            if (strcmp(c->name, "VEVENT") && strcmp(c->name, "VTODO")) continue;
            if (n == cap) {
                int nc = cap ? cap * 2 : 4;
                IcEvent *ne = realloc(ev, (size_t)nc * sizeof *ne);
                if (!ne) break;
                ev = ne; cap = nc;
            }
            fill_event(c, &ev[n++]);
            if (n >= 20000) break;
        }
    }
    ic_free(root);
    *out = ev;
    return n;
}

void ical_events_free(IcEvent *ev, int n) {
    for (int i = 0; i < n; i++) { free(ev[i].description); free(ev[i].att); }
    free(ev);
}

/* ---- recurrence ---- */

typedef struct {
    int freq;               /* 'D','W','M','Y' */
    int interval, count;
    int64_t until;
    int byday[16], byday_n[16], nbyday;     /* weekday 0=MO..6=SU, ordinal (0 = every) */
    int bymonthday[16], nbmd;
    int bymonth[12], nbm;
} Rule;

static int wd_index(const char *s) {
    static const char *w[] = { "MO", "TU", "WE", "TH", "FR", "SA", "SU" };
    for (int i = 0; i < 7; i++) if (!strncasecmp(s, w[i], 2)) return i;
    return -1;
}

static int parse_rule(const char *r, Rule *o) {
    memset(o, 0, sizeof *o);
    o->interval = 1;
    char buf[512];
    snprintf(buf, sizeof buf, "%s", r);
    for (char *save = NULL, *part = strtok_r(buf, ";", &save); part; part = strtok_r(NULL, ";", &save)) {
        char *eq = strchr(part, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = part, *v = eq + 1;
        if (!strcasecmp(k, "FREQ")) o->freq = !strcasecmp(v, "DAILY") ? 'D' : !strcasecmp(v, "WEEKLY") ? 'W' : !strcasecmp(v, "MONTHLY") ? 'M' : !strcasecmp(v, "YEARLY") ? 'Y' : 0;
        else if (!strcasecmp(k, "INTERVAL")) o->interval = bnum(v, 1, 1000, 1, NULL);
        else if (!strcasecmp(k, "COUNT")) o->count = bnum(v, 0, 100000, 0, NULL);
        else if (!strcasecmp(k, "UNTIL")) {
            int64_t t; int ad, utc;
            if (parse_dt(v, &t, &ad, &utc)) o->until = ad ? ic_wall_to_utc(t + 86399, NULL) : utc ? t : ic_wall_to_utc(t, NULL);
        } else if (!strcasecmp(k, "BYDAY")) {
            int maxd = (int)(sizeof o->byday / sizeof o->byday[0]);
            for (char *s2 = NULL, *d = strtok_r(v, ",", &s2); d && o->nbyday < maxd; d = strtok_r(NULL, ",", &s2)) {
                int ord = 0;
                if (*d == '+' || *d == '-' || isdigit((unsigned char)*d)) {
                    const char *e;
                    ord = bnum(d, -53, 53, 99, &e);           /* RFC 5545: ±1..53 */
                    if (ord == 99 || ord == 0) continue;
                    d = (char *)e;
                }
                int w = wd_index(d);
                if (w < 0) continue;
                o->byday[o->nbyday] = w;
                o->byday_n[o->nbyday] = ord;
                o->nbyday++;
            }
        } else if (!strcasecmp(k, "BYMONTHDAY")) {
            for (char *s2 = NULL, *d = strtok_r(v, ",", &s2); d && o->nbmd < 16; d = strtok_r(NULL, ",", &s2)) { int x = bnum(d, -31, 31, 0, NULL); if (x) o->bymonthday[o->nbmd++] = x; }
        } else if (!strcasecmp(k, "BYMONTH")) {
            for (char *s2 = NULL, *d = strtok_r(v, ",", &s2); d && o->nbm < 12; d = strtok_r(NULL, ",", &s2)) { int x = bnum(d, 1, 12, 0, NULL); if (x) o->bymonth[o->nbm++] = x; }
        }
    }
    return o->freq != 0;
}

static int weekday_of(int64_t days) { int64_t w = (days + 3) % 7; if (w < 0) w += 7; return (int)w; }   /* 1970-01-01 = Thursday → 0=MO */
static int month_len(int y, int m) { static const int ml[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 }; return m == 2 && ((y % 4 == 0 && y % 100) || y % 400 == 0) ? 29 : ml[m - 1]; }

static int in_list(const int *l, int n, int v) { for (int i = 0; i < n; i++) if (l[i] == v) return 1; return 0; }

/* days of a month that match the rule (ascending); returns count */
static int month_days(const Rule *r, int y, int m, int dflt_day, int64_t *out) {
    int ml = month_len(y, m), n = 0;
    int64_t first = ic_days_from_civil(y, m, 1);
    int pick[32] = { 0 };
    if (r->nbmd) {
        for (int i = 0; i < r->nbmd; i++) { int d = r->bymonthday[i] > 0 ? r->bymonthday[i] : ml + r->bymonthday[i] + 1; if (d >= 1 && d <= ml) pick[d] = 1; }
    } else if (r->nbyday) {
        for (int i = 0; i < r->nbyday; i++) {
            int w = r->byday[i], ord = r->byday_n[i];
            int fw = weekday_of(first);
            int d1 = 1 + (w - fw + 7) % 7;        /* first such weekday */
            if (ord < -53 || ord > 53) continue;           /* already bounded by parse_rule */
            if (!ord) { for (int d = d1; d >= 1 && d <= ml; d += 7) pick[d] = 1; }
            else if (ord > 0) { int d = d1 + (ord - 1) * 7; if (d >= 1 && d <= ml) pick[d] = 1; }
            else { int last = d1; while (last + 7 <= ml) last += 7; int d = last + (ord + 1) * 7; if (d >= 1 && d <= ml) pick[d] = 1; }
        }
    } else if (dflt_day >= 1 && dflt_day <= ml) pick[dflt_day] = 1;
    for (int d = 1; d <= ml; d++) if (pick[d]) out[n++] = first + d - 1;
    return n;
}

int ical_expand(const IcEvent *ev, int64_t from, int64_t to, int max, void (*fn)(void *u, int64_t start), void *u) {
    int64_t dur = ev->end - ev->start;
    int nout = 0;
    const char *zone = ev->allday ? NULL : (ev->tzid[0] ? ev->tzid : NULL);
#define EMIT(t) do { int64_t _t = (t); int _ex = 0; for (int _k = 0; _k < ev->nexdate; _k++) if (ev->exdate[_k] == _t || (ev->allday && (ev->exdate[_k] - _t) / 86400 == 0 && llabs(ev->exdate[_k] - _t) < 86400)) _ex = 1; \
        if (!_ex && _t < to && (_t + (dur > 0 ? dur : 1)) > from) { if (fn) fn(u, _t); nout++; } } while (0)
    Rule r;
    if (!ev->rrule[0] || ev->recurrence_id || !parse_rule(ev->rrule, &r)) { EMIT(ev->start); return nout; }
    int64_t wall0 = ev->start + ic_zone_offset(zone, ev->start);
    int64_t day0 = wall0 >= 0 ? wall0 / 86400 : (wall0 - 86399) / 86400;
    int64_t tod = wall0 - day0 * 86400;
    if (ev->allday) { day0 = ev->sdate; tod = 0; }
    int y0, m0, d0;
    ic_civil_from_days(day0, &y0, &m0, &d0);
    int seen = 0;
    int64_t cand[400];
    for (long period = 0; period < 50000 && nout < max; period++) {
        int nc = 0;
        if (r.freq == 'D') {
            int64_t d = day0 + period * r.interval;
            int y, m, dd;
            ic_civil_from_days(d, &y, &m, &dd);
            if ((!r.nbm || in_list(r.bymonth, r.nbm, m)) && (!r.nbyday || in_list(r.byday, r.nbyday, weekday_of(d))) &&
                (!r.nbmd || in_list(r.bymonthday, r.nbmd, dd))) cand[nc++] = d;
        } else if (r.freq == 'W') {
            int64_t wk = day0 - weekday_of(day0) + period * 7 * r.interval;
            for (int w = 0; w < 7; w++) {
                int64_t d = wk + w;
                if (r.nbyday ? in_list(r.byday, r.nbyday, w) : w == weekday_of(day0)) {
                    int y, m, dd;
                    ic_civil_from_days(d, &y, &m, &dd);
                    if (!r.nbm || in_list(r.bymonth, r.nbm, m)) cand[nc++] = d;
                }
            }
        } else if (r.freq == 'M') {
            long mi = (long)y0 * 12 + (m0 - 1) + period * r.interval;
            int y = (int)(mi / 12), m = (int)(mi % 12) + 1;
            if (!r.nbm || in_list(r.bymonth, r.nbm, m)) nc = month_days(&r, y, m, d0, cand);
        } else {
            int y = y0 + (int)(period * r.interval);
            if (y > 9999) break;
            if (r.nbm) { for (int k = 0; k < r.nbm && nc + 31 <= (int)(sizeof cand / sizeof cand[0]); k++) nc += month_days(&r, y, r.bymonth[k], d0, cand + nc); }
            else nc = month_days(&r, y, m0, d0, cand);
        }
        /* candidates are in ascending order per period (yearly months may not be: sort) */
        for (int a = 1; a < nc; a++) for (int b = a; b > 0 && cand[b] < cand[b - 1]; b--) { int64_t t = cand[b]; cand[b] = cand[b - 1]; cand[b - 1] = t; }
        int stop = 0;
        for (int k = 0; k < nc; k++) {
            if (cand[k] < day0) continue;
            int64_t start = ev->allday ? ic_wall_to_utc(cand[k] * 86400, NULL) : ic_wall_to_utc(cand[k] * 86400 + tod, zone);
            if (cand[k] == day0) start = ev->start;
            if (r.until && start > r.until) { stop = 1; break; }
            if (r.count && seen >= r.count) { stop = 1; break; }
            seen++;
            if (start >= to) { stop = 1; break; }
            EMIT(start);
            if (nout >= max) { stop = 1; break; }
        }
        if (stop) break;
        /* nothing can match anymore? (e.g. BYMONTHDAY=31 forever is fine; guard by the period cap) */
    }
#undef EMIT
    return nout;
}

/* ---- JSON ---- */

static void date_str(int days, char *out, size_t cap) {
    int y, m, d;
    ic_civil_from_days(days, &y, &m, &d);
    snprintf(out, cap, "%04d-%02d-%02d", y, m, d);
}

void ical_event_json(JBuf *b, const IcEvent *e, int64_t occ) {
    int64_t start = occ ? occ : e->start;
    int64_t end = start + (e->end - e->start);
    jb_raw(b, "{\"uid\":"); jb_str(b, e->uid);
    jb_raw(b, ",\"title\":"); jb_str(b, e->summary);
    if (e->location[0]) { jb_raw(b, ",\"loc\":"); jb_str(b, e->location); }
    if (e->description && e->description[0]) { jb_raw(b, ",\"notes\":"); jb_str(b, e->description); }
    if (e->url[0]) { jb_raw(b, ",\"url\":"); jb_str(b, e->url); }
    if (e->status[0]) { jb_raw(b, ",\"status\":"); jb_str(b, e->status); }
    if (e->categories[0]) { jb_raw(b, ",\"cats\":"); jb_str(b, e->categories); }
    jb_printf(b, ",\"start\":%lld,\"end\":%lld,\"allDay\":%s", (long long)start * 1000, (long long)end * 1000, e->allday ? "true" : "false");
    if (e->allday) {
        char ds[16], de[16];
        int sd = e->sdate, ed = e->edate;
        if (occ) { int64_t w = occ + ic_zone_offset(NULL, occ); int nd = (int)(w >= 0 ? w / 86400 : (w - 86399) / 86400); ed = nd + (ed - sd); sd = nd; }
        date_str(sd, ds, sizeof ds);
        date_str(ed, de, sizeof de);
        jb_raw(b, ",\"sdate\":"); jb_str(b, ds);
        jb_raw(b, ",\"edate\":"); jb_str(b, de);
    }
    if (e->tzid[0]) { jb_raw(b, ",\"tz\":"); jb_str(b, e->tzid); }
    if (e->rrule[0]) { jb_raw(b, ",\"rrule\":"); jb_str(b, e->rrule); }
    if (occ) jb_printf(b, ",\"occ\":%lld", (long long)occ * 1000);
    if (e->recurrence_id) jb_printf(b, ",\"rid\":%lld", (long long)e->recurrence_id * 1000);
    jb_raw(b, ",\"alarms\":[");
    for (int i = 0; i < e->nalarms; i++) jb_printf(b, "%s%d", i ? "," : "", e->alarms[i]);
    jb_raw(b, "]");
    if (e->organizer[0]) { jb_raw(b, ",\"org\":{\"email\":"); jb_str(b, e->organizer); jb_raw(b, ",\"name\":"); jb_str(b, e->organizer_name); jb_raw(b, "}"); }
    if (e->natt) {
        jb_raw(b, ",\"att\":[");
        for (int i = 0; i < e->natt; i++) {
            if (i) jb_raw(b, ",");
            jb_raw(b, "{\"email\":"); jb_str(b, e->att[i].email);
            jb_raw(b, ",\"name\":"); jb_str(b, e->att[i].name);
            jb_raw(b, ",\"ps\":"); jb_str(b, e->att[i].partstat);
            if (e->att[i].role[0]) { jb_raw(b, ",\"role\":"); jb_str(b, e->att[i].role); }
            jb_raw(b, "}");
        }
        jb_raw(b, "]");
    }
    jb_printf(b, ",\"seq\":%d", e->sequence);
    if (e->is_todo) {
        jb_raw(b, ",\"todo\":true");
        if (e->due) {
            jb_printf(b, ",\"due\":%lld,\"dueAllDay\":%s", (long long)e->due * 1000, e->due_allday ? "true" : "false");
            if (e->due_allday) { char ds[16]; int64_t w = e->due + ic_zone_offset(NULL, e->due); date_str((int)(w / 86400), ds, sizeof ds); jb_raw(b, ",\"dueDate\":"); jb_str(b, ds); }
        } else jb_raw(b, ",\"due\":null");
        jb_printf(b, ",\"prio\":%d,\"done\":%s,\"flag\":%s", e->priority, e->completed ? "true" : "false", e->flagged ? "true" : "false");
        if (e->completed > 1) jb_printf(b, ",\"doneAt\":%lld", (long long)e->completed * 1000);
        if (e->parent_uid[0]) { jb_raw(b, ",\"parent\":"); jb_str(b, e->parent_uid); }
    }
    jb_raw(b, "}");
}

/* ---- building ---- */

static void fmt_utc(int64_t t, char *out, size_t cap) {
    int64_t days = t >= 0 ? t / 86400 : (t - 86399) / 86400;
    int64_t s = t - days * 86400;
    int y, m, d;
    ic_civil_from_days(days, &y, &m, &d);
    snprintf(out, cap, "%04d%02d%02dT%02d%02d%02dZ", y, m, d, (int)(s / 3600), (int)(s / 60 % 60), (int)(s % 60));
}

static void fmt_wall(int64_t t, const char *zone, char *out, size_t cap) {
    int64_t w = t + ic_zone_offset(zone, t);
    int64_t days = w >= 0 ? w / 86400 : (w - 86399) / 86400;
    int64_t s = w - days * 86400;
    int y, m, d;
    ic_civil_from_days(days, &y, &m, &d);
    snprintf(out, cap, "%04d%02d%02dT%02d%02d%02d", y, m, d, (int)(s / 3600), (int)(s / 60 % 60), (int)(s % 60));
}

static void fmt_date(int64_t t, char *out, size_t cap) {
    int64_t w = t + ic_zone_offset(NULL, t);
    int y, m, d;
    ic_civil_from_days(w >= 0 ? w / 86400 : (w - 86399) / 86400, &y, &m, &d);
    snprintf(out, cap, "%04d%02d%02d", y, m, d);
}

static void kv(JBuf *b, const char *k, const char *v) {
    if (!v || !*v) return;
    char *e = ic_escape(v);
    if (!e) return;
    size_t n = strlen(k) + strlen(e) + 2;
    char *line = malloc(n);
    if (line) { snprintf(line, n, "%s:%s", k, e); ic_line(b, line); free(line); }
    free(e);
}

static int safe_rrule(const char *r) {
    for (; *r; r++) if (!isalnum((unsigned char)*r) && !strchr("=;,:+-", *r)) return 0;
    return 1;
}

static void safe_param(const char *in, char *out, size_t cap) {
    /* parameter values: no quotes, no control characters */
    size_t o = 0;
    for (; in && *in && o + 1 < cap; in++) if ((unsigned char)*in >= 0x20 && *in != '"') out[o++] = *in;
    out[o] = 0;
}

/* VTIMEZONE from the zone's TZif footer (POSIX rule "CET-1CEST,M3.5.0,M10.5.0/3"). 0 if done. */
static int vtimezone(JBuf *b, const char *zone) {
    ZoneCache *c = zone_load(zone);
    if (!c || c->n < 2) return -1;
    /* footer: "\n<rule>\n" at the end */
    size_t e = c->n - 1;
    if (c->buf[e] != '\n') return -1;
    size_t s = e;
    while (s > 0 && c->buf[s - 1] != '\n') s--;
    char rule[80];
    size_t rl = e - s;
    if (!rl || rl >= sizeof rule) return -1;
    memcpy(rule, c->buf + s, rl);
    rule[rl] = 0;
    /* std name, offset [dst name [offset], start, end] */
    const char *p = rule;
    char stdn[16], dstn[16] = "";
    int i = 0;
    if (*p == '<') { p++; while (*p && *p != '>' && i < 15) stdn[i++] = *p++; if (*p == '>') p++; }
    else while (isalpha((unsigned char)*p) && i < 15) stdn[i++] = *p++;
    stdn[i] = 0;
    int sign = 1, hh = 0, mm = 0;
    if (*p == '-') { sign = -1; p++; } else if (*p == '+') p++;
    if (!isdigit((unsigned char)*p)) return -1;
    hh = (int)strtol(p, (char **)&p, 10);
    if (*p == ':') { p++; mm = (int)strtol(p, (char **)&p, 10); }
    int stdoff = -sign * (hh * 3600 + mm * 60);             /* POSIX sign is inverted */
    int dstoff = stdoff + 3600;
    int has_dst = 0;
    int sm = 0, sw = 0, sd = 0, em = 0, ew = 0, ed = 0;
    if (*p) {
        i = 0;
        if (*p == '<') { p++; while (*p && *p != '>' && i < 15) dstn[i++] = *p++; if (*p == '>') p++; }
        else while (isalpha((unsigned char)*p) && i < 15) dstn[i++] = *p++;
        dstn[i] = 0;
        if (*p && *p != ',') {
            int sg = 1;
            if (*p == '-') { sg = -1; p++; } else if (*p == '+') p++;
            int h2 = (int)strtol(p, (char **)&p, 10), m2 = 0;
            if (*p == ':') { p++; m2 = (int)strtol(p, (char **)&p, 10); }
            dstoff = -sg * (h2 * 3600 + m2 * 60);
        }
        if (sscanf(p, ",M%d.%d.%d", &sm, &sw, &sd) == 3) {
            const char *q = strchr(p + 1, ',');
            if (q && sscanf(q, ",M%d.%d.%d", &em, &ew, &ed) == 3) has_dst = 1;
        }
        if (!has_dst) return -1;
    }
    char line[160], zq[80];
    safe_param(zone, zq, sizeof zq);
    jb_raw(b, "BEGIN:VTIMEZONE\r\n");
    snprintf(line, sizeof line, "TZID:%s", zq);
    ic_line(b, line);
    static const char *wd[] = { "SU", "MO", "TU", "WE", "TH", "FR", "SA" };
#define OFF(o) ((o) < 0 ? '-' : '+'), abs(o) / 3600, abs(o) / 60 % 60
    if (!has_dst) {
        jb_raw(b, "BEGIN:STANDARD\r\nDTSTART:19700101T000000\r\n");
        jb_printf(b, "TZOFFSETFROM:%c%02d%02d\r\nTZOFFSETTO:%c%02d%02d\r\n", OFF(stdoff), OFF(stdoff));
        if (stdn[0]) jb_printf(b, "TZNAME:%s\r\n", stdn);
        jb_raw(b, "END:STANDARD\r\n");
    } else {
        int sord = sw == 5 ? -1 : sw, eord = ew == 5 ? -1 : ew;
        jb_raw(b, "BEGIN:DAYLIGHT\r\nDTSTART:19700301T020000\r\n");
        jb_printf(b, "TZOFFSETFROM:%c%02d%02d\r\nTZOFFSETTO:%c%02d%02d\r\n", OFF(stdoff), OFF(dstoff));
        jb_printf(b, "RRULE:FREQ=YEARLY;BYMONTH=%d;BYDAY=%d%s\r\n", sm, sord, wd[sd % 7]);
        if (dstn[0]) jb_printf(b, "TZNAME:%s\r\n", dstn);
        jb_raw(b, "END:DAYLIGHT\r\nBEGIN:STANDARD\r\nDTSTART:19701001T020000\r\n");
        jb_printf(b, "TZOFFSETFROM:%c%02d%02d\r\nTZOFFSETTO:%c%02d%02d\r\n", OFF(dstoff), OFF(stdoff));
        jb_printf(b, "RRULE:FREQ=YEARLY;BYMONTH=%d;BYDAY=%d%s\r\n", em, eord, wd[ed % 7]);
        if (stdn[0]) jb_printf(b, "TZNAME:%s\r\n", stdn);
        jb_raw(b, "END:STANDARD\r\n");
    }
#undef OFF
    jb_raw(b, "END:VTIMEZONE\r\n");
    return 0;
}

static void time_line(JBuf *b, const char *name, int64_t t, int allday, const char *tz) {
    char v[40], line[200];
    if (allday) { fmt_date(t, v, sizeof v); snprintf(line, sizeof line, "%s;VALUE=DATE:%s", name, v); }
    else if (tz && *tz) { char zq[80]; safe_param(tz, zq, sizeof zq); fmt_wall(t, tz, v, sizeof v); snprintf(line, sizeof line, "%s;TZID=%s:%s", name, zq, v); }
    else { fmt_utc(t, v, sizeof v); snprintf(line, sizeof line, "%s:%s", name, v); }
    ic_line(b, line);
}

static void person_line(JBuf *b, const char *prop, const char *email, const char *name, const char *ps, const char *role, int rsvp) {
    char cn[200] = "", line[700];
    safe_param(name, cn, sizeof cn);
    char e[300];
    safe_param(email, e, sizeof e);
    size_t o = (size_t)snprintf(line, sizeof line, "%s", prop);
    if (cn[0]) o += (size_t)snprintf(line + o, sizeof line - o, ";CN=\"%s\"", cn);
    if (role && *role) o += (size_t)snprintf(line + o, sizeof line - o, ";ROLE=%s", role);
    if (ps && *ps) o += (size_t)snprintf(line + o, sizeof line - o, ";PARTSTAT=%s", ps);
    if (rsvp) o += (size_t)snprintf(line + o, sizeof line - o, ";RSVP=TRUE");
    if (o < sizeof line) snprintf(line + o, sizeof line - o, ":mailto:%s", e);
    ic_line(b, line);
}

char *ical_build(const IcBuild *x, size_t *olen) {
    JBuf b;
    jb_init(&b);
    jb_raw(&b, "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//Axia DE//Calendar 1.0//EN\r\nCALSCALE:GREGORIAN\r\n");
    if (x->method && *x->method) jb_printf(&b, "METHOD:%s\r\n", x->method);
    const char *tz = x->tzid && *x->tzid && tz_zone_valid(x->tzid) ? x->tzid : NULL;
    if (tz && !x->allday && (!x->is_todo || x->start)) { if (vtimezone(&b, tz)) tz = NULL; }
    jb_raw(&b, x->is_todo ? "BEGIN:VTODO\r\n" : "BEGIN:VEVENT\r\n");
    kv(&b, "UID", x->uid);
    char now[40];
    fmt_utc((int64_t)time(NULL), now, sizeof now);
    jb_printf(&b, "DTSTAMP:%s\r\nLAST-MODIFIED:%s\r\n", now, now);
    jb_printf(&b, "SEQUENCE:%d\r\n", x->sequence);
    if (!x->is_todo || x->start) time_line(&b, "DTSTART", x->start, x->allday, tz);
    if (!x->is_todo) {
        int64_t end = x->end > x->start ? x->end : x->start + (x->allday ? 86400 : 3600);
        time_line(&b, "DTEND", end, x->allday, tz);
    }
    kv(&b, "SUMMARY", x->summary);
    kv(&b, "LOCATION", x->location);
    kv(&b, "DESCRIPTION", x->description);
    if (x->url && *x->url) { char line[600]; char u[520]; safe_param(x->url, u, sizeof u); snprintf(line, sizeof line, "URL:%s", u); ic_line(&b, line); }
    /* a to-do's STATUS follows "completed" (below): one STATUS only (RFC 5545) */
    if (!x->is_todo && x->status && *x->status) { char st[32]; safe_param(x->status, st, sizeof st); jb_printf(&b, "STATUS:%s\r\n", st); }
    kv(&b, "CATEGORIES", x->categories);
    if (x->rrule && *x->rrule && safe_rrule(x->rrule)) { char line[600]; snprintf(line, sizeof line, "RRULE:%s", x->rrule); ic_line(&b, line); }
    for (int i = 0; i < x->nexdate; i++) time_line(&b, "EXDATE", x->exdate[i], x->allday, tz);
    if (x->organizer && *x->organizer) person_line(&b, "ORGANIZER", x->organizer, x->organizer_name, NULL, NULL, 0);
    for (int i = 0; i < x->natt; i++) person_line(&b, "ATTENDEE", x->att[i].email, x->att[i].name, x->att[i].partstat[0] ? x->att[i].partstat : "NEEDS-ACTION",
                                                  x->att[i].role[0] ? x->att[i].role : "REQ-PARTICIPANT", x->att[i].rsvp);
    if (x->is_todo) {
        if (x->due) time_line(&b, "DUE", x->due, x->due_allday, NULL);
        if (x->priority > 0 && x->priority <= 9) jb_printf(&b, "PRIORITY:%d\r\n", x->priority);
        if (x->completed) {
            char c[40];
            fmt_utc(x->completed > 1 ? x->completed : (int64_t)time(NULL), c, sizeof c);
            jb_printf(&b, "STATUS:COMPLETED\r\nCOMPLETED:%s\r\nPERCENT-COMPLETE:100\r\n", c);
        } else jb_raw(&b, "STATUS:NEEDS-ACTION\r\n");
        if (x->flagged) jb_raw(&b, "X-LORDOS-FLAGGED:1\r\n");
        kv(&b, "RELATED-TO", x->parent_uid);
    }
    for (int i = 0; i < x->nalarms && i < 8; i++) {
        int m = x->alarms[i];
        jb_raw(&b, "BEGIN:VALARM\r\nACTION:DISPLAY\r\n");
        kv(&b, "DESCRIPTION", x->summary && *x->summary ? x->summary : "Reminder");
        if (m >= 0) jb_printf(&b, "TRIGGER:-PT%dM\r\n", m);
        else jb_printf(&b, "TRIGGER:PT%dM\r\n", -m);
        jb_raw(&b, "END:VALARM\r\n");
    }
    jb_raw(&b, x->is_todo ? "END:VTODO\r\n" : "END:VEVENT\r\n");
    jb_raw(&b, "END:VCALENDAR\r\n");
    if (b.oom) { jb_free(&b); return NULL; }
    if (olen) *olen = b.n;
    return b.p;
}

char *ical_reply(const IcEvent *ev, const char *attendee, const char *name, const char *partstat, size_t *olen) {
    IcAttendee me;
    memset(&me, 0, sizeof me);
    snprintf(me.email, sizeof me.email, "%s", attendee);
    if (name) snprintf(me.name, sizeof me.name, "%s", name);
    snprintf(me.partstat, sizeof me.partstat, "%s", partstat);
    for (int i = 0; i < ev->natt; i++) if (!strcasecmp(ev->att[i].email, attendee)) { if (!name || !*name) snprintf(me.name, sizeof me.name, "%s", ev->att[i].name); snprintf(me.role, sizeof me.role, "%s", ev->att[i].role); }
    IcBuild b = { 0 };
    b.uid = ev->uid; b.summary = ev->summary; b.location = ev->location;
    b.start = ev->start; b.end = ev->end; b.allday = ev->allday;
    b.organizer = ev->organizer; b.organizer_name = ev->organizer_name;
    b.att = &me; b.natt = 1;
    b.sequence = ev->sequence;
    b.method = "REPLY";
    b.rrule = ev->rrule;
    return ical_build(&b, olen);
}
