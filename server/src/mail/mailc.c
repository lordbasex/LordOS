/* mailc.c — see mailc.h. */
#include "mailc.h"
#include "../sec/netconn.h"
#include "proto.h"
#include "../doc/proto.h"
#include "../doc/wire.h"
#include "../sec/json.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define MAX_IMG    256
#define IMG_BUDGET ((size_t)96 << 20)
#define MAX_ALLOW  128
#define MAX_GRANT  64

typedef struct { char host[256]; int port; int suffix; } Allow;
typedef struct { char host[256]; int port; time_t until; } Grant;
typedef struct { int handle, w, h, owner; uint32_t *px; uint64_t seq; } Img;

/* Isolated system apps (Mail, Calendar, Contacts, Reminders as `lord-server --app <id>`): each
 * app process is an "owner" with its own event queue. Replies to its requests go only there; the
 * events lord-maild broadcasts go to the session (owner 0) and to every owner whose mask allows
 * them. brokermail.c reads these queues and forwards them over the broker. */
typedef struct { int used, mask; JBuf ev; } Owner;
typedef struct { int id, owner; } Route;
#define MAX_ROUTE 512

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t send_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t img_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_t thread;
static int running, sock = -1, wakefd = -1;
static pid_t child;
static int next_id = 1, pending;
static JBuf events;
static char helper[PATH_MAX], cfg_path[PATH_MAX], cache_path[PATH_MAX];
static Allow allow[MAX_ALLOW];
static int nallow;
static Grant grants[MAX_GRANT];
static Img imgs[MAX_IMG];
static size_t img_bytes;
static uint64_t img_seq;
static int next_handle = 1;
static double last_crash;
static Owner owners[MAIL_OWNERS + 1];          /* index 1..MAIL_OWNERS; 0 is the session (events) */
static Route routes[MAX_ROUTE];
static int owner_wake = -1;

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

static void paths(void) {
    if (cfg_path[0]) return;
    const char *h = getenv("HOME");
    if (!h) h = "/nonexistent";
    snprintf(cfg_path, sizeof cfg_path, "%s/.config/lordos/mail/accounts.json", h);
    snprintf(cache_path, sizeof cache_path, "%s/.local/share/lordos/mail", h);
}

const char *mail_config_path(void) { paths(); return cfg_path; }
const char *mail_cache_path(void) { paths(); return cache_path; }

int mail_available(void) {
    if (!helper[0]) {
        char self[PATH_MAX];
        ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
        if (n <= 0) return -1;
        self[n] = 0;
        char *slash = strrchr(self, '/');
        if (!slash) return -1;
        *slash = 0;
        if (strlen(self) + 12 > sizeof helper) return -1;
        snprintf(helper, sizeof helper, "%s/lord-maild", self);
    }
    struct stat st, me;
    if (stat(helper, &st) || !S_ISREG(st.st_mode) || !(st.st_mode & S_IXUSR)) return -1;
    /* owned by root or by lord-server's owner, and not writable by anyone else */
    if (stat("/proc/self/exe", &me) || (st.st_uid != 0 && st.st_uid != me.st_uid) || (st.st_mode & 022)) return -1;
    return 0;
}

/* ---- allowed endpoints ---- */

static int host_ok(const char *h) {
    size_t n = strlen(h);
    if (!n || n > 253) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)h[i];
        if (!(isalnum(c) || c == '-' || c == '.' || c == ':' || c == '[' || c == ']')) return 0;
    }
    return 1;
}

/* "https://host:port/path" → host, port */
static int url_host(const char *url, char *host, size_t cap, int *port) {
    host[0] = 0;
    *port = 0;
    if (!url) return 0;
    int https = !strncasecmp(url, "https://", 8), http = !strncasecmp(url, "http://", 7);
    if (!https && !http) return 0;
    const char *p = url + (https ? 8 : 7);
    const char *e = p + strcspn(p, "/?#");
    const char *at = memchr(p, '@', (size_t)(e - p));
    if (at) p = at + 1;
    const char *colon = NULL;
    if (*p == '[') { const char *rb = memchr(p, ']', (size_t)(e - p)); if (!rb) return 0; colon = rb + 1 < e && rb[1] == ':' ? rb + 1 : NULL; }
    else colon = memchr(p, ':', (size_t)(e - p));
    size_t hl = (size_t)((colon ? colon : e) - p);
    if (!hl || hl >= cap) return 0;
    for (size_t i = 0; i < hl; i++) host[i] = (char)tolower((unsigned char)p[i]);
    host[hl] = 0;
    *port = colon ? atoi(colon + 1) : (https ? 443 : 80);
    if (*port <= 0 || *port > 65535) return 0;
    return host_ok(host);
}

