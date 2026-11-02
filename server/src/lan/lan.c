/* lan.c — see lan.h. */
#include "lan.h"
#include "lanaddr.h"
#include "lanparse.h"
#include "lanproto.h"
#include "lanservices.h"
#include "../doc/wire.h"
#include "../print/mdns.h"
#include "../sec/json.h"
#include "../sec/netconn.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <limits.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_IF 4
#define MAX_HOSTS 128
#define MAX_SVC 16
#define MAX_SWEEP 512                   /* addresses in one pass, whatever the interfaces */
#define MAX_FLIGHT 24
#define CONNECT_MS 600
#define JOB_MS 5500                     /* a little over the helper's own limit */
#define MDNS_MS 1800
#define MDNS_Q 1200                     /* the largest query packet */

typedef struct { char kind[10]; char name[96]; int port; char path[128]; char model[64]; } Svc;

typedef struct {
    uint32_t a;                         /* host order */
    char name[128], domain[64];
    Svc svc[MAX_SVC];
    int nsvc;
    int smb, nfs;                       /* the port answered */
    int smb_done, nfs_done;
    char session[12];
    int listed, signing, dialect;
    LanShare *shares;
    int nshares;
    LanExport *exports;
    int nexports;
} Host;

typedef struct { uint32_t addr, mask; char name[IF_NAMESIZE]; } Iface;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static Host *hosts[MAX_HOSTS];
static int nhosts;
static const char *state = "idle";      /* idle | no-lan | no-connector | denied | no-helper | mdns | sweep | done */
static int probed, total, running;
static long long last_start;
static atomic_int cancel;

static long long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000LL + t.tv_nsec / 1000000;
}

static void set_state(const char *s) { pthread_mutex_lock(&mu); state = s; pthread_mutex_unlock(&mu); }

/* ---- sockets ----
 * Made by the component's connector, so the kernel counts and decides them as lordos:lan (sec/netconn.h). */
static int lan_socket(int type, const struct sockaddr *to, socklen_t tolen) { return netconn_socket(NC_LAN, AF_INET, type, to, tolen); }

static int lan_tcp(uint32_t a, int port) {
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port), .sin_addr.s_addr = htonl(a) };
    return lan_socket(SOCK_STREAM, (struct sockaddr *)&to, sizeof to);
}

/* ---- where ---- */

static Iface ifs[MAX_IF];
static int nifs;

