/* batthist.c — the computer battery's history (see batthist.h). */
#include "batthist.h"

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SAMPLE_EVERY 600
#define APPS_EVERY 600
#define MAX_GAP 180                /* a longer gap between ticks (suspend) counts no screen time */

int bh_scope_ok(const char *s) {
    size_t l = s ? strlen(s) : 0;
    if (!l || l >= 80 || s[0] == '.') return 0;
    for (const char *p = s; *p; p++) if (!isalnum((unsigned char)*p) && *p != '.' && *p != '_' && *p != '-') return 0;
    return !strcmp(s, "server") || (l > 6 && !strcmp(s + l - 6, ".scope"));
}

static BhDay *day_of(BattHist *h, int ymd) {
    for (int i = 0; i < h->nd; i++) if (h->d[i].ymd == ymd) return &h->d[i];
    if (h->nd == BH_DAYS) { memmove(h->d, h->d + 1, sizeof h->d[0] * (BH_DAYS - 1)); h->nd--; }
    BhDay *d = &h->d[h->nd++];
    memset(d, 0, sizeof *d);
    d->ymd = ymd;
    return d;
}

static void add_app(BhDay *d, const char *scope, long long usec) {
    if (usec <= 0) return;
    for (int i = 0; i < d->napps; i++) if (!strcmp(d->apps[i].scope, scope)) { d->apps[i].usec += usec; return; }
    if (d->napps == BH_APPS) {                    /* full: the smallest makes room if this one is bigger */
        int m = 0;
        for (int i = 1; i < d->napps; i++) if (d->apps[i].usec < d->apps[m].usec) m = i;
        if (d->apps[m].usec >= usec) return;
        snprintf(d->apps[m].scope, sizeof d->apps[m].scope, "%s", scope);
        d->apps[m].usec = usec;
        return;
    }
    snprintf(d->apps[d->napps].scope, sizeof d->apps[0].scope, "%s", scope);
    d->apps[d->napps++].usec = usec;
}

/* each scope's CPU at the previous app reading, to take the difference */
typedef struct { char scope[80]; long long usec; } Prev;
static Prev prev_cpu[64];
static int nprev_cpu = -1;              /* -1: no reading yet */

void bh_tick(BattHist *h, long long now, int today, const PowerSupply *bat, int on_ac, int seen,
             const BhScope *scopes, int nscopes) {
    if (!bat || bat->percent < 0) return;
    if (!h->first_seen) h->first_seen = now;
    int charging = !strcmp(bat->status, "Charging");
    int discharging = !strcmp(bat->status, "Discharging");
    BhDay *d = day_of(h, today);
    if (h->have_prev) {
        long long dt = now - h->prev_t;
        if (discharging && dt > 0 && dt <= MAX_GAP) { if (seen) d->on_s += dt; else d->off_s += dt; }
        if (discharging && bat->percent < h->prev_p) {
            long long tenths = (long long)(h->prev_p - bat->percent) * 10;
            d->drain += tenths;
            h->drain_total += tenths;
        }
        if (h->prev_charging && !charging) { h->last_charge_t = now; h->last_charge_p = bat->percent; }
    }
    /* the level every 10 minutes */
    if (!h->ns || now - h->s[h->ns - 1].t >= SAMPLE_EVERY) {
        if (h->ns == BH_SAMPLES) { memmove(h->s, h->s + 1, sizeof h->s[0] * (BH_SAMPLES - 1)); h->ns--; }
        h->s[h->ns++] = (BhSample){ now, bat->percent, (charging ? 1 : 0) | (on_ac ? 2 : 0) };
    }
    /* each app's CPU while on battery */
    if (scopes && nscopes > 0 && (!h->prev_apps_t || now - h->prev_apps_t >= APPS_EVERY)) {
        if (nprev_cpu >= 0 && discharging && now - h->prev_apps_t <= APPS_EVERY * 3) {
            for (int i = 0; i < nscopes; i++) {
                if (!bh_scope_ok(scopes[i].scope)) continue;
                for (int j = 0; j < nprev_cpu; j++)
                    if (!strcmp(prev_cpu[j].scope, scopes[i].scope) && scopes[i].usec > prev_cpu[j].usec) add_app(d, scopes[i].scope, scopes[i].usec - prev_cpu[j].usec);
            }
        }
        nprev_cpu = 0;
        for (int i = 0; i < nscopes && nprev_cpu < 64; i++) {
            snprintf(prev_cpu[nprev_cpu].scope, sizeof prev_cpu[0].scope, "%s", scopes[i].scope);
            prev_cpu[nprev_cpu++].usec = scopes[i].usec;
        }
        h->prev_apps_t = now;
    }
    h->prev_t = now;
    h->prev_p = bat->percent;
    h->prev_charging = charging;
    h->have_prev = 1;
}