static void add_allow(Allow *list, int *n, const char *host, int port, int suffix) {
    if (*n >= MAX_ALLOW || !host || !host_ok(host) || port <= 0 || port > 65535) return;
    Allow *a = &list[(*n)++];
    snprintf(a->host, sizeof a->host, "%s", host);
    for (char *c = a->host; *c; c++) *c = (char)tolower((unsigned char)*c);
    a->port = port;
    a->suffix = suffix;
}

static void allow_server(Allow *list, int *n, const JVal *srv) {
    if (!srv || srv->type != J_OBJ) return;
    const char *h = json_str(srv, "host");
    int port = (int)json_num(srv, "port", 0);
    if (h) add_allow(list, n, h, port, 0);
}

static void allow_url(Allow *list, int *n, const char *url) {
    char host[256];
    int port;
    if (!url_host(url, host, sizeof host, &port)) return;
    add_allow(list, n, host, port, 0);
    /* CalDAV/CardDAV discovery redirects inside the same provider (to a numbered host such as pNN-caldav.<domain>):
     * allow other hosts of the same domain, HTTPS only */
    const char *dot1 = strrchr(host, '.');
    if (!dot1) return;
    const char *dot2 = dot1;
    while (dot2 > host && dot2[-1] != '.') dot2--;
    if (dot2 > host && strchr(host, '.') != dot1) add_allow(list, n, dot2, 443, 1);
}

static int build_allow(const char *json, size_t len, Allow *list) {
    int n = 0;
    if (!json || !len) return 0;
    JDoc d;
    if (json_parse(json, len, &d, NULL, 0)) return 0;
    const JVal *acc = json_get(d.root, "accounts");
    for (const JVal *a = acc && acc->type == J_ARR ? acc->kid : NULL; a; a = a->next) {
        allow_server(list, &n, json_get(a, "imap"));
        allow_server(list, &n, json_get(a, "smtp"));
        allow_url(list, &n, json_str(a, "url"));          /* CalDAV/CardDAV accounts */
        allow_url(list, &n, json_str(a, "caldav"));
        allow_url(list, &n, json_str(a, "carddav"));
    }
    json_free(&d);
    return n;
}

static int match(const Allow *list, int n, const char *host, int port) {
    for (int i = 0; i < n; i++) {
        if (list[i].port != port) continue;
        if (!strcasecmp(list[i].host, host)) return 1;
        if (list[i].suffix) {
            size_t hl = strlen(host), sl = strlen(list[i].host);
            if (hl > sl && host[hl - sl - 1] == '.' && !strcasecmp(host + hl - sl, list[i].host)) return 1;
        }
    }
    return 0;
}

int mail_endpoint_allowed(const char *config_json, size_t len, const char *host, int port) {
    Allow tmp[MAX_ALLOW];
    int n = build_allow(config_json, len, tmp);
    return match(tmp, n, host, port);
}

static char *read_config(size_t *len) {
    paths();
    *len = 0;
    int fd = open(cfg_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size > MAIL_CONFIG_MAX) { close(fd); return NULL; }
    char *b = malloc((size_t)st.st_size + 1);
    if (!b) { close(fd); return NULL; }
    ssize_t r = read(fd, b, (size_t)st.st_size);
    close(fd);
    if (r < 0) { free(b); return NULL; }
    b[r] = 0;
    JDoc d;
    if (json_parse(b, (size_t)r, &d, NULL, 0)) { free(b); return NULL; }
    json_free(&d);
    *len = (size_t)r;
    return b;
}

void mail_allow_hosts(const char *const *urls, int n, int seconds) {
    time_t until = time(NULL) + (seconds > 0 && seconds < 3600 ? seconds : 600);
    pthread_mutex_lock(&mu);
    for (int i = 0; i < n; i++) {
        char host[256];
        int port;
        if (!url_host(urls[i], host, sizeof host, &port)) continue;
        if (port != 80 && port != 443) continue;          /* remote images: plain web ports only */
        int slot = -1;
        for (int k = 0; k < MAX_GRANT; k++) {
            if (grants[k].host[0] && !strcmp(grants[k].host, host) && grants[k].port == port) { slot = k; break; }
            if (slot < 0 && (!grants[k].host[0] || grants[k].until < time(NULL))) slot = k;
        }
        if (slot < 0) slot = 0;
        snprintf(grants[slot].host, sizeof grants[slot].host, "%s", host);
        grants[slot].port = port;
        grants[slot].until = until;
    }
    pthread_mutex_unlock(&mu);
}

