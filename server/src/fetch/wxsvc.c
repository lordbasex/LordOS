/* wxsvc.c — the weather widget's forecasts in lord-server (see wxsvc.h). */
#include "wxsvc.h"
#include "fetchproto.h"
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
    char err[24];
} Zone;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static Zone Z[WXS_ZONES];
static int nz, busy, inited;
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

/* tzdata's coordinates of a zone (as display.c's sunrise does) */
static int coords(const char *zone, double *lat, double *lon) {
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

typedef struct { int fd; pid_t pid; } Helper;

static int helper_start(Helper *h) {
    struct stat st, me;
    if (stat(fetch_bin, &st) || !S_ISREG(st.st_mode) || stat("/proc/self/exe", &me) ||
        (st.st_uid != 0 && st.st_uid != me.st_uid) || (st.st_mode & 022)) return -1;   /* root's or ours, not writable by others */
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) return -1;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, sv[1], 3);
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    posix_spawnattr_setsigmask(&at, &none);
    posix_spawnattr_setsigdefault(&at, &all);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    char *argv[] = { "lord-fetch", NULL };
    char *envp[] = { "LANG=C.UTF-8", getenv("LORDOS_SESSIOND") ? "LORDOS_SESSIOND=1" : NULL, NULL };
    int e = posix_spawn(&h->pid, fetch_bin, &fa, &at, argv, envp);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    close(sv[1]);
    if (e) { close(sv[0]); return -1; }
    h->fd = sv[0];
    return 0;
}

static void helper_stop(Helper *h) {
    if (h->fd < 0) return;
    doc_send(h->fd, FQ_END, 0, "{}", 2, NULL, 0);
    close(h->fd);
    h->fd = -1;
    for (int i = 0; i < 50; i++) {
        if (waitpid(h->pid, NULL, WNOHANG) == h->pid) return;
        struct timespec ts = { 0, 20 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    kill(h->pid, SIGKILL);
    waitpid(h->pid, NULL, 0);
}

/* a socket to WX_HOST:443 made by the weather connector (its own scope and firewall rules) */
static int connect_host(void) {
    struct addrinfo hints = { 0 }, *res = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_ADDRCONFIG;
    if (getaddrinfo(WX_HOST, "443", &hints, &res)) return -2;
    int fd = -1;
    for (struct addrinfo *a = res; a && fd < 0; a = a->ai_next) {
        fd = netconn_socket(NC_WEATHER, a->ai_family, SOCK_STREAM, a->ai_addr, a->ai_addrlen);
        if (fd < 0) continue;
        struct pollfd p = { fd, POLLOUT, 0 };
        int err = 0;
        socklen_t el = sizeof err;
        if (poll(&p, 1, 15000) != 1 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) || err) { close(fd); fd = -1; }
    }
    freeaddrinfo(res);
    if (fd >= 0) fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
    return fd;
}

/* one forecast: 0 with *d, or -1 with err */
static int fetch_one(Helper *h, double lat, double lon, WxData *d, char *err, size_t errlen) {
    if (h->fd < 0 && helper_start(h)) { snprintf(err, errlen, "helper"); return -1; }
    int fd = connect_host();
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
        Zone *y = find(zone);          /* still the same slot: zones are never removed */
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
        if (nz >= WXS_ZONES || coords(zone, &lat, &lon)) { pthread_mutex_unlock(&mu); return -1; }
        z = &Z[nz++];
        memset(z, 0, sizeof *z);
        snprintf(z->zone, sizeof z->zone, "%s", zone);
        z->lat = lat;
        z->lon = lon;
    }
    long long now = now_s();
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