size_t bh_save(const BattHist *h, char *out, size_t n) {
    size_t o = 0;
#define PUT(...) do { if (o < n) { int r_ = snprintf(out + o, n - o, __VA_ARGS__); if (r_ > 0) o += (size_t)r_; } } while (0)
    PUT("v 1\nfirst %lld\ndrain %lld\nlast %lld %d\n", h->first_seen, h->drain_total, h->last_charge_t, h->last_charge_p);
    for (int i = 0; i < h->ns; i++) PUT("s %lld %d %d\n", h->s[i].t, h->s[i].p, h->s[i].flags);
    for (int i = 0; i < h->nd; i++) {
        PUT("d %d %lld %lld %lld\n", h->d[i].ymd, h->d[i].drain, h->d[i].on_s, h->d[i].off_s);
        for (int k = 0; k < h->d[i].napps; k++) PUT("a %d %s %lld\n", h->d[i].ymd, h->d[i].apps[k].scope, h->d[i].apps[k].usec);
    }
#undef PUT
    return o < n ? o : n;
}

void bh_load(BattHist *h, const char *text) {
    memset(h, 0, sizeof *h);
    if (!text) return;
    const char *p = text;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        char line[160];
        if (l < sizeof line) {
            memcpy(line, p, l);
            line[l] = 0;
            long long a, b, c;
            int x, y;
            char sc[96], extra;
            if (sscanf(line, "first %lld %c", &a, &extra) == 1 && a >= 0) h->first_seen = a;
            else if (sscanf(line, "drain %lld %c", &a, &extra) == 1 && a >= 0) h->drain_total = a;
            else if (sscanf(line, "last %lld %d %c", &a, &x, &extra) == 2 && a >= 0 && x >= 0 && x <= 100) { h->last_charge_t = a; h->last_charge_p = x; }
            else if (sscanf(line, "s %lld %d %d %c", &a, &x, &y, &extra) == 3 && a > 0 && x >= 0 && x <= 100 && y >= 0 && y <= 3 && h->ns < BH_SAMPLES)
                h->s[h->ns++] = (BhSample){ a, x, y };
            else if (sscanf(line, "d %d %lld %lld %lld %c", &x, &a, &b, &c, &extra) == 4 && x > 19000101 && x < 30000101 && a >= 0 && b >= 0 && c >= 0) {
                BhDay *d = day_of(h, x);
                d->drain = a; d->on_s = b; d->off_s = c;
            } else if (sscanf(line, "a %d %95s %lld %c", &x, sc, &a, &extra) == 3 && a > 0 && bh_scope_ok(sc)) {
                for (int i = 0; i < h->nd; i++) if (h->d[i].ymd == x) add_app(&h->d[i], sc, a);
            }
        }
        if (!e) break;
        p = e + 1;
    }
}

size_t bh_json(const BattHist *h, long long now, char *out, size_t n) {
    size_t o = 0;
#define PUT(...) do { if (o < n) { int r_ = snprintf(out + o, n - o, __VA_ARGS__); if (r_ > 0) o += (size_t)r_; } } while (0)
    PUT("{\"firstSeen\":%lld,\"cycles\":%.1f,\"lastCharge\":{\"t\":%lld,\"p\":%d},\"samples\":[", h->first_seen,
        (double)h->drain_total / 1000.0, h->last_charge_t, h->last_charge_p);
    int first = 1;
    for (int i = 0; i < h->ns; i++) {
        if (now - h->s[i].t > 26 * 3600) continue;
        PUT("%s[%lld,%d,%d]", first ? "" : ",", h->s[i].t, h->s[i].p, h->s[i].flags);
        first = 0;
    }
    PUT("],\"days\":[");
    for (int i = 0; i < h->nd; i++) {
        const BhDay *d = &h->d[i];
        PUT("%s{\"ymd\":%d,\"drain\":%lld,\"on\":%lld,\"off\":%lld,\"apps\":{", i ? "," : "", d->ymd, d->drain, d->on_s, d->off_s);
        for (int k = 0; k < d->napps; k++) PUT("%s\"%s\":%lld", k ? "," : "", d->apps[k].scope, d->apps[k].usec);
        PUT("}}");
    }
    PUT("]}");
#undef PUT
    if (o >= n) { if (n > 2) snprintf(out, n, "{}"); return n > 2 ? 2 : 0; }
    return o;
}

/* ---- lord-server's side ---- */

static BattHist hist;
static int loaded;
static long long saved_at;
static char session_cg[PATH_MAX];       /* /sys/fs/cgroup/…/lordos-session-<uid>, "" if unknown */