static int allowed_now(const char *host, int port) {
    int ok = 0;
    pthread_mutex_lock(&mu);
    if (match(allow, nallow, host, port)) ok = 1;
    time_t now = time(NULL);
    for (int k = 0; !ok && k < MAX_GRANT; k++)
        if (grants[k].host[0] && grants[k].until >= now && grants[k].port == port && !strcasecmp(grants[k].host, host)) ok = 2;
    pthread_mutex_unlock(&mu);
    return ok;
}

/* ---- events ---- */

static void ev_append(JBuf *dst, int id, const char *type, const char *data, size_t dlen) {
    if (dst->n) jb_raw(dst, ",");
    jb_printf(dst, "{\"id\":%d,\"type\":\"%s\"", id, type);
    if (data && dlen) {
        jb_raw(dst, ",\"data\":");
        /* the helper's JSON is re-validated before JS sees it */
        JDoc d;
        if (!json_parse(data, dlen, &d, NULL, 0)) { json_free(&d); char *tmp = malloc(dlen + 1); if (tmp) { memcpy(tmp, data, dlen); tmp[dlen] = 0; jb_raw(dst, tmp); free(tmp); } else jb_raw(dst, "null"); }
        else jb_raw(dst, "{\"error\":\"bad-json\"}");
    }
    jb_raw(dst, "}");
}

static void owner_signal(void) {
    if (owner_wake < 0) return;
    uint64_t one = 1;
    if (write(owner_wake, &one, sizeof one) < 0) { /* already signalled */ }
}

/* Which owner sent request `id` (0 = the session); the route is consumed. Called with mu held. */
static int route_take(int id) {
    if (id <= 0) return 0;
    for (int i = 0; i < MAX_ROUTE; i++) if (routes[i].id == id) { int o = routes[i].owner; routes[i].id = 0; return o; }
    return 0;
}

/* Events that an owner with MAIL_EV_PIM (Calendar, Contacts, Reminders) may see: no mail
 * metadata ("new" carries senders and subjects), only that the PIM data changed. */
static int pim_event(const char *data, size_t dlen) {
    JDoc d;
    int ok = 0;
    if (data && dlen && !json_parse(data, dlen, &d, NULL, 0)) {
        const char *ev = json_str(d.root, "ev");
        ok = ev && (!strcmp(ev, "pim-changed") || !strcmp(ev, "ready"));
        json_free(&d);
    }
    return ok;
}

/* owner >= 0: a reply (done/err) for that owner; -1: a broadcast (evt, crash); -2: a reply for
 * an app process that is gone (dropped). */
static void ev_push_to(int owner, int id, const char *type, const char *data, size_t dlen) {
    pthread_mutex_lock(&mu);
    int sig = 0;
    if (owner == -2) { /* dropped */ }
    else if (owner > 0) {
        if (owner <= MAIL_OWNERS && owners[owner].used) { ev_append(&owners[owner].ev, id, type, data, dlen); sig = 1; }
    } else {
        ev_append(&events, id, type, data, dlen);
        if (owner < 0) {
            int pim = !strcmp(type, "crash") || pim_event(data, dlen);
            for (int o = 1; o <= MAIL_OWNERS; o++)
                if (owners[o].used && (owners[o].mask == MAIL_EV_ALL || pim)) { ev_append(&owners[o].ev, id, type, data, dlen); sig = 1; }
        }
    }
    if (strcmp(type, "evt") && strcmp(type, "crash") && pending > 0) pending--;
    pthread_mutex_unlock(&mu);
    if (sig) owner_signal();
}

static void ev_push(int id, const char *type, const char *data, size_t dlen) {
    int bcast = !strcmp(type, "evt") || !strcmp(type, "crash");
    int owner = -1;
    if (!bcast) { pthread_mutex_lock(&mu); owner = route_take(id); pthread_mutex_unlock(&mu); }
    ev_push_to(owner, id, type, data, dlen);
}

char *mail_poll(void) {
    pthread_mutex_lock(&mu);
    char *r = NULL;
    if (events.n) {
        size_t n = events.n;
        r = malloc(n + 3);
        if (r) { r[0] = '['; memcpy(r + 1, events.p, n); r[n + 1] = ']'; r[n + 2] = 0; }
        events.n = 0;
    }
    pthread_mutex_unlock(&mu);
    return r;
}