/* into l: the pass's own table, or a sign-in's, which may run during a pass */
static int ifaces_into(Iface *l) {
    int nifs = 0;
    struct ifaddrs *all = NULL;
    if (getifaddrs(&all)) return 0;
    for (struct ifaddrs *i = all; i && nifs < MAX_IF; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !i->ifa_netmask) continue;
        if (!(i->ifa_flags & IFF_UP) || !(i->ifa_flags & IFF_RUNNING) || i->ifa_flags & (IFF_LOOPBACK | IFF_POINTOPOINT)) continue;
        /* a real device: containers' bridges, veths and VPN tunnels have none */
#ifndef LAN_ANY_LINK                    /* test builds only: a veth to a network namespace */
        char p[64 + IF_NAMESIZE];
        snprintf(p, sizeof p, "/sys/class/net/%s/device", i->ifa_name);
        if (access(p, F_OK)) continue;
#endif
        uint32_t a = ntohl(((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr), m = ntohl(((struct sockaddr_in *)i->ifa_netmask)->sin_addr.s_addr);
        if (!lan_private(a)) continue;
        l[nifs].addr = a;
        l[nifs].mask = m;
        snprintf(l[nifs].name, sizeof l[nifs].name, "%s", i->ifa_name);
        nifs++;
    }
    freeifaddrs(all);
    return nifs;
}

static void load_ifaces(void) { nifs = ifaces_into(ifs); }

/* on one of those subnets, private, and neither us, the network nor its broadcast */
static int on_link_of(const Iface *l, int n, uint32_t a) {
    if (!lan_private(a)) return 0;
    for (int i = 0; i < n; i++) if (l[i].addr == a) return 0;
    for (int i = 0; i < n; i++) {
        uint32_t m = l[i].mask;
        if ((a & m) == (l[i].addr & m) && (a & ~m) != 0 && (a & ~m) != ~m) return 1;
    }
    return 0;
}
static int on_link(uint32_t a) { return on_link_of(ifs, nifs, a); }

static Host *host_of(uint32_t a) {
    for (int i = 0; i < nhosts; i++) if (hosts[i]->a == a) return hosts[i];
    if (nhosts >= MAX_HOSTS) return NULL;
    Host *h = calloc(1, sizeof *h);
    if (!h) return NULL;
    h->a = a;
    hosts[nhosts++] = h;
    return h;
}

static void hosts_clear(void) {
    for (int i = 0; i < nhosts; i++) { free(hosts[i]->shares); free(hosts[i]->exports); free(hosts[i]); }
    nhosts = 0;
}

/* ---- the helper ---- */

static int helper_fd = -1;
static pid_t helper_pid;

static int helper_spawn(int *out, pid_t *pid) {
    char bin[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", bin, sizeof bin - 16);
    if (n <= 0) return -1;
    bin[n] = 0;
    char *slash = strrchr(bin, '/');
    if (!slash) return -1;
    strcpy(slash + 1, "axia-lan");
    struct stat st;
    if (stat(bin, &st) || !S_ISREG(st.st_mode) || (st.st_uid != 0 && st.st_uid != geteuid()) || (st.st_mode & 022)) return -1;   /* root's or ours, not writable by others */
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
    char *argv[] = { "axia-lan", NULL };
    char *envp[] = { "LANG=C.UTF-8", getenv("LORDOS_SESSIOND") ? "LORDOS_SESSIOND=1" : NULL, NULL };
    int e = posix_spawn(pid, bin, &fa, &at, argv, envp);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    close(sv[1]);
    if (e) { close(sv[0]); return -1; }
    *out = sv[0];
    return 0;
}
static int helper_start(void) { return helper_spawn(&helper_fd, &helper_pid); }

static void helper_stop(void) {
    if (helper_fd < 0) return;
    doc_send(helper_fd, LQ_END, 0, "{}", 2, NULL, 0);
    close(helper_fd);
    helper_fd = -1;
    for (int i = 0; i < 25; i++) {
        if (waitpid(helper_pid, NULL, WNOHANG) == helper_pid) return;
        struct timespec ts = { 0, 20 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    kill(helper_pid, SIGKILL);
    waitpid(helper_pid, NULL, 0);
}

/* One answer, or the helper is gone (it never gets a second chance within a job: a helper that
 * doesn't answer in time is killed and started again). 0 ok; the caller frees m. */
static int helper_wait(uint32_t id, DocMsg *m) {
    long long end = now_ms() + JOB_MS;
    while (now_ms() < end && !atomic_load(&cancel)) {
        struct pollfd p = { helper_fd, POLLIN, 0 };
        int r = poll(&p, 1, 200);
        if (r < 0 && errno != EINTR) break;
        if (r <= 0) continue;
        if (doc_recv(helper_fd, m)) break;
        if (m->h.id == id && m->json) return 0;
        doc_msg_free(m);
    }
    kill(helper_pid, SIGKILL);
    helper_stop();
    if (!atomic_load(&cancel)) helper_start();
    return -1;
}

static uint32_t job_id;

static int ask(uint32_t op, const char *payload, size_t len, int fd, DocMsg *m) {
    if (helper_fd < 0) { if (fd >= 0) close(fd); return -1; }
    uint32_t id = ++job_id;
    int r = doc_send(helper_fd, op, id, payload, len, fd >= 0 ? &fd : NULL, fd >= 0 ? 1 : 0);
    if (fd >= 0) close(fd);
    return r ? -1 : helper_wait(id, m);
}

/* ---- mDNS ---- */

static const char *svc_kind(const char *type) {
    for (int i = 0; i < LAN_SERVICES; i++) if (type && !strcmp(lan_services[i].type, type)) return lan_services[i].kind;
    return NULL;
}

static void add_service(const JVal *s) {
    const char *kind = svc_kind(json_str(s, "type")), *addr = json_str(s, "addr");
    int port = (int)json_num(s, "port", 0);
    struct in_addr in;
    if (!kind || !addr || port < 1 || port > 65535 || inet_pton(AF_INET, addr, &in) != 1) return;
    uint32_t a = ntohl(in.s_addr);
    if (!on_link(a)) return;                            /* an answer can name any address: only the LAN's count */
    char name[96] = "", host[128] = "", path[128] = "", model[64] = "";
    lan_clean_text(json_str(s, "name"), name, sizeof name);
    lan_clean_text(json_str(s, "host"), host, sizeof host);
    lan_clean_text(json_str(s, "path"), path, sizeof path);
    lan_clean_text(json_str(s, "model"), model, sizeof model);
    char *dot = strstr(host, ".local");
    if (dot && !dot[6]) *dot = 0;
    pthread_mutex_lock(&mu);
    Host *h = host_of(a);
    if (h) {
        if (h->nsvc < MAX_SVC) {
            int dup = 0;
            for (int i = 0; i < h->nsvc; i++) if (!strcmp(h->svc[i].kind, kind) && h->svc[i].port == port) dup = 1;
            if (!dup) {
                Svc *v = &h->svc[h->nsvc++];
                snprintf(v->kind, sizeof v->kind, "%s", kind);
                snprintf(v->name, sizeof v->name, "%s", name);
                snprintf(v->path, sizeof v->path, "%s", path);
                snprintf(v->model, sizeof v->model, "%s", model);
                v->port = port;
            }
        }
        if (host[0] && !h->name[0]) snprintf(h->name, sizeof h->name, "%s", host);
        if (!strcmp(kind, "smb")) h->smb = 1;
        if (!strcmp(kind, "nfs")) h->nfs = 1;
    }
    pthread_mutex_unlock(&mu);
}

/* Sends q (ql bytes, up to `packets` of them, each MDNS_Q long) `sends` times and hands what answers to the helper for ms. */
static void mdns_round(const uint8_t *q, const size_t *ql, int packets, int sends, int ms) {
    int fds[MAX_IF];
    for (int i = 0; i < nifs; i++) {
        fds[i] = lan_socket(SOCK_DGRAM, NULL, 0);
        if (fds[i] < 0) continue;
        unsigned char ttl = 255;
        struct in_addr my = { htonl(ifs[i].addr) };
        setsockopt(fds[i], IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
        setsockopt(fds[i], IPPROTO_IP, IP_MULTICAST_IF, &my, sizeof my);
    }
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(5353), .sin_addr.s_addr = htonl(0xe00000fbu) };
    long long start = now_ms(), next = start;
    int sent = 0;
    while (now_ms() - start < ms && !atomic_load(&cancel)) {
        if (now_ms() >= next && sent < sends) {
            for (int k = 0; k < packets; k++)
                for (int i = 0; i < nifs; i++) if (fds[i] >= 0 && ql[k]) sendto(fds[i], q + (size_t)k * MDNS_Q, ql[k], 0, (struct sockaddr *)&to, sizeof to);
            sent++;
            next = now_ms() + (sent == 1 ? 350 : 700);
        }
        struct pollfd p[MAX_IF];
        for (int i = 0; i < nifs; i++) p[i] = (struct pollfd){ fds[i], POLLIN, 0 };
        if (poll(p, (nfds_t)nifs, 100) <= 0) continue;
        for (int i = 0; i < nifs; i++) {
            if (!(p[i].revents & POLLIN)) continue;
            char pkt[4 + 1500];
            struct sockaddr_in from;
            socklen_t fl = sizeof from;
            ssize_t r = recvfrom(fds[i], pkt + 4, sizeof pkt - 4, 0, (struct sockaddr *)&from, &fl);
            if (r <= 0) continue;
            memcpy(pkt, &from.sin_addr.s_addr, 4);
            if (helper_fd >= 0) doc_send(helper_fd, LQ_MDNS, 0, pkt, (size_t)r + 4, NULL, 0);
        }
    }
    for (int i = 0; i < nifs; i++) if (fds[i] >= 0) close(fds[i]);
}

static void add_name(const JVal *r) {
    const char *addr = json_str(r, "addr");
    char name[128];
    struct in_addr in;
    if (!addr || inet_pton(AF_INET, addr, &in) != 1 || !on_link(ntohl(in.s_addr)) || !lan_clean_text(json_str(r, "host"), name, sizeof name)) return;
    pthread_mutex_lock(&mu);
    Host *h = host_of(ntohl(in.s_addr));
    if (h && !h->name[0]) snprintf(h->name, sizeof h->name, "%s", name);
    pthread_mutex_unlock(&mu);
}

/* What the helper gathered from the round that just ended. */
static void mdns_results(void) {
    DocMsg m;
    if (ask(LQ_MDNS_END, "{}", 2, -1, &m)) return;
    JDoc d;
    if (m.h.op == LR_MDNS && !json_parse(m.json, m.len, &d, NULL, 0)) {
        const JVal *l = json_get(d.root, "services");
        for (const JVal *s = l && l->type == J_ARR ? l->kid : NULL; s; s = s->next) add_service(s);
        l = json_get(d.root, "names");
        for (const JVal *s = l && l->type == J_ARR ? l->kid : NULL; s; s = s->next) add_name(s);
        json_free(&d);
    }
    doc_msg_free(&m);
}

static void mdns_pass(void) {
    const char *names[LAN_SERVICES];
    int types[LAN_SERVICES];
    char full[LAN_SERVICES][40];
    for (int i = 0; i < LAN_SERVICES; i++) {
        snprintf(full[i], sizeof full[i], "%s.local", lan_services[i].type);
        names[i] = full[i];
        types[i] = MDNS_T_PTR;
    }
    static uint8_t q[MDNS_Q];
    size_t ql = mdns_query(q, sizeof q, names, types, LAN_SERVICES, 0);
    mdns_round(q, &ql, 1, 3, MDNS_MS);
    mdns_results();
}

/* The name each machine that is there gives itself: a reverse mDNS question for its address. */
static void names_pass(const uint32_t *list, int n) {
    enum { PER = 30, PACKETS = 5 };
    static uint8_t q[MDNS_Q * PACKETS];
    size_t ql[PACKETS] = { 0 };
    int packets = 0;
    for (int o = 0; o < n && packets < PACKETS; o += PER, packets++) {
        char rev[PER][40];
        const char *names[PER];
        int types[PER], c = 0;
        for (int i = o; i < n && c < PER; i++, c++) {
            uint32_t a = list[i];
            snprintf(rev[c], sizeof rev[c], "%u.%u.%u.%u.in-addr.arpa", a & 255, (a >> 8) & 255, (a >> 16) & 255, a >> 24);
            names[c] = rev[c];
            types[c] = MDNS_T_PTR;
        }
        ql[packets] = mdns_query(q + (size_t)packets * MDNS_Q, MDNS_Q, names, types, c, 0);
    }
    if (!packets) return;
    mdns_round(q, ql, packets, 2, 900);
    mdns_results();
}

/* ---- the sweep ---- */

typedef struct { int fd; uint32_t a; int port; long long t0; } Flight;

static int denied;

static uint32_t tg[MAX_SWEEP];
static int ntg;

static int target_add(uint32_t a) {
    if (ntg >= MAX_SWEEP) return 0;
    for (int i = 0; i < ntg; i++) if (tg[i] == a) return 0;
    tg[ntg++] = a;
    return 1;
}

static void port_open(uint32_t a, int port) {
    pthread_mutex_lock(&mu);
    Host *h = host_of(a);
    if (h) {
        if (port == 445) h->smb = 1;
        else if (port == 2049) h->nfs = 1;
        else if (h->nsvc < MAX_SVC) {
            int dup = 0;
            for (int i = 0; i < h->nsvc; i++) if (h->svc[i].port == port) dup = 1;
            if (!dup) { h->svc[h->nsvc].port = port; snprintf(h->svc[h->nsvc].kind, sizeof h->svc[0].kind, "ssh"); h->nsvc++; }
        }
    }
    pthread_mutex_unlock(&mu);
}

static void port_settled(Flight *f) {
    int err = 0;
    socklen_t l = sizeof err;
    getsockopt(f->fd, SOL_SOCKET, SO_ERROR, &err, &l);
    if (!err) port_open(f->a, f->port);
    close(f->fd);
    f->fd = -1;
    pthread_mutex_lock(&mu);
    probed++;
    pthread_mutex_unlock(&mu);
    if (err == EPERM || err == EACCES) denied = 1;
}

/* targets [from, to) on np ports: at most LAN_RATE new connections a second */
static void sweep(int from, int to, const int *ports, int np) {
    Flight fl[MAX_FLIGHT];
    for (int i = 0; i < MAX_FLIGHT; i++) fl[i].fd = -1;
    int next = from * np, end = to * np, live = 0;
    long long gap = 1000 / LAN_RATE, due = now_ms();
    while ((next < end || live) && !atomic_load(&cancel)) {
        if (next < end && live < MAX_FLIGHT && now_ms() >= due) {
            uint32_t a = tg[next / np];
            int port = ports[next % np];
            next++;
            due = now_ms() + gap;
            int fd = lan_tcp(a, port);
            if (fd < 0) {
                if (errno == EPERM || errno == EACCES) denied = 1;
                pthread_mutex_lock(&mu);
                probed++;
                pthread_mutex_unlock(&mu);
                continue;
            }
            for (int i = 0; i < MAX_FLIGHT; i++) if (fl[i].fd < 0) { fl[i] = (Flight){ fd, a, port, now_ms() }; live++; break; }
        }
        struct pollfd p[MAX_FLIGHT];
        int map[MAX_FLIGHT], n = 0;
        for (int i = 0; i < MAX_FLIGHT; i++) if (fl[i].fd >= 0) { p[n] = (struct pollfd){ fl[i].fd, POLLOUT, 0 }; map[n++] = i; }
        int w = next < end && live < MAX_FLIGHT ? (int)(due - now_ms()) : 50;
        if (w < 1) w = 1;
        if (w > 50) w = 50;
        if (n) poll(p, (nfds_t)n, w);
        else { struct timespec ts = { 0, (long)w * 1000000 }; nanosleep(&ts, NULL); }
        for (int i = 0; i < n; i++) if (p[i].revents) { port_settled(&fl[map[i]]); live--; }
        long long now = now_ms();
        for (int i = 0; i < MAX_FLIGHT; i++) {
            if (fl[i].fd >= 0 && now - fl[i].t0 > CONNECT_MS) {
                close(fl[i].fd);
                fl[i].fd = -1;
                live--;
                pthread_mutex_lock(&mu);
                probed++;
                pthread_mutex_unlock(&mu);
            }
        }
    }
    for (int i = 0; i < MAX_FLIGHT; i++) if (fl[i].fd >= 0) close(fl[i].fd);
}

/* The neighbours the kernel knows: the ones already there at the start, then everything the sweep
 * woke up (a machine answers ARP whatever its firewall does with the port). */
static void add_neighbours(void) {
    FILE *f = fopen("/proc/net/arp", "re");
    if (!f) return;
    char line[256];
    if (!fgets(line, sizeof line, f)) { fclose(f); return; }
    while (fgets(line, sizeof line, f)) {
        char ip[32];
        unsigned type, flags;
        struct in_addr in;
        if (sscanf(line, "%31s 0x%x 0x%x", ip, &type, &flags) != 3 || !(flags & 2) || inet_pton(AF_INET, ip, &in) != 1) continue;
        if (on_link(ntohl(in.s_addr))) target_add(ntohl(in.s_addr));
    }
    fclose(f);
}

/* ---- asking each server ---- */

static int connected(uint32_t a, int port) {
    int fd = lan_tcp(a, port);
    if (fd < 0) return -1;
    struct pollfd p = { fd, POLLOUT, 0 };
    int err = 0;
    socklen_t l = sizeof err;
    if (poll(&p, 1, 1500) <= 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &l) || err) { close(fd); return -1; }
    return fd;
}

static void smb_ask(Host *h) {
    uint32_t a = h->a;
    char ip[16];
    struct in_addr in = { htonl(a) };
    inet_ntop(AF_INET, &in, ip, sizeof ip);
    int fd = connected(a, 445);
    DocMsg m;
    JDoc d;
    if (fd < 0 || ask(LQ_SMB, ip, strlen(ip), fd, &m)) return;
    if (m.h.op != LR_SMB || json_parse(m.json, m.len, &d, NULL, 0)) { doc_msg_free(&m); return; }
    LanShare *sh = calloc(LAN_MAX_LIST, sizeof *sh);
    int n = 0;
    const JVal *l = json_get(d.root, "shares");
    for (const JVal *s = sh && l && l->type == J_ARR ? l->kid : NULL; s && n < LAN_MAX_LIST; s = s->next) {
        if (!lan_clean_text(json_str(s, "name"), sh[n].name, sizeof sh[n].name) || strpbrk(sh[n].name, "/\\")) continue;
        lan_clean_text(json_str(s, "comment"), sh[n].comment, sizeof sh[n].comment);
        n++;
    }
    pthread_mutex_lock(&mu);
    char name[128] = "";
    lan_clean_text(json_str(d.root, "name"), name, sizeof name);
    if (name[0]) snprintf(h->name, sizeof h->name, "%s", name);
    lan_clean_text(json_str(d.root, "domain"), h->domain, sizeof h->domain);
    const char *ses = json_str(d.root, "session");
    snprintf(h->session, sizeof h->session, "%s", ses && (!strcmp(ses, "anonymous") || !strcmp(ses, "guest") || !strcmp(ses, "refused")) ? ses : "none");
    h->dialect = (int)json_num(d.root, "dialect", 0);
    h->signing = json_num(d.root, "signing", 0) == 1;
    h->listed = json_num(d.root, "listed", 0) == 1;
    free(h->shares);
    h->shares = sh;
    h->nshares = n;
    pthread_mutex_unlock(&mu);
    json_free(&d);
    doc_msg_free(&m);
}

static void nfs_ask(Host *h) {
    int fd = connected(h->a, 111);
    DocMsg m;
    JDoc d;
    if (fd < 0 || ask(LQ_PORTMAP, "{}", 2, fd, &m)) return;
    int port = m.h.op == LR_PORTMAP && !json_parse(m.json, m.len, &d, NULL, 0) ? (int)json_num(d.root, "mountd", 0) : 0;
    if (m.h.op == LR_PORTMAP) json_free(&d);
    doc_msg_free(&m);
    if (port < 1 || port > 65535) return;
    fd = connected(h->a, port);                         /* that same host, whatever the port */
    if (fd < 0 || ask(LQ_EXPORTS, "{}", 2, fd, &m)) return;
    if (m.h.op != LR_EXPORTS || json_parse(m.json, m.len, &d, NULL, 0)) { doc_msg_free(&m); return; }
    LanExport *ex = calloc(LAN_MAX_LIST, sizeof *ex);
    int n = 0;
    const JVal *l = json_get(d.root, "exports");
    for (const JVal *s = ex && l && l->type == J_ARR ? l->kid : NULL; s && n < LAN_MAX_LIST; s = s->next) {
        if (!lan_clean_text(json_str(s, "path"), ex[n].path, sizeof ex[n].path) || ex[n].path[0] != '/') continue;
        ex[n].open = json_num(s, "open", 0) == 1;
        n++;
    }
    pthread_mutex_lock(&mu);
    free(h->exports);
    h->exports = ex;
    h->nexports = n;
    pthread_mutex_unlock(&mu);
    json_free(&d);
    doc_msg_free(&m);
}

static void ask_servers(void) {
    for (int i = 0; !atomic_load(&cancel); i++) {
        pthread_mutex_lock(&mu);
        Host *h = i < nhosts ? hosts[i] : NULL;
        int do_smb = h && h->smb && !h->smb_done, do_nfs = h && h->nfs && !h->nfs_done;
        if (do_smb) h->smb_done = 1;
        if (do_nfs) h->nfs_done = 1;
        pthread_mutex_unlock(&mu);
        if (!h) return;
        if (do_smb) smb_ask(h);
        if (do_nfs && !atomic_load(&cancel)) nfs_ask(h);
    }
}

/* ---- the shares a user sees, after signing in ---- */

static pthread_mutex_t lmu = PTHREAD_MUTEX_INITIALIZER;
static struct { char addr[16], user[64], domain[64], password[256]; } lq;
static const char *lstate = "idle";
static int lbusy, lcount;
static LanShare *lshares;

static void list_done(const char *st, LanShare *sh, int n) {
    pthread_mutex_lock(&lmu);
    lstate = st;
    free(lshares);
    lshares = sh;
    lcount = n;
    lbusy = 0;
    explicit_bzero(&lq, sizeof lq);
    pthread_mutex_unlock(&lmu);
}

/* A helper of its own, one request: a sign-in can happen during a pass, or with no pass at all. */
static void *list_thread(void *arg) {
    (void)arg;
    Iface l[MAX_IF];
    int nl = ifaces_into(l);
    struct in_addr in;
    if (inet_pton(AF_INET, lq.addr, &in) != 1 || !on_link_of(l, nl, ntohl(in.s_addr))) { list_done("off-link", NULL, 0); return NULL; }
    int sock = -1;
    pid_t pid = 0;
    if (helper_spawn(&sock, &pid)) { list_done("no-helper", NULL, 0); return NULL; }
    const char *st = "failed";
    LanShare *sh = NULL;
    int n = 0, fd = connected(ntohl(in.s_addr), 445);
    if (fd < 0) st = errno == EACCES || errno == EPERM ? "denied-net" : "no-answer";
    else {
        JBuf b;
        jb_init(&b);
        int guest = !lq.user[0];
        if (guest) jb_raw(&b, lq.addr);
        else {
            jb_raw(&b, "{\"addr\":");
            jb_str(&b, lq.addr);
            jb_raw(&b, ",\"user\":");
            jb_str(&b, lq.user);
            jb_raw(&b, ",\"domain\":");
            jb_str(&b, lq.domain);
            jb_raw(&b, ",\"password\":");
            jb_str(&b, lq.password);
            jb_raw(&b, "}");
        }
        int rc = b.oom ? -1 : doc_send(sock, guest ? LQ_SMB : LQ_SMB_AUTH, 1, b.p, b.n, &fd, 1);
        if (b.p) explicit_bzero(b.p, b.cap);
        jb_free(&b);
        close(fd);
        struct pollfd p = { sock, POLLIN, 0 };
        DocMsg m;
        JDoc d;
        if (!rc && poll(&p, 1, JOB_MS + 1000) > 0 && !doc_recv(sock, &m)) {
            if (m.json && !json_parse(m.json, m.len, &d, NULL, 0)) {
                const char *e = json_str(d.root, "error");
                if (m.h.op == LR_ERR) st = e && !strcmp(e, "denied") ? "denied" : "failed";
                else if (m.h.op == LR_SMB && json_num(d.root, "listed", 0) == 1) {
                    sh = calloc(LAN_MAX_LIST, sizeof *sh);
                    const JVal *a = json_get(d.root, "shares");
                    for (const JVal *s = sh && a && a->type == J_ARR ? a->kid : NULL; s && n < LAN_MAX_LIST; s = s->next) {
                        if (!lan_clean_text(json_str(s, "name"), sh[n].name, sizeof sh[n].name) || strpbrk(sh[n].name, "/\\")) continue;
                        lan_clean_text(json_str(s, "comment"), sh[n].comment, sizeof sh[n].comment);
                        n++;
                    }
                    st = "done";
                } else if (guest) st = "denied";
                json_free(&d);
            }
            doc_msg_free(&m);
        }
    }
    doc_send(sock, LQ_END, 0, "{}", 2, NULL, 0);
    close(sock);
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    list_done(st, sh, n);
    return NULL;
}

int lan_list_start(const char *addr, const char *domain, const char *user, const char *password) {
    pthread_mutex_lock(&lmu);
    if (lbusy || strlen(addr) >= sizeof lq.addr || strlen(user) >= sizeof lq.user || strlen(domain) >= sizeof lq.domain || strlen(password) >= sizeof lq.password) {
        pthread_mutex_unlock(&lmu);
        return 0;
    }
    strcpy(lq.addr, addr); strcpy(lq.user, user); strcpy(lq.domain, domain); strcpy(lq.password, password);
    lbusy = 1;
    lstate = "running";
    pthread_mutex_unlock(&lmu);
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 1024 * 1024);
    sigset_t all, old;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &old);
    int r = pthread_create(&th, &at, list_thread, NULL);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    pthread_attr_destroy(&at);
    if (r) { list_done("no-helper", NULL, 0); return 0; }
    return 1;
}

char *lan_list_poll(void) {
    JBuf b;
    jb_init(&b);
    pthread_mutex_lock(&lmu);
    jb_printf(&b, "{\"state\":\"%s\",\"shares\":[", lstate);
    for (int k = 0; k < lcount; k++) {
        if (k) jb_raw(&b, ",");
        jb_raw(&b, "{\"name\":");
        jb_str(&b, lshares[k].name);
        jb_raw(&b, ",\"comment\":");
        jb_str(&b, lshares[k].comment);
        jb_raw(&b, "}");
    }
    pthread_mutex_unlock(&lmu);
    jb_raw(&b, "]}");
    if (b.oom) { jb_free(&b); return NULL; }
    return b.p;
}

/* ---- one pass ---- */

static void *pass(void *arg) {
    (void)arg;
    load_ifaces();
    denied = 0;
    if (!nifs) { set_state("no-lan"); goto out; }
    if (helper_start()) { set_state("no-helper"); goto out; }
    set_state("mdns");
    mdns_pass();
    /* first who is known to be there, then the rest of the subnet */
    ntg = 0;
    add_neighbours();
    pthread_mutex_lock(&mu);
    for (int i = 0; i < nhosts; i++) target_add(hosts[i]->a);
    pthread_mutex_unlock(&mu);
    int known = ntg;
    for (int i = 0; i < nifs; i++) {
        uint32_t first, last;
        if (!lan_sweep_range(ifs[i].addr, ifs[i].mask, &first, &last)) continue;
        for (uint32_t a = first; a <= last; a++) if (on_link(a)) target_add(a);
    }
    pthread_mutex_lock(&mu);
    probed = 0;
    total = ntg * 2;
    state = "sweep";
    pthread_mutex_unlock(&mu);
    static const int file_ports[2] = { 445, 2049 }, ssh_port[1] = { 22 };
    sweep(0, known, file_ports, 2);
    ask_servers();
    sweep(known, ntg, file_ports, 2);
    ask_servers();
    /* everything that answered ARP: it goes on the list, with its name and SSH if it has them */
    if (!atomic_load(&cancel)) {
        ntg = 0;
        add_neighbours();
        pthread_mutex_lock(&mu);
        for (int i = 0; i < ntg; i++) host_of(tg[i]);
        for (int i = 0; i < nhosts; i++) target_add(hosts[i]->a);
        total += ntg;
        pthread_mutex_unlock(&mu);
        sweep(0, ntg, ssh_port, 1);
        static uint32_t list[MAX_HOSTS];
        int n = 0;
        pthread_mutex_lock(&mu);
        for (int i = 0; i < nhosts; i++) if (!hosts[i]->name[0] || hosts[i]->smb) list[n++] = hosts[i]->a;
        pthread_mutex_unlock(&mu);
        names_pass(list, n);
    }
    helper_stop();
    set_state(denied ? "denied" : "done");
out:
    if (helper_fd >= 0) helper_stop();
    pthread_mutex_lock(&mu);
    running = 0;
    if (atomic_load(&cancel)) last_start = 0;
    pthread_mutex_unlock(&mu);
    return NULL;
}

int lan_start(int asked) {
    pthread_mutex_lock(&mu);
    long long t = now_ms();
    if (running || (last_start && t - last_start < (asked ? LAN_REST_S : LAN_KEEP_S) * 1000LL)) { pthread_mutex_unlock(&mu); return 0; }
    atomic_store(&cancel, 0);
    running = 1;
    last_start = t;
    probed = total = 0;
    hosts_clear();
    state = "mdns";
    pthread_mutex_unlock(&mu);
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 1024 * 1024);
    sigset_t all, old;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &old);
    int r = pthread_create(&th, &at, pass, NULL);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    pthread_attr_destroy(&at);
    if (r) {
        pthread_mutex_lock(&mu);
        running = 0;
        state = "no-helper";
        pthread_mutex_unlock(&mu);
        return 0;
    }
    return 1;
}

