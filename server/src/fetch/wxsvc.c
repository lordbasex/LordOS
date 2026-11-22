/* wxsvc.c — the weather widget's forecasts in lord-server (see wxsvc.h). */
#include "geocities.h"
#include "wxsvc.h"
#include "fetchproto.h"
#include "fetchspawn.h"
#include "../doc/wire.h"
#include "../hw/night.h"
#include "../sec/netconn.h"

#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    char zone[64];
    double lat, lon;
    int want;                   /* asked, not yet fetched */
    int have;                   /* d is a forecast */
    WxData d;
    long long at;               /* when d came */
    long long tried;            /* the last attempt */
    unsigned long long asked;   /* when wxs_request last asked (a count): the least recent gives its slot to a new zone */
    char err[24];
} Zone;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static Zone Z[WXS_ZONES];
static int nz, busy, inited;
static unsigned long long asks;   /* wxs_request's count, for Zone.asked */
static char fetch_bin[PATH_MAX];

static long long now_s(void) { return (long long)time(NULL); }

int wxs_init(const char *bindir) {
    if (inited) return 0;
    if (!bindir || snprintf(fetch_bin, sizeof fetch_bin, "%s/lord-fetch", bindir) >= (int)sizeof fetch_bin) return -1;
    inited = 1;
    return 0;
}