int mail_owner_open(int mask) {
    pthread_mutex_lock(&mu);
    if (owner_wake < 0) owner_wake = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    int got = -1;
    for (int o = 1; o <= MAIL_OWNERS && got < 0; o++) if (!owners[o].used) {
        jb_init(&owners[o].ev);
        owners[o].used = 1;
        owners[o].mask = mask == MAIL_EV_ALL ? MAIL_EV_ALL : MAIL_EV_PIM;
        got = o;
    }
    pthread_mutex_unlock(&mu);
    return got;
}

void mail_owner_close(int owner) {
    if (owner <= 0 || owner > MAIL_OWNERS) return;
    pthread_mutex_lock(&mu);
    if (owners[owner].used) { jb_free(&owners[owner].ev); owners[owner].used = 0; }
    for (int i = 0; i < MAX_ROUTE; i++) if (routes[i].id && routes[i].owner == owner) routes[i].owner = -2;   /* its replies are dropped */
    pthread_mutex_unlock(&mu);
    /* its decoded images go too */
    pthread_mutex_lock(&img_mu);
    for (int i = 0; i < MAX_IMG; i++) if (imgs[i].px && imgs[i].owner == owner) {
        img_bytes -= (size_t)imgs[i].w * (size_t)imgs[i].h * 4;
        free(imgs[i].px);
        memset(&imgs[i], 0, sizeof imgs[i]);
    }
    pthread_mutex_unlock(&img_mu);
}

char *mail_poll_owner(int owner) {
    if (owner <= 0 || owner > MAIL_OWNERS) return NULL;
    pthread_mutex_lock(&mu);
    char *r = NULL;
    Owner *o = &owners[owner];
    if (o->used && o->ev.n) {
        size_t n = o->ev.n;
        r = malloc(n + 3);
        if (r) { r[0] = '['; memcpy(r + 1, o->ev.p, n); r[n + 1] = ']'; r[n + 2] = 0; }
        o->ev.n = 0;
    }
    pthread_mutex_unlock(&mu);
    return r;
}

int mail_owner_pollfd(void) { return owner_wake; }
void mail_owner_clear_wake(void) { uint64_t v; if (owner_wake >= 0 && read(owner_wake, &v, sizeof v) < 0) { /* nothing pending */ } }

int mail_pending(void) { pthread_mutex_lock(&mu); int p = pending; pthread_mutex_unlock(&mu); return p; }

/* ---- images ---- */

static int img_store(int w, int h, int fd, int owner) {
    size_t bytes = (size_t)w * (size_t)h * 4;
    void *m = mmap(NULL, bytes, PROT_READ, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) return -1;
    uint32_t *px = malloc(bytes);
    if (!px) { munmap(m, bytes); return -1; }
    memcpy(px, m, bytes);
    munmap(m, bytes);
    pthread_mutex_lock(&img_mu);
    /* make room: oldest first */
    while (img_bytes + bytes > IMG_BUDGET) {
        int old = -1;
        for (int i = 0; i < MAX_IMG; i++) if (imgs[i].px && (old < 0 || imgs[i].seq < imgs[old].seq)) old = i;
        if (old < 0) break;
        img_bytes -= (size_t)imgs[old].w * (size_t)imgs[old].h * 4;
        free(imgs[old].px);
        memset(&imgs[old], 0, sizeof imgs[old]);
    }
    int slot = -1;
    for (int i = 0; i < MAX_IMG; i++) if (!imgs[i].px) { slot = i; break; }
    if (slot < 0) {
        slot = 0;
        for (int i = 1; i < MAX_IMG; i++) if (imgs[i].seq < imgs[slot].seq) slot = i;
        img_bytes -= (size_t)imgs[slot].w * (size_t)imgs[slot].h * 4;
        free(imgs[slot].px);
    }
    imgs[slot] = (Img){ .handle = next_handle++, .w = w, .h = h, .owner = owner, .px = px, .seq = ++img_seq };
    img_bytes += bytes;
    int handle = imgs[slot].handle;
    pthread_mutex_unlock(&img_mu);
    return handle;
}

const uint32_t *mail_image_lock(int handle, int *w, int *h) {
    pthread_mutex_lock(&img_mu);
    for (int i = 0; i < MAX_IMG; i++) if (imgs[i].px && imgs[i].handle == handle && imgs[i].owner == 0) { *w = imgs[i].w; *h = imgs[i].h; imgs[i].seq = ++img_seq; return imgs[i].px; }
    return NULL;
}
void mail_image_unlock(void) { pthread_mutex_unlock(&img_mu); }
void mail_image_release(int handle) { mail_image_release_as(0, handle); }