void lan_stop(void) { atomic_store(&cancel, 1); }

char *lan_poll(void) {
    JBuf b;
    jb_init(&b);
    pthread_mutex_lock(&mu);
    jb_printf(&b, "{\"state\":\"%s\",\"running\":%s,\"probed\":%d,\"total\":%d,\"hosts\":[", state, running ? "true" : "false", probed, total);
    for (int i = 0; i < nhosts; i++) {
        Host *h = hosts[i];
        char ip[16];
        struct in_addr in = { htonl(h->a) };
        inet_ntop(AF_INET, &in, ip, sizeof ip);
        if (i) jb_raw(&b, ",");
        jb_raw(&b, "{\"addr\":");
        jb_str(&b, ip);
        jb_raw(&b, ",\"name\":");
        jb_str(&b, h->name);
        jb_raw(&b, ",\"domain\":");
        jb_str(&b, h->domain);
        jb_printf(&b, ",\"smb\":%s,\"nfs\":%s,\"session\":\"%s\",\"listed\":%s,\"signing\":%s,\"dialect\":%d,\"services\":[",
                  h->smb ? "true" : "false", h->nfs ? "true" : "false", h->session, h->listed ? "true" : "false", h->signing ? "true" : "false", h->dialect);
        for (int k = 0; k < h->nsvc; k++) {
            Svc *v = &h->svc[k];
            if (k) jb_raw(&b, ",");
            jb_printf(&b, "{\"kind\":\"%s\",\"port\":%d,\"name\":", v->kind, v->port);
            jb_str(&b, v->name);
            jb_raw(&b, ",\"path\":");
            jb_str(&b, v->path);
            jb_raw(&b, ",\"model\":");
            jb_str(&b, v->model);
            jb_raw(&b, "}");
        }
        jb_raw(&b, "],\"shares\":[");
        for (int k = 0; k < h->nshares; k++) {
            if (k) jb_raw(&b, ",");
            jb_raw(&b, "{\"name\":");
            jb_str(&b, h->shares[k].name);
            jb_raw(&b, ",\"comment\":");
            jb_str(&b, h->shares[k].comment);
            jb_raw(&b, "}");
        }
        jb_raw(&b, "],\"exports\":[");
        for (int k = 0; k < h->nexports; k++) {
            if (k) jb_raw(&b, ",");
            jb_raw(&b, "{\"path\":");
            jb_str(&b, h->exports[k].path);
            jb_printf(&b, ",\"open\":%s}", h->exports[k].open ? "true" : "false");
        }
        jb_raw(&b, "]}");
    }
    pthread_mutex_unlock(&mu);
    jb_raw(&b, "]}");
    if (b.oom) { jb_free(&b); return NULL; }
    return b.p;
}