int wxs_zone_ok(const char *z) {
    size_t n = z ? strlen(z) : 0;
    if (!n || n > 63 || z[0] == '/' || strstr(z, "..")) return 0;
    for (const char *p = z; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' || *p == '+' || *p == '/')) return 0;
    return 1;
}

static char cities_path[512];
void wxs_set_cities(const char *path) { snprintf(cities_path, sizeof cities_path, "%s", path ? path : ""); }

/* tzdata's coordinates of a zone (as display.c's sunrise does), or a picked city's from the offline list */
static int coords(const char *zone, double *lat, double *lon) {
    if (!strncmp(zone, "geonames/", 9)) {
        const char *d = zone + 9;
        size_t l = strlen(d);
        if (!l || l > 10 || strspn(d, "0123456789") != l || !cities_path[0]) return -1;
        return geo_lookup(cities_path, strtol(d, NULL, 10), lat, lon, NULL, 0);
    }
    static char *tab;
    if (!tab) {
        const char *paths[] = { "/usr/share/zoneinfo/zone1970.tab", "/usr/share/zoneinfo/zone.tab" };
        for (int i = 0; i < 2 && !tab; i++) {
            FILE *f = fopen(paths[i], "re");
            if (!f) continue;
            tab = calloc(1, 65536);
            if (tab && fread(tab, 1, 65535, f) == 0) { free(tab); tab = NULL; }
            fclose(f);
        }
        if (!tab) return -1;
    }
    return night_zone_coords(tab, zone, lat, lon);
}

static Zone *find(const char *zone) {
    for (int i = 0; i < nz; i++) if (!strcmp(Z[i].zone, zone)) return &Z[i];
    return NULL;
}

/* ---- the worker: one lord-fetch for the zones waiting, then it exits ---- */

typedef FetchHelper Helper;
#define helper_start(h) fetch_helper_start(fetch_bin, (h))
#define helper_stop(h) fetch_helper_stop(h)

/* one forecast: 0 with *d, or -1 with err */
static int fetch_one(Helper *h, double lat, double lon, WxData *d, char *err, size_t errlen) {
    if (h->fd < 0 && helper_start(h)) { snprintf(err, errlen, "helper"); return -1; }
    int fd = fetch_connect(NC_WEATHER, WX_HOST);
    if (fd < 0) { snprintf(err, errlen, fd == -2 ? "dns" : "connect"); return -1; }
    char req[96];
    snprintf(req, sizeof req, "{\"lat\":%.4f,\"lon\":%.4f}", lat, lon);
    int sent = doc_send(h->fd, FQ_WEATHER, 1, req, strlen(req), &fd, 1) == 0;
    close(fd);
    struct pollfd p = { h->fd, POLLIN, 0 };
    DocMsg m;
    if (!sent || poll(&p, 1, 60000) != 1 || doc_recv(h->fd, &m)) { snprintf(err, errlen, "helper"); helper_stop(h); return -1; }
    int rc = -1;
    if (m.h.op == FR_WEATHER && m.json && !m.nfds && !wx_from_compact(m.json, m.len, d)) rc = 0;
    else snprintf(err, errlen, m.h.op == FR_ERR ? "fetch" : "helper");
    doc_msg_free(&m);
    return rc;
}

static void *worker(void *arg) {
    (void)arg;
    Helper h = { -1, 0 };
    for (;;) {
        pthread_mutex_lock(&mu);
        Zone *z = NULL;
        for (int i = 0; i < nz && !z; i++) if (Z[i].want) z = &Z[i];
        if (!z) { busy = 0; pthread_mutex_unlock(&mu); break; }
        z->want = 0;
        z->tried = now_s();
        char zone[64];
        snprintf(zone, sizeof zone, "%s", z->zone);
        double lat = z->lat, lon = z->lon;
        pthread_mutex_unlock(&mu);
        WxData d;
        char err[24] = "";
        int ok = !fetch_one(&h, lat, lon, &d, err, sizeof err);
        pthread_mutex_lock(&mu);
        Zone *y = find(zone);          /* by name: its slot may have gone to another zone meanwhile */
        if (y) {
            if (ok) { y->d = d; y->have = 1; y->at = now_s(); y->err[0] = 0; }
            else snprintf(y->err, sizeof y->err, "%s", err);
        }
        pthread_mutex_unlock(&mu);
    }
    helper_stop(&h);
    return NULL;
}

int wxs_request(const char *zone) {
    if (!inited || !wxs_zone_ok(zone)) return -1;
    pthread_mutex_lock(&mu);
    Zone *z = find(zone);
    if (!z) {
        double lat, lon;
        if (coords(zone, &lat, &lon)) { pthread_mutex_unlock(&mu); return -1; }
        /* full: the zone asked least recently that isn't waiting for its fetch gives up its slot (each weather widget
         * has its own city since 0.8.9.1, #193: one person trying cities must not fill the table for good) */
        if (nz < WXS_ZONES) z = &Z[nz++];
        else for (int i = 0; i < nz; i++) if (!Z[i].want && (!z || Z[i].asked < z->asked)) z = &Z[i];
        if (!z) { pthread_mutex_unlock(&mu); return -1; }
        memset(z, 0, sizeof *z);
        snprintf(z->zone, sizeof z->zone, "%s", zone);
        z->lat = lat;
        z->lon = lon;
    }
    long long now = now_s();
    z->asked = ++asks;
    int fresh = z->have && now - z->at < WXS_FRESH, waiting = z->tried && now - z->tried < (z->err[0] ? WXS_RETRY : 60);
    if (!fresh && !waiting && !z->want) {
        z->want = 1;
        if (!busy) {
            busy = 1;
            pthread_t th;
            pthread_attr_t a;
            pthread_attr_init(&a);
            pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
            if (pthread_create(&th, &a, worker, NULL)) { busy = 0; z->want = 0; }
            pthread_attr_destroy(&a);
        }
    }
    pthread_mutex_unlock(&mu);
    return 0;
}

int wxs_get(const char *zone, WxData *d, long long *at, char *err, size_t errlen) {
    if (!wxs_zone_ok(zone)) return -1;
    pthread_mutex_lock(&mu);
    Zone *z = find(zone);
    int rc = !z ? -1 : z->have ? 1 : 0;
    if (z && z->have) { *d = z->d; *at = z->at; }
    if (z && err && errlen) snprintf(err, errlen, "%s", z->err);
    pthread_mutex_unlock(&mu);
    return rc;
}