void mail_image_release_as(int owner, int handle) {
    pthread_mutex_lock(&img_mu);
    for (int i = 0; i < MAX_IMG; i++) if (imgs[i].px && imgs[i].handle == handle && imgs[i].owner == owner) {
        img_bytes -= (size_t)imgs[i].w * (size_t)imgs[i].h * 4;
        free(imgs[i].px);
        memset(&imgs[i], 0, sizeof imgs[i]);
    }
    pthread_mutex_unlock(&img_mu);
}

/* A copy of an owner's decoded image in a sealed memfd (for the app process to map read-only). */
int mail_image_fd(int owner, int handle, int *w, int *h) {
    int fd = -1;
    pthread_mutex_lock(&img_mu);
    for (int i = 0; i < MAX_IMG; i++) if (imgs[i].px && imgs[i].handle == handle && imgs[i].owner == owner && owner > 0) {
        size_t bytes = (size_t)imgs[i].w * (size_t)imgs[i].h * 4;
        fd = memfd_create("lordos-mail-img", MFD_CLOEXEC | MFD_ALLOW_SEALING);
        if (fd >= 0) {
            size_t off = 0;
            while (off < bytes) { ssize_t r = write(fd, (const char *)imgs[i].px + off, bytes - off); if (r <= 0) break; off += (size_t)r; }
            if (off != bytes || fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL)) { close(fd); fd = -1; }
        }
        *w = imgs[i].w;
        *h = imgs[i].h;
        imgs[i].seq = ++img_seq;
        break;
    }
    pthread_mutex_unlock(&img_mu);
    return fd;
}

/* ---- connector ---- */

static int v4_public(uint32_t a) {          /* host byte order */
    uint8_t b0 = (uint8_t)(a >> 24), b1 = (uint8_t)(a >> 16);
    if (b0 == 0 || b0 == 10 || b0 == 127) return 0;                    /* this net, private, loopback */
    if (b0 == 100 && (b1 & 0xc0) == 64) return 0;                       /* 100.64/10 carrier NAT */
    if (b0 == 169 && b1 == 254) return 0;                               /* link-local, cloud metadata */
    if (b0 == 172 && (b1 & 0xf0) == 16) return 0;                       /* 172.16/12 */
    if (b0 == 192 && b1 == 168) return 0;                               /* 192.168/16 */
    if (b0 == 192 && b1 == 0 && (uint8_t)(a >> 8) == 0) return 0;       /* 192.0.0/24 IETF */
    if (b0 == 198 && (b1 & 0xfe) == 18) return 0;                       /* 198.18/15 benchmarking */
    if (b0 >= 224) return 0;                                            /* multicast, reserved, broadcast */
    return 1;
}

int mail_addr_public(const struct sockaddr *sa) {
    if (sa->sa_family == AF_INET) return v4_public(ntohl(((const struct sockaddr_in *)(const void *)sa)->sin_addr.s_addr));
    if (sa->sa_family != AF_INET6) return 0;
    const uint8_t *b = ((const struct sockaddr_in6 *)(const void *)sa)->sin6_addr.s6_addr;
    static const uint8_t zero[16];
    if (!memcmp(b, zero, 16)) return 0;                                  /* :: */
    if (!memcmp(b, zero, 15) && b[15] == 1) return 0;                    /* ::1 */
    if (b[0] == 0xfe && (b[1] & 0xc0) == 0x80) return 0;                 /* fe80::/10 link-local */
    if (b[0] == 0xfe && (b[1] & 0xc0) == 0xc0) return 0;                 /* fec0::/10 site-local (old) */
    if ((b[0] & 0xfe) == 0xfc) return 0;                                 /* fc00::/7 unique local */
    if (b[0] == 0xff) return 0;                                          /* multicast */
    uint32_t v4 = (uint32_t)b[12] << 24 | (uint32_t)b[13] << 16 | (uint32_t)b[14] << 8 | b[15];
    /* v4-mapped ::ffff:a.b.c.d and v4-compatible ::a.b.c.d */
    if (!memcmp(b, zero, 10) && b[10] == 0xff && b[11] == 0xff) return v4_public(v4);
    if (!memcmp(b, zero, 12)) return 0;
    /* NAT64 64:ff9b::/96 (and the local-use 64:ff9b:1::/48): judge the embedded IPv4 */
    if (b[0] == 0 && b[1] == 0x64 && b[2] == 0xff && b[3] == 0x9b) { if (b[4] == 0 && b[5] == 1) return 0; return v4_public(v4); }
    if (b[0] == 0x20 && b[1] == 0x01 && b[2] == 0x0d && b[3] == 0xb8) return 0;   /* 2001:db8::/32 documentation */
    if (b[0] == 0x20 && b[1] == 0x02) return v4_public((uint32_t)b[2] << 24 | (uint32_t)b[3] << 16 | (uint32_t)b[4] << 8 | b[5]);   /* 6to4 */
    return 1;
}