static void path_of(char *out, size_t n) {
    const struct passwd *pw = getpwuid(getuid());
    const char *home = pw && pw->pw_dir ? pw->pw_dir : getenv("HOME");
    snprintf(out, n, "%s/.local/state/lordos/battery-history", home ? home : "/nonexistent");
}

static void load_once(void) {
    if (loaded) return;
    loaded = 1;
    char p[PATH_MAX];
    path_of(p, sizeof p);
    int fd = open(p, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd >= 0) {
        char *buf = malloc(512 * 1024);
        if (buf) {
            ssize_t r = read(fd, buf, 512 * 1024 - 1);
            buf[r > 0 ? r : 0] = 0;
            bh_load(&hist, buf);
            free(buf);
        }
        close(fd);
    }
    /* lord-server's own cgroup leaf ".../lordos-session-<uid>/server": its parent holds every scope of the session */
    char cg[512] = "";
    int cf = open("/proc/self/cgroup", O_RDONLY | O_CLOEXEC);
    if (cf >= 0) {
        ssize_t r = read(cf, cg, sizeof cg - 1);
        close(cf);
        cg[r > 0 ? r : 0] = 0;
        char *line = strstr(cg, "0::");
        char *slash = line ? strrchr(line, '/') : NULL;
        char *nl = line ? strchr(line, '\n') : NULL;
        if (nl) *nl = 0;
        if (line && slash && !strcmp(slash, "/server") && strstr(line, "/lordos-session-")) {
            *slash = 0;
            snprintf(session_cg, sizeof session_cg, "/sys/fs/cgroup%s", line + 3);
        }
    }
}

static void save(void) {
    char p[PATH_MAX], tmp[PATH_MAX + 8];
    path_of(p, sizeof p);
    char *dir = strdup(p);
    if (!dir) return;
    char *s = strrchr(dir, '/');
    if (s) { *s = 0; mkdir(dir, 0700); }
    free(dir);
    char *buf = malloc(400 * 1024);
    if (!buf) return;
    size_t n = bh_save(&hist, buf, 400 * 1024);
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd >= 0) {
        int ok = write(fd, buf, n) == (ssize_t)n;
        close(fd);
        if (ok) rename(tmp, p); else unlink(tmp);
    }
    free(buf);
}

/* the scopes in session_cg + sub ("" or "/lordos-machines.slice", the machines' slice of docs/VIRTUALIZATION.md 3.7) */
static int read_scopes_in(const char *sub, BhScope *out, int n, int max) {
    char base[PATH_MAX];
    if ((size_t)snprintf(base, sizeof base, "%s%s", session_cg, sub) >= sizeof base) return n;
    DIR *d = opendir(base);
    if (!d) return n;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        if (!bh_scope_ok(e->d_name)) continue;
        if (sub[0] && strncmp(e->d_name, "app-lordos-lordos-vm-", 21)) continue;
        char p[PATH_MAX + 96], buf[256];
        size_t nl = strlen(e->d_name);                          /* bh_scope_ok: under 80 */
        if ((size_t)snprintf(p, sizeof p, "%s/", base) + nl + 10 >= sizeof p) continue;
        strcat(p, e->d_name);
        strcat(p, "/cpu.stat");
        int fd = open(p, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) continue;
        ssize_t r = read(fd, buf, sizeof buf - 1);
        close(fd);
        buf[r > 0 ? r : 0] = 0;
        long long us;
        if (sscanf(buf, "usage_usec %lld", &us) != 1 || us < 0) continue;
        memcpy(out[n].scope, e->d_name, nl + 1);
        out[n++].usec = us;
    }
    closedir(d);
    return n;
}

static int read_scopes(BhScope *out, int max) {
    if (!session_cg[0]) return 0;
    int n = read_scopes_in("", out, 0, max);
    return read_scopes_in("/lordos-machines.slice", out, n, max);
}

void batthist_tick(const PowerState *ps, int seen) {
    if (!ps || !ps->has_battery) return;
    load_once();
    const PowerSupply *bat = NULL;
    for (int i = 0; i < ps->n; i++) if (ps->s[i].battery && !ps->s[i].device_scope && ps->s[i].present) { bat = &ps->s[i]; break; }
    if (!bat) return;
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    int today = (lt.tm_year + 1900) * 10000 + (lt.tm_mon + 1) * 100 + lt.tm_mday;
    static BhScope sc[64];
    int nsc = (!hist.prev_apps_t || now - hist.prev_apps_t >= APPS_EVERY) ? read_scopes(sc, 64) : 0;
    bh_tick(&hist, (long long)now, today, bat, ps->on_ac, seen, sc, nsc);
    if (now - saved_at >= SAMPLE_EVERY) { save(); saved_at = now; }
}

size_t batthist_json(char *out, size_t n) {
    load_once();
    return bh_json(&hist, (long long)time(NULL), out, n);
}