typedef struct { uint32_t cid; char host[256]; int port; int image; } ConnReq;

static int send_msg(uint32_t op, uint32_t id, const char *json, int fd) {
    pthread_mutex_lock(&send_mu);
    int s = sock;
    int r = s >= 0 ? doc_send(s, op, id, json, json ? strlen(json) : 0, fd >= 0 ? &fd : NULL, fd >= 0 ? 1 : 0) : -1;
    pthread_mutex_unlock(&send_mu);
    return r;
}

/* The socket is made and connected by Mail's own connector, in lordos-net-mail.scope (0.8.5,
 * sec/netconn.h): Mail and the calendar sync are their own app for the firewall and the Monitor. */
static int connect_timeout(const struct addrinfo *ai, int ms) {
    int fd = netconn_socket(NC_MAIL, ai->ai_family, SOCK_STREAM, ai->ai_addr, ai->ai_addrlen);
    if (fd < 0) return -1;
    struct pollfd p = { fd, POLLOUT, 0 };
    if (poll(&p, 1, ms) != 1) { close(fd); return -1; }
    int err = 0;
    socklen_t el = sizeof err;
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) || err) { close(fd); return -1; }
    int fl = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
    return fd;
}

static void *connector(void *arg) {
    ConnReq *q = arg;
    char port[8];
    snprintf(port, sizeof port, "%d", q->port);
    struct addrinfo hints = { 0 }, *res = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_ADDRCONFIG;
    int fd = -1;
    const char *why = "dns";
    if (!getaddrinfo(q->host, port, &hints, &res)) {
        why = "connect";
        for (struct addrinfo *a = res; a && fd < 0; a = a->ai_next) {
            /* image hosts (chosen by whoever sent the mail): never loopback, LAN or link-local,
             * judged on the resolved address we actually connect to (no DNS rebinding) */
            if (q->image && !mail_addr_public(a->ai_addr)) { why = "denied"; continue; }
            fd = connect_timeout(a, 10000);
        }
        freeaddrinfo(res);
    }
    if (fd >= 0) { send_msg(MQ_SOCK, q->cid, "{}", fd); close(fd); }
    else { char j[64]; snprintf(j, sizeof j, "{\"error\":\"%s\"}", why); send_msg(MQ_SOCK, q->cid, j, -1); }
    free(q);
    return NULL;
}

static void on_connect(uint32_t cid, const char *json, size_t len) {
    JDoc d;
    char host[256] = "";
    int port = 0;
    if (json && !json_parse(json, len, &d, NULL, 0)) {
        const char *h = json_str(d.root, "host");
        if (h && strlen(h) < sizeof host) { snprintf(host, sizeof host, "%s", h); for (char *c = host; *c; c++) *c = (char)tolower((unsigned char)*c); }
        port = (int)json_num(d.root, "port", 0);
        json_free(&d);
    }
    int how = host_ok(host) && port > 0 && port <= 65535 ? allowed_now(host, port) : 0;
    if (!how) {
        fprintf(stderr, "lord-server: lord-maild asked for %s:%d, which is not a configured server: denied\n", host, port);
        send_msg(MQ_SOCK, cid, "{\"error\":\"denied\"}", -1);
        return;
    }
    ConnReq *q = malloc(sizeof *q);
    if (!q) { send_msg(MQ_SOCK, cid, "{\"error\":\"connect\"}", -1); return; }
    q->cid = cid;
    snprintf(q->host, sizeof q->host, "%s", host);
    q->port = port;
    q->image = how == 2;
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &at, connector, q)) { free(q); send_msg(MQ_SOCK, cid, "{\"error\":\"connect\"}", -1); }
    pthread_attr_destroy(&at);
}

/* ---- the helper process ---- */

static void *loop(void *arg) {
    (void)arg;
    for (;;) {
        struct pollfd p[2] = { { sock, POLLIN, 0 }, { wakefd, POLLIN, 0 } };
        int r = poll(p, 2, -1);
        if (r < 0) { if (errno == EINTR) continue; break; }
        if (p[1].revents & POLLIN) { uint64_t v; if (read(wakefd, &v, sizeof v) < 0) { /* nothing */ } }
        if (!(p[0].revents & (POLLIN | POLLHUP | POLLERR))) continue;
        DocMsg m;
        int k = doc_recv(sock, &m);
        if (k) {
            /* the helper is gone: every pending request fails, and the next one restarts it */
            pthread_mutex_lock(&send_mu);
            close(sock);
            sock = -1;
            pthread_mutex_unlock(&send_mu);
            if (child > 0) { kill(child, SIGKILL); waitpid(child, NULL, 0); child = 0; }
            pthread_mutex_lock(&mu);
            int np = pending;
            pending = 0;
            running = 0;
            pthread_mutex_unlock(&mu);
            (void)np;
            pthread_mutex_lock(&mu);
            memset(routes, 0, sizeof routes);           /* every pending request failed */
            pthread_mutex_unlock(&mu);
            ev_push_to(-1, 0, "crash", NULL, 0);
            last_crash = now_ms();
            break;
        }
        switch (m.h.op) {
        case MR_DONE: ev_push((int)m.h.id, "done", m.json, m.len); break;
        case MR_ERR: ev_push((int)m.h.id, "err", m.json, m.len); break;
        case MR_EVT: ev_push(0, "evt", m.json, m.len); break;
        case MR_CONNECT: on_connect(m.h.id, m.json, m.len); break;
        case MR_IMG: {
            JDoc d;
            int w = 0, h = 0;
            if (m.json && !json_parse(m.json, m.len, &d, NULL, 0)) { w = (int)json_num(d.root, "w", 0); h = (int)json_num(d.root, "h", 0); json_free(&d); }
            int handle = -1;
            pthread_mutex_lock(&mu);
            int owner = route_take((int)m.h.id);
            pthread_mutex_unlock(&mu);
            if (owner >= 0 && m.nfds == 1 && w > 0 && h > 0 && w <= 8192 && h <= 8192 && doc_check_sealed(m.fds[0], (size_t)w * (size_t)h * 4) >= 0)
                handle = img_store(w, h, m.fds[0], owner);
            char j[96];
            if (handle > 0) snprintf(j, sizeof j, "{\"image\":%d,\"w\":%d,\"h\":%d}", handle, w, h);
            else snprintf(j, sizeof j, "{\"error\":\"image\"}");
            ev_push_to(owner, (int)m.h.id, handle > 0 ? "done" : "err", j, strlen(j));
            break;
        }
        default: break;
        }
        doc_msg_free(&m);
    }
    return NULL;
}

static int local_zone(char *out, size_t cap) {
    char l[PATH_MAX];
    ssize_t n = readlink("/etc/localtime", l, sizeof l - 1);
    out[0] = 0;
    if (n <= 0) return 0;
    l[n] = 0;
    const char *z = strstr(l, "zoneinfo/");
    if (!z) return 0;
    snprintf(out, cap, "%s", z + 9);
    return 1;
}

static int mkdirs(const char *p, mode_t mode) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s", p);
    for (char *s = tmp + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(tmp, mode); *s = '/'; }
    return mkdir(tmp, mode) && errno != EEXIST ? -1 : 0;
}

int mail_start(void) {
    pthread_mutex_lock(&mu);
    int r = running;
    pthread_mutex_unlock(&mu);
    if (r) return 0;
    if (mail_available()) return -1;
    if (last_crash && now_ms() - last_crash < 3000) return -1;     /* do not spin on a crash loop */
    if (thread) { pthread_join(thread, NULL); thread = 0; }
    paths();
    if (mkdirs(cache_path, 0700)) return -1;
    int dfd = open(cache_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dfd < 0) return -1;
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) { close(dfd); return -1; }
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
    char henv[PATH_MAX + 8], *envp[6];
    int ne = 0;
    const char *home = getenv("HOME");
    if (home) { snprintf(henv, sizeof henv, "HOME=%s", home); envp[ne++] = henv; }
    envp[ne++] = "LANG=C.UTF-8";
    if (getenv("LORD_MAIL_DEBUG")) envp[ne++] = "LORD_MAIL_DEBUG=1";
    envp[ne] = NULL;
    char *argv[] = { "lord-maild", NULL };
    pid_t pid;
    int e = posix_spawn(&pid, helper, &fa, &at, argv, envp);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    close(sv[1]);
    if (e) { close(sv[0]); close(dfd); return -1; }
    size_t clen;
    char *cfg = read_config(&clen);
    pthread_mutex_lock(&mu);
    nallow = build_allow(cfg, clen, allow);
    pthread_mutex_unlock(&mu);
    char zone[128];
    local_zone(zone, sizeof zone);
    JBuf b;
    jb_init(&b);
    jb_raw(&b, "{\"config\":");
    jb_raw(&b, cfg ? cfg : "{}");
    jb_raw(&b, ",\"zone\":");
    jb_str(&b, zone);
    jb_raw(&b, "}");
    free(cfg);
    int ok = !b.oom && doc_send(sv[0], MQ_INIT, 0, b.p, b.n, &dfd, 1) == 0;
    jb_free(&b);
    close(dfd);
    if (!ok) { close(sv[0]); kill(pid, SIGKILL); waitpid(pid, NULL, 0); return -1; }
    if (wakefd < 0) wakefd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    pthread_mutex_lock(&send_mu);
    sock = sv[0];
    pthread_mutex_unlock(&send_mu);
    child = pid;
    pthread_mutex_lock(&mu);
    running = 1;
    pthread_mutex_unlock(&mu);
    if (pthread_create(&thread, NULL, loop, NULL)) { pthread_mutex_lock(&mu); running = 0; pthread_mutex_unlock(&mu); return -1; }
    return 0;
}

int mail_request(const char *json, size_t len, int *fds, int nfds) { return mail_request_as(0, json, len, fds, nfds); }

int mail_request_as(int owner, const char *json, size_t len, int *fds, int nfds) {
    if (owner < 0 || owner > MAIL_OWNERS || mail_start()) { for (int i = 0; i < nfds; i++) close(fds[i]); return -1; }
    pthread_mutex_lock(&mu);
    if (owner > 0) {
        int slot = -1;
        for (int i = 0; i < MAX_ROUTE && slot < 0; i++) if (!routes[i].id) slot = i;
        if (!owners[owner].used || slot < 0) { pthread_mutex_unlock(&mu); for (int i = 0; i < nfds; i++) close(fds[i]); return -1; }
        routes[slot] = (Route){ next_id, owner };
    }
    int id = next_id++;
    if (next_id > 1 << 30) next_id = 1;
    pending++;
    pthread_mutex_unlock(&mu);
    pthread_mutex_lock(&send_mu);
    int r = sock >= 0 ? doc_send(sock, MQ_REQ, (uint32_t)id, json, len, nfds ? fds : NULL, nfds) : -1;
    pthread_mutex_unlock(&send_mu);
    for (int i = 0; i < nfds; i++) close(fds[i]);
    if (r) { pthread_mutex_lock(&mu); pending--; route_take(id); pthread_mutex_unlock(&mu); return -1; }
    return id;
}

int mail_save_config(const char *j, size_t len) {
    JDoc d;
    int bad = len > MAIL_CONFIG_MAX || json_parse(j, len, &d, NULL, 0);
    if (!bad) {
        const JVal *acc = json_get(d.root, "accounts");
        bad = d.root->type != J_OBJ || (acc && (acc->type != J_ARR || acc->len > MAIL_MAX_ACCOUNTS));
        /* no secrets in this file */
        for (const JVal *a = acc ? acc->kid : NULL; !bad && a; a = a->next) if (json_get(a, "password") || json_get(a, "secret")) bad = 1;
        json_free(&d);
    }
    if (bad) return 1;
    paths();
    char dir[PATH_MAX], tmp[PATH_MAX + 16];
    snprintf(dir, sizeof dir, "%s", cfg_path);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0;
    if (mkdirs(dir, 0700)) return 2;
    snprintf(tmp, sizeof tmp, "%s.tmp", cfg_path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    int ok = fd >= 0 && write(fd, j, len) == (ssize_t)len && fsync(fd) == 0;
    if (fd >= 0) close(fd);
    ok = ok && rename(tmp, cfg_path) == 0;
    if (!ok) { unlink(tmp); return 2; }
    mail_reload_config();
    return 0;
}

void mail_reload_config(void) {
    size_t clen;
    char *cfg = read_config(&clen);
    pthread_mutex_lock(&mu);
    nallow = build_allow(cfg, clen, allow);
    int r = running;
    pthread_mutex_unlock(&mu);
    if (r) {
        JBuf b;
        jb_init(&b);
        jb_raw(&b, "{\"config\":");
        jb_raw(&b, cfg ? cfg : "{}");
        jb_raw(&b, "}");
        if (!b.oom) send_msg(MQ_CONFIG, 0, b.p, -1);
        jb_free(&b);
    }
    free(cfg);
}
