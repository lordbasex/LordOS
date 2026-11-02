/* netperm.c — lord-netperm: the root service that enforces per-app network permissions in the
 * kernel (docs/FIREWALL.md, step 2). Closed and minimal:
 *
 *   - no arguments, executes nothing, no network of its own (it creates no AF_INET/AF_INET6
 *     sockets: the systemd service also forbids it with RestrictAddressFamilies);
 *   - loads the BPF programs it carries inside (np.bpf.c → build/gen/np_bpf.h) into the cgroup of
 *     lordos.service, which contains the subtrees of all sessions: the network ones and, with the
 *     same per-app policy, the device program ("no camera", "no USB") and the Bluetooth socket
 *     program ("no Bluetooth"; it needs the kernel's function list, /sys/kernel/btf/vmlinux);
 *   - reads the rules of each user with a session from ~/.config/lordos/firewall.json (fixed,
 *     validated path) and compiles them into maps (npcompile.c);
 *   - learns names only from the system resolver (npdns.c);
 *   - tells each lord-server about its apps' new connections over /run/lordos/netperm.sock
 *     (npproto.h). The only thing it accepts there is "reload my rules";
 *   - (0.8.5) reads each user's subscribed blocklists (~/.config/lordos/fwlists.txt, the same
 *     fixed-path reader) into np_blk4/np_blk6: their networks, and the addresses the resolver gave
 *     for their domains; and counts the essential system services' bytes (np_sysc);
 *   - cuts the connections a rule now denies: the packet filter drops their packets in the
 *     kernel, and this process destroys their sockets over sock_diag netlink (npdiag.c, the only
 *     socket besides AF_UNIX and the DNS AF_PACKET one), right after a reload and whenever the
 *     filter reports a socket.
 *
 * If the kernel has no cgroup BPF (or CAP_BPF is missing), it says so in the log and carries on
 * doing nothing: the desktop doesn't notice. With the service stopped, the programs detach on
 * their own (bpf_link): everything is allowed. */
#include "netperm/bpfsys.h"
#include "netperm/npcompile.h"
#include "netperm/npdiag.h"
#include "netperm/npdns.h"
#include "netperm/npload.h"
#include "netperm/npproto.h"
#include "fw/fwmap.h"
#include "fw/fwlists.h"
#include "errcode.h"
#include "sec/json.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <malloc.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/bpf.h>
#include <netinet/in.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#ifdef NP_HAVE_BPF
#include "np_bpf.h"
#endif

#define CG_ROOT      "/sys/fs/cgroup"
#define CG_SERVICE   "system.slice/lordos.service"
#define CG_USER      "user.slice"       /* user.slice/user-<uid>.slice/user@<uid>.service */
#define SVC_LEVEL    2                  /* system.slice/lordos.service */
#define SCOPE_LEVEL  (SVC_LEVEL + 2)    /* lordos-session-<uid>/<scope> */
#define USER_LEVEL   3                  /* user.slice/user-<uid>.slice/user@<uid>.service */
#define NAMES_APP    "lordos"           /* dnsmap key for what the resolver says */
#define MAX_SESS     NP_MAX_SESS
#define MAX_CLIENTS  64
#define CLIENT_OUT   (256u << 10)

typedef struct {
    char name[256];
    char app[FW_APP_MAX];
    uint64_t cgid;
    uint32_t idx;
    unsigned gen;                       /* generation of its installed nodes */
    int has_domains, seen;
    NpApp app_v;                        /* the last thing written to np_apps */
    uint64_t allowed, denied;           /* kernel counters */
    uint64_t blocked;                   /* bytes the packet filter dropped (np_counts) */
    uint64_t cuts;                      /* sockets destroyed by a sweep (counted as denied) */
    uint64_t rx, tx;                    /* bytes (np_counts) */
    double rrx, rtx;                    /* bytes per second over the last tick */
} Scope;

typedef struct {
    unsigned uid;
    char dir[64];
    int wd, seen;
    FwStore *store;
    Scope *sc;
    int nsc, cap;
    int dirty_counts;
    uint64_t user_cg;                   /* id of user@<uid>.service, or 0 if it isn't there (nor attached) */
    int ulinks[16], nulinks;            /* the programs attached to user@<uid>.service */
    FwLists *lists;                     /* (0.8.5) the subscribed blocklists, NULL: none */
    uint32_t blkset;                    /* its set in np_blk4/np_blk6 (NpApp.blk) */
    FwMap blkin;                        /* installed entries: (fam, prefix, address) → BlkEnt */
    int nlearned;                       /* entries from the resolver among them */
} Session;

typedef struct {
    int learned;                        /* 1: an address the resolver gave for a listed name */
    long long mark;                     /* the pass that last wanted it */
} BlkEnt;

#define BLK_LEARNED_MAX 16384           /* resolver addresses per user */

typedef struct {
    int fd;
    unsigned uid;
    unsigned char in[NP_IN_MAX + 8];
    size_t inlen;
    char *out;
    size_t outlen;
} Client;

typedef struct {
    uint32_t idx;
    unsigned gen;
    NpList list;
} Installed;

static struct {
    int bpf_ok;
    char bpf_err[200];                  /* a code line (errcode.h): the firewall page shows it translated */
    int m_np_cfg, m_np_apps, m_np_union, m_np_lpm4, m_np_lpm6, m_np_seen, m_np_counts, m_np_events, m_np_snd, m_np_flows, m_np_bound;
    int m_np_blk4, m_np_blk6, m_np_sysc;
    uint32_t next_blkset;
    long long blk_pass;
    struct { uint64_t cgid; int links[4], nlinks; uint64_t rx, tx; double rrx, rtx; } sys[3];   /* dns, time, network */
    int diag_fd;                        /* NETLINK_SOCK_DIAG (npdiag.c), -1 until first used */
    int diag_warned;                    /* the kernel can't destroy sockets: said once */
    int acct_ok;                        /* the traffic accounting programs loaded */
    FwMap flows;                        /* NpFlowKey → FlowSt: the previous reading of np_flows */
    long long counts_ns;                /* when push_counts last read the counters */
    uint32_t snd[NP_MAX_SND];           /* the ALSA minors in np_snd */
    int nsnd;
    int dev_ok, bt_ok;                  /* the device / Bluetooth programs loaded */
    int links[16], nlinks;
    int progs[16], nprogs;
    uint32_t attach[16];
    uint64_t attached_cg;               /* id of the attached lordos.service cgroup */
    int sys_wd;
    BpfRing ring;
    int cgfd;
    int ep, lfd, ifd, tfd, root_wd, dns_fd;
    Session sess[MAX_SESS];
    int nsess;
    Client cl[MAX_CLIENTS];
    int ncl;
    FwMap idxmap;                       /* "uid:app" → idx */
    uint32_t next_idx;
    FwMap installed;                    /* node key (idx, fam, prefix, address) → Installed */
    DnsMap *names;
    NpDns *dns;
    FwStore *empty;
    long long tick;
} G;

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("lord-netperm: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static long long mono(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}

/* ---- reading a user's rules ----------------------------------------------------------------- */

/* ~/.config/lordos/firewall.json without following links: every folder and the file must belong
 * to the user, the file must be regular and at most FW_FILE_MAX. The home folder comes from /etc/passwd. */
static char *read_user_file(unsigned uid, const char *file, size_t max, size_t *len, char *why, size_t wn) {
    struct passwd *pw = getpwuid(uid);
    if (!pw || pw->pw_dir[0] != '/') { snprintf(why, wn, "user without a home folder"); return NULL; }
    int fd = open(pw->pw_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    const char *const parts[] = { ".config", "lordos", file };
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || st.st_uid != uid) { snprintf(why, wn, "odd home folder"); if (fd >= 0) close(fd); return NULL; }
    for (int i = 0; i < 3; i++) {
        int last = i == 2;
        int nfd = openat(fd, parts[i], O_RDONLY | O_NOFOLLOW | O_CLOEXEC | (last ? O_NONBLOCK : O_DIRECTORY));
        close(fd);
        if (nfd < 0) { snprintf(why, wn, errno == ENOENT ? "no rules" : "%s: %s", parts[i], strerror(errno)); return NULL; }
        fd = nfd;
        if (fstat(fd, &st) || st.st_uid != uid || (last && (!S_ISREG(st.st_mode) || st.st_size > (off_t)max))) {
            snprintf(why, wn, "%s is not the user's or not a regular file", parts[i]);
            close(fd);
            return NULL;
        }
    }
    char *buf = malloc((size_t)st.st_size + 1);
    size_t got = 0;
    while (buf && got < (size_t)st.st_size) {
        ssize_t n = read(fd, buf + got, (size_t)st.st_size - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);
    if (!buf || got != (size_t)st.st_size) { free(buf); snprintf(why, wn, "could not read"); return NULL; }
    buf[got] = 0;
    *len = got;
    return buf;
}

static void load_rules(Session *s) {
    size_t len = 0;
    char why[160] = "", err[200] = "";
    char *text = read_user_file(s->uid, "firewall.json", FW_FILE_MAX, &len, why, sizeof why);
    FwStore *n = fw_store_new();
    if (!n) { free(text); return; }
    if (text && fw_from_json(n, text, len, err, sizeof err)) {
        logf_("uid %u: invalid firewall.json (%s); the previous rules stay in use", s->uid, err);
        fw_store_free(n);
        free(text);
        return;
    }
    if (!text && strcmp(why, "no rules")) logf_("uid %u: %s", s->uid, why);
    free(text);
    fw_store_free(s->store);
    s->store = n;
    logf_("uid %u: %zu rules, mode %d", s->uid, fw_rule_count(n), (int)fw_mode(n));
}

/* (0.8.5) ~/.config/lordos/fwlists.txt, the user's subscribed blocklists compiled by lord-server
 * (fw/fwlists.h), through the same reader. A file that doesn't validate keeps the previous set. */
static void load_lists(Session *s) {
    size_t len = 0;
    char why[160] = "", err[120] = "";
    char *text = read_user_file(s->uid, "fwlists.txt", FWL_FILE_MAX, &len, why, sizeof why);
    if (!text) {
        if (strcmp(why, "no rules")) logf_("uid %u: blocklists: %s", s->uid, why);
        fwl_free(s->lists);
        s->lists = NULL;
        return;
    }
    FwLists *l = fwl_load_text(text, len, err, sizeof err);
    free(text);
    if (!l) { logf_("uid %u: invalid fwlists.txt (%s); the previous blocklists stay in use", s->uid, err); return; }
    fwl_free(s->lists);
    s->lists = fwl_domains(l) || fwl_nets(l) ? l : (fwl_free(l), NULL);
    if (s->lists) logf_("uid %u: blocklists: %zu names, %zu networks", s->uid, fwl_domains(s->lists), fwl_nets(s->lists));
    malloc_trim(0);                     /* the file's text and the set's growth: back to the system */
}

/* ---- maps ------------------------------------------------------------------------------------ */

#ifdef NP_HAVE_BPF                     /* the program loader's relocations (bpf_setup) */
static int map_fd(const char *name) {
    if (!strcmp(name, "np_cfg")) return G.m_np_cfg;
    if (!strcmp(name, "np_apps")) return G.m_np_apps;
    if (!strcmp(name, "np_union")) return G.m_np_union;
    if (!strcmp(name, "np_lpm4")) return G.m_np_lpm4;
    if (!strcmp(name, "np_lpm6")) return G.m_np_lpm6;
    if (!strcmp(name, "np_seen")) return G.m_np_seen;
    if (!strcmp(name, "np_counts")) return G.m_np_counts;
    if (!strcmp(name, "np_events")) return G.m_np_events;
    if (!strcmp(name, "np_snd")) return G.m_np_snd;
    if (!strcmp(name, "np_flows")) return G.m_np_flows;
    if (!strcmp(name, "np_bound")) return G.m_np_bound;
    if (!strcmp(name, "np_blk4")) return G.m_np_blk4;
    if (!strcmp(name, "np_blk6")) return G.m_np_blk6;
    if (!strcmp(name, "np_sysc")) return G.m_np_sysc;
    return -1;
}
#endif

static uint32_t idx_of(unsigned uid, const char *app) {
    char key[FW_APP_MAX + 16];
    int kl = snprintf(key, sizeof key, "%u:%s", uid, app);
    void **slot = fwmap_slot(&G.idxmap, key, (size_t)kl, 1);
    if (!slot) return 0;
    if (!*slot) *slot = (void *)(uintptr_t)++G.next_idx;
    return (uint32_t)(uintptr_t)*slot;
}

static size_t inst_key(uint32_t idx, const NpNode *n, unsigned char *k) {
    memcpy(k, &idx, 4);
    k[4] = (unsigned char)n->fam;
    k[5] = (unsigned char)n->prefix;
    memcpy(k + 6, n->addr, 16);
    return 22;
}

static int lpm_put(uint32_t idx, const NpNode *n) {
    if (n->fam == 4) { NpLpm4Key k; np_key4(idx, n, &k); return bpfs_map_update(G.m_np_lpm4, &k, &n->list, BPF_ANY); }
    NpLpm6Key k; np_key6(idx, n, &k); return bpfs_map_update(G.m_np_lpm6, &k, &n->list, BPF_ANY);
}

static void lpm_del(uint32_t idx, const unsigned char *ik) {
    NpNode n;
    memset(&n, 0, sizeof n);
    n.fam = ik[4];
    n.prefix = ik[5];
    memcpy(n.addr, ik + 6, 16);
    if (n.fam == 4) { NpLpm4Key k; np_key4(idx, &n, &k); bpfs_map_delete(G.m_np_lpm4, &k); }
    else { NpLpm6Key k; np_key6(idx, &n, &k); bpfs_map_delete(G.m_np_lpm6, &k); }
}

/* Installs a node if it changed. 0 ok. */
static int install(uint32_t idx, unsigned gen, const NpNode *n) {
    unsigned char k[22];
    size_t kl = inst_key(idx, n, k);
    void **slot = fwmap_slot(&G.installed, k, kl, 1);
    if (!slot) return -1;
    Installed *in = *slot;
    if (in && in->list.n == n->list.n && !memcmp(in->list.e, n->list.e, n->list.n * sizeof(NpEnt))) { in->gen = gen; return 0; }
    if (lpm_put(idx, n)) { logf_("a trie entry didn't fit: %s", strerror(errno)); return -1; }
    if (!in && !(in = *slot = malloc(sizeof *in))) return -1;
    in->idx = idx;
    in->gen = gen;
    in->list = n->list;
    return 0;
}

/* Deletes idx's nodes that aren't from generation gen (gen 0: all). */
static void prune(uint32_t idx, unsigned gen) {
    FWMAP_EACH(&G.installed, e) {
        Installed *in = e->val;
        if (in->idx != idx || (gen && in->gen == gen)) continue;
        lpm_del(idx, e->key);
        free(fwmap_take(&G.installed, e->key, e->klen));
    }
}

static FwStore *store_of(Session *s) { return s->store ? s->store : G.empty; }

static void sync_scope(Session *s, Scope *sc) {
    if (!G.bpf_ok) return;
    NpCompiled c;
    if (np_compile_app(store_of(s), sc->app, sc->idx, G.names, NAMES_APP, mono(), &c)) { logf_("out of memory compiling %s", sc->app); return; }
    if (c.truncated) logf_("uid %u %s: %d rules didn't fit in the maps (at most %d per network)", s->uid, sc->app, c.truncated, NP_LIST_MAX);
    unsigned gen = ++sc->gen ? sc->gen : ++sc->gen;
    for (int i = 0; i < c.nnodes; i++) install(sc->idx, gen, &c.nodes[i]);
    prune(sc->idx, gen);
    sc->has_domains = c.has_domains;
    /* the lists deny new connections in connect()/sendmsg(); an open connection to an address that
     * just joined a list is cut by the sweep after the change (sync_blk), so the per-packet filter
     * doesn't decide for every app because lists are on (it would cost every packet a decision) */
    if (s->lists) c.app.blk = s->blkset;
    sc->app_v = c.app;
    if (bpfs_map_update(G.m_np_apps, &sc->cgid, &c.app, BPF_ANY)) logf_("np_apps: %s", strerror(errno));
    /* the counters' entry exists from the start: the accounting programs only add to existing
     * entries, so they never store anything for a cgroup that isn't an app */
    NpCount zero = { 0, 0, 0, 0, 0 };
    bpfs_map_update(G.m_np_counts, &sc->cgid, &zero, BPF_NOEXIST);
    np_compiled_free(&c);
}

static void drop_scope(Scope *sc) {
    if (!G.bpf_ok) return;
    FWMAP_EACH(&G.flows, e) {                       /* its addresses' byte counters */
        const NpFlowKey *k = (const NpFlowKey *)(const void *)e->key;
        if (k->cgid != sc->cgid) continue;
        bpfs_map_delete(G.m_np_flows, k);
        free(fwmap_take(&G.flows, e->key, e->klen));
    }
    bpfs_map_delete(G.m_np_apps, &sc->cgid);
    bpfs_map_delete(G.m_np_counts, &sc->cgid);
    prune(sc->idx, 0);
}

/* ---- blocklists (0.8.5, docs/FIREWALL.md "Blocklists") --------------------------------------
 *
 * Each user with subscribed lists gets a set in np_blk4/np_blk6 (NpApp.blk of each of their apps):
 * the lists' networks, and the addresses the system resolver gave for a listed name, as long as
 * every name the resolver gave for that address is listed (a shared CDN address stays allowed).
 * The whole set is recomputed on a reload and every 30 s (what the resolver forgot goes); a DNS
 * answer updates its own addresses at once, before the app connects. The kernel decides with
 * np_decide: a rule the owner wrote for that destination still wins. */

static size_t blk_key(int fam, const uint8_t *addr, int prefix, unsigned char *k) {
    memset(k, 0, 18);
    k[0] = (unsigned char)fam;
    k[1] = (unsigned char)prefix;
    memcpy(k + 2, addr, fam == 4 ? 4 : 16);
    return 18;
}

static int blk_map(uint32_t set, int fam, const uint8_t *addr, int prefix, int del) {
    uint8_t one = 1;
    if (fam == 4) {
        NpLpm4Key k;
        memset(&k, 0, sizeof k);
        k.prefixlen = 32 + (uint32_t)prefix;
        k.data[0] = (uint8_t)(set >> 24); k.data[1] = (uint8_t)(set >> 16); k.data[2] = (uint8_t)(set >> 8); k.data[3] = (uint8_t)set;
        memcpy(k.data + 4, addr, 4);
        return del ? bpfs_map_delete(G.m_np_blk4, &k) : bpfs_map_update(G.m_np_blk4, &k, &one, BPF_ANY);
    }
    NpLpm6Key k;
    memset(&k, 0, sizeof k);
    k.prefixlen = 32 + (uint32_t)prefix;
    k.data[0] = (uint8_t)(set >> 24); k.data[1] = (uint8_t)(set >> 16); k.data[2] = (uint8_t)(set >> 8); k.data[3] = (uint8_t)set;
    memcpy(k.data + 4, addr, 16);
    return del ? bpfs_map_delete(G.m_np_blk6, &k) : bpfs_map_update(G.m_np_blk6, &k, &one, BPF_ANY);
}

static int blk_added;                   /* entries sync_blk installed (a sweep follows) */

static void blk_want(Session *s, int fam, const uint8_t *addr, int prefix, int learned) {
    unsigned char k[18];
    size_t kl = blk_key(fam, addr, prefix, k);
    void **slot = fwmap_slot(&s->blkin, k, kl, 1);
    if (!slot) return;
    BlkEnt *e = *slot;
    if (e) {
        e->mark = G.blk_pass;
        if (!learned && e->learned) { e->learned = 0; s->nlearned--; }
        return;
    }
    if ((learned && s->nlearned >= BLK_LEARNED_MAX) || blk_map(s->blkset, fam, addr, prefix, 0) || !(e = malloc(sizeof *e))) {
        fwmap_take(&s->blkin, k, kl);
        return;
    }
    e->learned = learned;
    e->mark = G.blk_pass;
    s->nlearned += learned;
    *slot = e;
    blk_added++;
}

/* An address the resolver now also gives for an unlisted name: no longer blocked. */
static void blk_unwant_learned(Session *s, int fam, const uint8_t *addr) {
    unsigned char k[18];
    size_t kl = blk_key(fam, addr, fam == 4 ? 32 : 128, k);
    BlkEnt *e = fwmap_get(&s->blkin, k, kl);
    if (!e || !e->learned) return;
    blk_map(s->blkset, fam, addr, fam == 4 ? 32 : 128, 1);
    free(fwmap_take(&s->blkin, k, kl));
    s->nlearned--;
}

static int blk_net_cb(void *u, int fam, const uint8_t *addr, int prefix) { blk_want(u, fam, addr, prefix, 0); return 0; }

static int blk_dns_cb(void *u, int fam, const uint8_t *addr, const char (*names)[FW_NAME_MAX], int n) {
    Session *s = u;
    const char *p[DNSMAP_PER_IP];
    for (int i = 0; i < n && i < DNSMAP_PER_IP; i++) p[i] = names[i];
    if (fwl_block_names(s->lists, p, n < DNSMAP_PER_IP ? n : DNSMAP_PER_IP)) blk_want(s, fam, addr, fam == 4 ? 32 : 128, 1);
    return 0;
}

static void sweep(unsigned uid);

static void sync_blk(Session *s) {
    if (!G.bpf_ok) return;
    G.blk_pass++;
    blk_added = 0;
    if (s->lists) {
        if (!s->blkset) s->blkset = ++G.next_blkset;
        fwl_net_each(s->lists, blk_net_cb, s);
        if (fwl_domains(s->lists)) dnsmap_each(G.names, NAMES_APP, mono(), blk_dns_cb, s);
    }
    FWMAP_EACH(&s->blkin, e) {
        BlkEnt *b = e->val;
        if (b->mark == G.blk_pass) continue;
        blk_map(s->blkset, e->key[0], e->key + 2, e->key[1], 1);
        s->nlearned -= b->learned;
        free(fwmap_take(&s->blkin, e->key, e->klen));
    }
    if (blk_added) sweep(s->uid);       /* open connections to what was just added */
}

/* ---- user@<uid>.service: the systemd-run escape ------------------------------------------- */

/* An app without a sandbox, running as the user, can ask its systemd manager
 * (systemd-run --user, scope or service) to move a process into user@<uid>.service, outside
 * lordos.service, where the apps' scopes don't reach. To close that, the same programs are also
 * attached to each session's user@<uid>.service and, when the process isn't in any app's scope,
 * it falls into np_union[user@]: the strict union (OR of the NP_R_* restrictions) of the user's
 * restricted apps. That way an escaped process is never less restricted than its own policy; it
 * grants nothing. If no restricted app is running, there's no entry and user@ stays free (the
 * user manager and its services aren't touched). */

/* Recompiles the user's union from the flags of their scopes and writes it (or deletes it if
 * there's no restriction). Only takes effect with user@ attached. */
static void sync_union(Session *s) {
    if (!G.bpf_ok || !s->user_cg) return;
    NpApp u;
    memset(&u, 0, sizeof u);
    /* "no microphone" stays out of the union: under user@ it would deny the ALSA capture nodes
     * to the user's own PipeWire, which then drops the microphone for every app (and doesn't get
     * it back). A process escaped there is held by the PipeWire guard instead (its clients count
     * as restricted while restricted apps run); reading the card directly from there is the
     * residual risk (docs/FIREWALL.md). */
    for (int k = 0; k < s->nsc; k++) u.flags |= s->sc[k].app_v.flags & ~NP_R_NO_MIC;
    if (u.flags) {
        u.trusted = 1;                  /* whatever the union doesn't restrict is allowed without notice */
        u.mode = NP_M_SILENT;
        if (bpfs_map_update(G.m_np_union, &s->user_cg, &u, BPF_ANY)) logf_("np_union: %s", strerror(errno));
    } else {
        bpfs_map_delete(G.m_np_union, &s->user_cg);
    }
}

static void drop_user_links(Session *s) {
    for (int i = 0; i < s->nulinks; i++) close(s->ulinks[i]);
    s->nulinks = 0;
}

/* Attaches (or re-attaches) the programs to user@<uid>.service. systemd creates it at login and
 * deletes and recreates it with another id when the user manager restarts, so this is called on
 * every pass: if the id changed (or disappeared), it drops the old links and the old union. */
static void sync_user_scope(Session *s) {
    if (!G.bpf_ok) return;
    char path[200];
    snprintf(path, sizeof path, CG_ROOT "/" CG_USER "/user-%u.slice", s->uid);
    char leaf[64];
    snprintf(leaf, sizeof leaf, "user@%u.service", s->uid);
    uint64_t id = 0;
    int ufd = -1, sfd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (sfd >= 0) {
        id = bpfs_cgroup_id(sfd, leaf);
        if (id) ufd = openat(sfd, leaf, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        close(sfd);
    }
    if (id == s->user_cg) { if (ufd >= 0) close(ufd); return; }
    drop_user_links(s);
    if (s->user_cg) { bpfs_map_delete(G.m_np_union, &s->user_cg); bpfs_map_delete(G.m_np_counts, &s->user_cg); }
    s->user_cg = 0;
    if (!id || ufd < 0) { if (ufd >= 0) close(ufd); return; }
    for (int i = 0; i < G.nprogs && s->nulinks < (int)(sizeof s->ulinks / sizeof s->ulinks[0]); i++) {
        if (!np_attach_user(G.attach[i])) continue;             /* accounting: only the app scopes */
        int l = bpfs_link_create(G.progs[i], ufd, G.attach[i]);
        if (l < 0) { logf_("uid %u: could not attach to user@: %s", s->uid, strerror(errno)); drop_user_links(s); close(ufd); return; }
        s->ulinks[s->nulinks++] = l;
    }
    close(ufd);
    s->user_cg = id;
    logf_("uid %u: %d programs attached to user@%u.service", s->uid, s->nulinks, s->uid);
    sync_union(s);
}

/* ---- sessions and scopes --------------------------------------------------------------------- */

static int scope_app(const char *name, char *app) {
    if (!strcmp(name, "server")) { snprintf(app, FW_APP_MAX, "lordos"); return 0; }
    size_t n = strlen(name);
    /* (0.8.5) LordOS's own components that use the network: lordos-net-<name>.scope → lordos:<name> */
    if (!strncmp(name, "lordos-net-", 11)) return fw_component_scope(name, app, FW_APP_MAX);
    if (n < 18 || strncmp(name, "app-lordos-", 11) || strcmp(name + n - 6, ".scope")) return -1;
    return fw_app_canon(name, app, FW_APP_MAX);
}

static void scan_session(Session *s) {
    char path[160];
    snprintf(path, sizeof path, CG_ROOT "/" CG_SERVICE "/%s", s->dir);
    int dfd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd < 0) return;
    DIR *d = fdopendir(dup(dfd));
    for (int i = 0; i < s->nsc; i++) s->sc[i].seen = 0;
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (e->d_type != DT_DIR || e->d_name[0] == '.' || strlen(e->d_name) >= sizeof s->sc[0].name) continue;
        char app[FW_APP_MAX];
        if (scope_app(e->d_name, app)) continue;
        uint64_t id = bpfs_cgroup_id(dfd, e->d_name);
        if (!id) continue;
        Scope *sc = NULL;
        for (int i = 0; i < s->nsc; i++) if (s->sc[i].cgid == id && !strcmp(s->sc[i].name, e->d_name)) sc = &s->sc[i];
        if (!sc) {
            if (s->nsc == s->cap) {
                int nc = s->cap ? s->cap * 2 : 8;
                Scope *ns = realloc(s->sc, (size_t)nc * sizeof *ns);
                if (!ns) break;
                s->sc = ns;
                s->cap = nc;
            }
            sc = &s->sc[s->nsc++];
            memset(sc, 0, sizeof *sc);
            snprintf(sc->name, sizeof sc->name, "%s", e->d_name);
            snprintf(sc->app, sizeof sc->app, "%s", app);
            sc->cgid = id;
            sc->idx = idx_of(s->uid, app);
            sync_scope(s, sc);
        }
        sc->seen = 1;
    }
    if (d) closedir(d);
    close(dfd);
    for (int i = 0; i < s->nsc;) {
        if (s->sc[i].seen) { i++; continue; }
        drop_scope(&s->sc[i]);
        s->sc[i] = s->sc[--s->nsc];
    }
    sync_user_scope(s);                 /* attaches/re-attaches user@ and recompiles the union */
    sync_union(s);
}

static void drop_session(int k) {
    Session *s = &G.sess[k];
    for (int i = 0; i < s->nsc; i++) drop_scope(&s->sc[i]);
    fwl_free(s->lists);
    s->lists = NULL;
    sync_blk(s);                        /* with no lists: removes every entry */
    fwmap_free(&s->blkin, free);
    drop_user_links(s);
    if (s->user_cg) { bpfs_map_delete(G.m_np_union, &s->user_cg); bpfs_map_delete(G.m_np_counts, &s->user_cg); }
    if (s->wd >= 0) inotify_rm_watch(G.ifd, s->wd);
    free(s->sc);
    fw_store_free(s->store);
    G.sess[k] = G.sess[--G.nsess];
}

/* Every ALSA node (/sys/class/sound/<name>/dev, "116:<minor>"), capture PCMs (pcmC*D*c) marked,
 * for "no microphone" against apps that read the sound card directly instead of through PipeWire.
 * The program denies a no-mic app's own scope every ALSA minor not marked NP_SND_OTHER, so a node
 * that appears between two scans (a USB microphone plugged in) is denied until it is known: fail
 * closed. Rescanned with the sessions. Only sysfs: no device node is opened. */
static int capture_pcm(const char *name) {
    size_t l = strlen(name);
    return !strncmp(name, "pcmC", 4) && l >= 6 && name[l - 1] == 'c';
}

static void scan_snd(void) {
    if (!G.bpf_ok) return;
    static uint32_t now[NP_MAX_SND], kind[NP_MAX_SND];
    int n = 0;
    DIR *d = opendir("/sys/class/sound");
    struct dirent *e;
    while (d && (e = readdir(d)) && n < NP_MAX_SND) {
        if (e->d_name[0] == '.' || strlen(e->d_name) > 64) continue;
        char p[300], buf[32];
        snprintf(p, sizeof p, "/sys/class/sound/%s/dev", e->d_name);
        int fd = open(p, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) continue;
        ssize_t k = read(fd, buf, sizeof buf - 1);
        close(fd);
        unsigned ma, mi;
        if (k > 0 && (buf[k] = 0, sscanf(buf, "%u:%u", &ma, &mi) == 2) && ma == NP_MAJOR_SND) {
            now[n] = mi;
            kind[n++] = capture_pcm(e->d_name) ? NP_SND_CAPTURE : NP_SND_OTHER;
        }
    }
    if (d) closedir(d);
    for (int k = 0; k < n; k++) bpfs_map_update(G.m_np_snd, &now[k], &kind[k], BPF_ANY);
    for (int i = 0; i < G.nsnd; i++) {
        int keep = 0;
        for (int k = 0; k < n && !keep; k++) keep = now[k] == G.snd[i];
        if (!keep) bpfs_map_delete(G.m_np_snd, &G.snd[i]);
    }
    memcpy(G.snd, now, sizeof(uint32_t) * (size_t)n);
    G.nsnd = n;
}

static void scan_all(void) {
    scan_snd();
    DIR *d = opendir(CG_ROOT "/" CG_SERVICE);
    for (int i = 0; i < G.nsess; i++) G.sess[i].seen = 0;
    struct dirent *e;
    while (d && (e = readdir(d))) {
        unsigned uid;
        char tail;
        if (e->d_type != DT_DIR || sscanf(e->d_name, "lordos-session-%u%c", &uid, &tail) != 1) continue;
        Session *s = NULL;
        for (int i = 0; i < G.nsess; i++) if (G.sess[i].uid == uid) s = &G.sess[i];
        if (!s) {
            if (G.nsess == MAX_SESS) continue;
            s = &G.sess[G.nsess++];
            memset(s, 0, sizeof *s);
            s->uid = uid;
            snprintf(s->dir, sizeof s->dir, "lordos-session-%u", uid);
            char path[160];
            snprintf(path, sizeof path, CG_ROOT "/" CG_SERVICE "/%s", s->dir);
            s->wd = inotify_add_watch(G.ifd, path, IN_CREATE | IN_DELETE | IN_ONLYDIR);
            load_rules(s);
            load_lists(s);
            sync_blk(s);
        }
        s->seen = 1;
        /* a session that ended and started again has a new folder under the same name: watch the
         * one there now (for the same folder this returns the same watch), or a new scope would only
         * be found by the 5-second rescan (0.8.5: a component's scope connects right away) */
        {
            char path[160];
            snprintf(path, sizeof path, CG_ROOT "/" CG_SERVICE "/%s", s->dir);
            int wd = inotify_add_watch(G.ifd, path, IN_CREATE | IN_DELETE | IN_ONLYDIR);
            if (wd >= 0) s->wd = wd;
        }
        scan_session(s);
    }
    if (d) closedir(d);
    for (int i = 0; i < G.nsess;) {
        if (G.sess[i].seen) i++;
        else drop_session(i);
    }
}

static Scope *scope_by_cgid(uint64_t id, Session **out) {
    for (int i = 0; i < G.nsess; i++)
        for (int k = 0; k < G.sess[i].nsc; k++)
            if (G.sess[i].sc[k].cgid == id) { *out = &G.sess[i]; return &G.sess[i].sc[k]; }
    return NULL;
}

static Session *session_by_user_cg(uint64_t id) {
    for (int i = 0; i < G.nsess; i++) if (G.sess[i].user_cg && G.sess[i].user_cg == id) return &G.sess[i];
    return NULL;
}

/* ---- clients --------------------------------------------------------------------------------- */

static void cl_drop(int k) {
    epoll_ctl(G.ep, EPOLL_CTL_DEL, G.cl[k].fd, NULL);
    close(G.cl[k].fd);
    free(G.cl[k].out);
    G.cl[k] = G.cl[--G.ncl];
}

static void cl_flush(Client *c) {
    while (c->outlen) {
        ssize_t n = send(c->fd, c->out, c->outlen, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n <= 0) break;
        memmove(c->out, c->out + n, c->outlen - (size_t)n);
        c->outlen -= (size_t)n;
    }
    struct epoll_event ev = { .events = EPOLLIN | (c->outlen ? EPOLLOUT : 0), .data.u64 = 1000 + (uint64_t)c->fd };
    epoll_ctl(G.ep, EPOLL_CTL_MOD, c->fd, &ev);
}

static void cl_send(Client *c, int type, const char *json, size_t n, int droppable) {
    if (!c->out && !(c->out = malloc(CLIENT_OUT))) return;
    if (c->outlen + n + 5 > CLIENT_OUT) { (void)droppable; return; }   /* nobody is reading: notices are lost (the counters go out again when they change) */
    uint32_t l = (uint32_t)n + 1;
    unsigned char h[5] = { (unsigned char)l, (unsigned char)(l >> 8), (unsigned char)(l >> 16), (unsigned char)(l >> 24), (unsigned char)type };
    memcpy(c->out + c->outlen, h, 5);
    memcpy(c->out + c->outlen + 5, json, n);
    c->outlen += n + 5;
    cl_flush(c);
}

static void send_uid(unsigned uid, int type, const char *json, size_t n, int droppable) {
    for (int i = 0; i < G.ncl; i++) if (G.cl[i].uid == uid) cl_send(&G.cl[i], type, json, n, droppable);
}

static void status_json(char *b, size_t n) {
    char err[260];
    json_quote(err, sizeof err, G.bpf_ok ? "" : G.bpf_err);
    snprintf(b, n, "{\"bpf\":%s,\"devices\":%s,\"bluetooth\":%s,\"bytes\":%s,\"dns\":\"%s\",\"error\":%s}", G.bpf_ok ? "true" : "false",
             G.bpf_ok && G.dev_ok ? "true" : "false", G.bpf_ok && G.bt_ok ? "true" : "false", G.bpf_ok && G.acct_ok == 2 ? "true" : "false",
             G.dns ? npdns_source(G.dns) : "none", err);
}

static void accept_client(void) {
    for (;;) {
        int fd = accept4(G.lfd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (fd < 0) return;
        struct ucred cr;
        socklen_t cl = sizeof cr;
        if (G.ncl == MAX_CLIENTS || getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &cl)) { close(fd); continue; }
        int per = 0;
        for (int i = 0; i < G.ncl; i++) per += G.cl[i].uid == cr.uid;
        if (per >= 4) { close(fd); continue; }               /* up to 4 per user */
        Client *c = &G.cl[G.ncl++];
        memset(c, 0, sizeof *c);
        c->fd = fd;
        c->uid = cr.uid;
        struct epoll_event ev = { .events = EPOLLIN, .data.u64 = 1000 + (uint64_t)fd };
        epoll_ctl(G.ep, EPOLL_CTL_ADD, fd, &ev);
        char st[400];
        status_json(st, sizeof st);
        cl_send(c, NP_MSG_STATUS, st, strlen(st), 0);
        for (int i = 0; i < G.nsess; i++) if (G.sess[i].uid == cr.uid) G.sess[i].dirty_counts = 1;
    }
}

static void sweep(unsigned uid);

static void reload_uid(unsigned uid) {
    for (int i = 0; i < G.nsess; i++) {
        if (G.sess[i].uid != uid) continue;
        load_rules(&G.sess[i]);
        load_lists(&G.sess[i]);
        sync_blk(&G.sess[i]);
        for (int k = 0; k < G.sess[i].nsc; k++) sync_scope(&G.sess[i], &G.sess[i].sc[k]);
        sync_union(&G.sess[i]);
    }
    sweep(uid);                         /* the open connections the new rules deny */
}

static void client_read(int k) {
    Client *c = &G.cl[k];
    for (;;) {
        ssize_t n = recv(c->fd, c->in + c->inlen, sizeof c->in - c->inlen, MSG_DONTWAIT);
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) { cl_drop(k); return; }
        if (n < 0) return;
        c->inlen += (size_t)n;
        while (c->inlen >= 5) {
            uint32_t l = (uint32_t)c->in[0] | (uint32_t)c->in[1] << 8 | (uint32_t)c->in[2] << 16 | (uint32_t)c->in[3] << 24;
            if (l != 1 || c->in[4] != NP_MSG_RELOAD) { cl_drop(k); return; }   /* the only thing accepted */
            reload_uid(c->uid);
            memmove(c->in, c->in + 5, c->inlen - 5);
            c->inlen -= 5;
        }
        if (c->inlen == sizeof c->in) { cl_drop(k); return; }
    }
}

/* ---- cutting the connections a rule now denies (docs/FIREWALL.md, "Existing connections are cut") ----
 *
 * The packet filter (np.bpf.c, cgroup_skb) already drops every packet of a connection the app's
 * current rules deny, the moment the maps change. This side destroys the sockets too, so the app
 * sees the connection end (ECONNABORTED, and TCP sends a reset) instead of a stall: right after a
 * reload (a sweep of the machine's TCP and connected UDP sockets, by cgroup) and whenever the
 * filter reports a socket it dropped a packet of (one that wasn't there in the sweep, or a domain
 * whose address was learned later). Only sockets in an app scope of that user, only those the
 * same np_decide denies. */

static int diag_fd(void) {
    if (G.diag_fd < 0 && (G.diag_fd = npdiag_open()) < 0 && !G.diag_warned) {
        logf_("sock_diag: %s: open connections are cut only by dropping their packets", strerror(errno));
        G.diag_warned = 1;
    }
    return G.diag_fd;
}

static int cut_sock(const NpSock *k) {
    int fd = diag_fd();
    if (fd < 0) return -ENOTSOCK;
    int rc = npdiag_destroy(fd, k);
    if (rc == -EAGAIN || rc == -EINTR || rc == -EIO) {         /* a lost answer: start over with a new socket */
        close(G.diag_fd);
        G.diag_fd = -1;
    }
    if ((rc == -EOPNOTSUPP || rc == -EPERM) && !G.diag_warned) {
        logf_("the kernel can't destroy %s sockets (%s; CONFIG_INET_DIAG_DESTROY): their packets are dropped, the app sees a stall",
              k->proto == IPPROTO_TCP ? "TCP" : "UDP", strerror(-rc));
        G.diag_warned = 1;
    }
    return rc;
}

/* The remote address of a socket as the rules see it: an IPv4-mapped IPv6 one is IPv4. */
static int remote_of(const NpSock *k, uint8_t *a) {
    memset(a, 0, 16);
    if (k->fam == AF_INET) { memcpy(a, k->dst, 4); return 4; }
    static const uint8_t mapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };
    if (!memcmp(k->dst, mapped, 12)) { memcpy(a, k->dst + 12, 4); return 4; }
    memcpy(a, k->dst, 16);
    return 6;
}

/* The same decision as the packet filter, from userspace: the app's NpApp, its trie list for the
 * address and whether the local port is one it bound (an incoming connection). 1 if denied. */
static int user_denies(const Scope *sc, const NpSock *k, int *fam, uint8_t *addr, int *dir, int *port, NpRes *r) {
    const NpApp *a = &sc->app_v;
    if (!a->can_deny && !a->blk) return 0;
    *fam = remote_of(k, addr);
    int proto = k->proto == IPPROTO_TCP ? NP_P_TCP : NP_P_UDP, lan;
    NpList l;
    int have = 0;
    if (*fam == 4) {
        lan = np_is_lan4(addr);
        NpLpm4Key key;
        memset(&key, 0, sizeof key);
        key.prefixlen = 64;
        key.data[0] = (uint8_t)(a->idx >> 24); key.data[1] = (uint8_t)(a->idx >> 16); key.data[2] = (uint8_t)(a->idx >> 8); key.data[3] = (uint8_t)a->idx;
        memcpy(key.data + 4, addr, 4);
        have = a->has4 && !bpfs_map_lookup(G.m_np_lpm4, &key, &l);
    } else {
        lan = np_is_lan6(addr);
        NpLpm6Key key;
        memset(&key, 0, sizeof key);
        key.prefixlen = 160;
        key.data[0] = (uint8_t)(a->idx >> 24); key.data[1] = (uint8_t)(a->idx >> 16); key.data[2] = (uint8_t)(a->idx >> 8); key.data[3] = (uint8_t)a->idx;
        memcpy(key.data + 4, addr, 16);
        have = a->has6 && !bpfs_map_lookup(G.m_np_lpm6, &key, &l);
    }
    NpBoundKey bk;
    memset(&bk, 0, sizeof bk);
    bk.cgid = sc->cgid;
    bk.port = k->sport;
    bk.proto = (np_u8)proto;
    uint8_t one;
    *dir = NP_D_OUT;
    *port = k->dport;
    if (!bpfs_map_lookup(G.m_np_bound, &bk, &one)) { *dir = NP_D_IN; *port = k->sport; }
    int listed = 0;
    if (a->blk) {
        uint8_t v;
        if (*fam == 4) {
            NpLpm4Key key;
            memset(&key, 0, sizeof key);
            key.prefixlen = 64;
            key.data[0] = (uint8_t)(a->blk >> 24); key.data[1] = (uint8_t)(a->blk >> 16); key.data[2] = (uint8_t)(a->blk >> 8); key.data[3] = (uint8_t)a->blk;
            memcpy(key.data + 4, addr, 4);
            listed = !bpfs_map_lookup(G.m_np_blk4, &key, &v);
        } else {
            NpLpm6Key key;
            memset(&key, 0, sizeof key);
            key.prefixlen = 160;
            key.data[0] = (uint8_t)(a->blk >> 24); key.data[1] = (uint8_t)(a->blk >> 16); key.data[2] = (uint8_t)(a->blk >> 8); key.data[3] = (uint8_t)a->blk;
            memcpy(key.data + 4, addr, 16);
            listed = !bpfs_map_lookup(G.m_np_blk6, &key, &v);
        }
    }
    np_decide(a, have ? &l : NULL, *dir, proto, *port, lan, listed, np_is_loop(*fam, addr), r);
    return r->verdict == NP_V_DENY;
}

typedef struct {
    NpSock k;
    NpEvent ev;
} CutReq;

typedef struct {
    unsigned uid;                       /* only this user's apps; (unsigned)-1: everyone's */
    CutReq *v;
    int n, cap;
} Sweep;

static int sweep_cb(void *u, const NpSock *k) {
    Sweep *w = u;
    Session *s;
    Scope *sc = k->cgid ? scope_by_cgid(k->cgid, &s) : NULL;
    if (!sc || (w->uid != (unsigned)-1 && s->uid != w->uid)) return 0;
    int proto = k->proto == IPPROTO_TCP ? NP_P_TCP : NP_P_UDP;
    if (k->proto == IPPROTO_TCP && k->state == 10) {           /* TCP_LISTEN: the app's server (also after a restart of this service) */
        NpBoundKey bk;
        memset(&bk, 0, sizeof bk);
        bk.cgid = sc->cgid;
        bk.port = k->sport;
        bk.proto = (np_u8)proto;
        uint8_t one = 1;
        bpfs_map_update(G.m_np_bound, &bk, &one, BPF_ANY);
        return 0;
    }
    if (!k->dport) return 0;                                   /* not connected */
    CutReq q;
    memset(&q, 0, sizeof q);
    int fam, dir, port;
    NpRes r;
    if (!user_denies(sc, k, &fam, q.ev.addr, &dir, &port, &r)) return 0;
    if (w->n == w->cap) {
        int nc = w->cap ? w->cap * 2 : 32;
        CutReq *nv = realloc(w->v, (size_t)nc * sizeof *nv);
        if (!nv) return 1;
        w->v = nv;
        w->cap = nc;
    }
    q.k = *k;
    q.ev.cgid = sc->cgid;
    q.ev.port = (np_u16)port;
    q.ev.fam = (np_u8)fam; q.ev.proto = (np_u8)proto; q.ev.dir = (np_u8)dir; q.ev.verdict = NP_V_DENY;
    q.ev.reason = (np_u8)r.reason; q.ev.record = (np_u8)r.record; q.ev.rule_id = r.rule_id; q.ev.flag = r.flag;
    w->v[w->n++] = q;
    return 0;
}

static void on_net_event(const NpEvent *ev, int cut);

/* Destroys the open sockets of uid's apps (or everyone's) that their rules now deny, and learns
 * the ports their servers listen on. */
static void sweep(unsigned uid) {
    if (!G.bpf_ok) return;
    int fd = diag_fd();
    if (fd < 0) return;
    Sweep w = { uid, NULL, 0, 0 };
    /* TCP: every state but TIME_WAIT (6), CLOSE (7) and the request sockets (3, 12); UDP: connected (1) */
    uint32_t tcp_states = ~((1u << 3) | (1u << 6) | (1u << 7) | (1u << 12)), udp_states = 1u << 1;
    static const int fams[2] = { AF_INET, AF_INET6 };
    for (int f = 0; f < 2; f++) {
        int rc = npdiag_each(fd, fams[f], IPPROTO_TCP, tcp_states, sweep_cb, &w);
        if (rc >= 0) rc = npdiag_each(fd, fams[f], IPPROTO_UDP, udp_states, sweep_cb, &w);
        if (rc < 0 && rc != -ENOENT) {
            logf_("sock_diag dump: %s", strerror(-rc));
            close(G.diag_fd);
            G.diag_fd = -1;
            break;
        }
    }
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);                        /* the clock of bpf_ktime_get_ns */
    uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    for (int i = 0; i < w.n; i++) {
        if (cut_sock(&w.v[i].k)) continue;                      /* gone already, or the kernel can't: the drop holds */
        /* the filter may have dropped one of its packets in the meantime and counted it already
         * (np_seen by cookie, as report_cut keys it); otherwise mark it so it isn't counted twice */
        NpSeenKey sk;
        memset(&sk, 0, sizeof sk);
        sk.cgid = w.v[i].ev.cgid;
        uint64_t cookie = (uint64_t)w.v[i].k.cookie[0] | (uint64_t)w.v[i].k.cookie[1] << 32;
        for (int b = 0; b < 8; b++) sk.addr[b] = (uint8_t)(cookie >> (b * 8));
        sk.dir = NP_D_CUT;
        sk.verdict = 1;
        uint64_t last;
        if (!bpfs_map_lookup(G.m_np_seen, &sk, &last)) continue;
        bpfs_map_update(G.m_np_seen, &sk, &now, BPF_ANY);
        Session *s;
        Scope *sc = scope_by_cgid(w.v[i].ev.cgid, &s);
        if (sc) { sc->cuts++; s->dirty_counts = 1; }
        on_net_event(&w.v[i].ev, 1);
    }
    free(w.v);
}

/* ---- notices and counters -------------------------------------------------------------------- */

static const char *by_name(int r) {
    return r == NP_BY_RESTRICTED ? "restricted" : r == NP_BY_RULE ? "rule" : r == NP_BY_TRUSTED ? "trusted" : r == NP_BY_LIST ? "list" : "mode";
}

static void on_event(void *u, const void *data, uint32_t len) {
    (void)u;
    if (len < sizeof(NpEvent)) return;
    NpEvent ev;
    memcpy(&ev, data, sizeof ev);
    Session *s;
    Scope *sc = scope_by_cgid(ev.cgid, &s);
    if (ev.dir == NP_D_DEV || ev.dir == NP_D_BT) {
        /* a device open or a Bluetooth socket: only in the journal (the Monitor's counters and
         * connection log are about the network) */
        Session *us = sc ? s : session_by_user_cg(ev.cgid);
        unsigned minor = (unsigned)ev.addr[0] | (unsigned)ev.addr[1] << 8 | (unsigned)ev.addr[2] << 16 | (unsigned)ev.addr[3] << 24;
        if (!us) return;
        if (ev.dir == NP_D_DEV)
            logf_("uid %u %s: denied opening device %u:%u (%s)", us->uid, sc ? sc->app : "user@ (union of restricted apps)", ev.port, minor, fw_flag_name(ev.flag));
        else
            logf_("uid %u %s: denied a Bluetooth socket (%s)", us->uid, sc ? sc->app : "user@ (union of restricted apps)", fw_flag_name(ev.flag));
        return;
    }
    if (ev.cut) {
        /* the packet filter dropped a packet of an open socket the rules now deny: destroy it */
        int cut = 0;
        if (ev.destroy && sc && (ev.fam == 4 || ev.fam == 6) && (ev.proto == NP_P_TCP || ev.proto == NP_P_UDP)) {
            NpSock k;
            memset(&k, 0, sizeof k);
            k.fam = ev.fam == 4 ? AF_INET : AF_INET6;
            k.proto = ev.proto == NP_P_TCP ? IPPROTO_TCP : IPPROTO_UDP;
            memcpy(k.src, ev.laddr, 16);
            memcpy(k.dst, ev.addr, 16);
            k.sport = ev.lport;
            k.dport = ev.rport;
            k.cookie[0] = (uint32_t)ev.cookie;
            k.cookie[1] = (uint32_t)(ev.cookie >> 32);
            cut = cut_sock(&k) == 0;
        }
        if (ev.cut != 1) return;                    /* asked again: already counted and reported */
        on_net_event(&ev, cut);
        return;
    }
    on_net_event(&ev, 0);
}

/* A connection notice (new, denied or cut) towards the user's lord-server and the journal. */
static void on_net_event(const NpEvent *evp, int cut) {
    NpEvent ev = *evp;
    Session *s;
    Scope *sc = scope_by_cgid(ev.cgid, &s);
    char addr[INET6_ADDRSTRLEN] = "";
    if (ev.fam == 4) inet_ntop(AF_INET, ev.addr, addr, sizeof addr);
    else if (ev.fam == 6) inet_ntop(AF_INET6, ev.addr, addr, sizeof addr);
    if (!sc) {
        /* the user@ union: a process that escaped with systemd-run --user (no app scope) */
        Session *us = session_by_user_cg(ev.cgid);
        if (us && ev.verdict == NP_V_DENY)
            logf_("uid %u user@: denied %s %s %s:%u (union of restricted apps%s%s)", us->uid,
                  ev.dir == NP_D_IN ? "in" : "out", ev.proto == NP_P_TCP ? "tcp" : ev.proto == NP_P_UDP ? "udp" : "other",
                  addr[0] ? addr : "?", ev.port, ev.flag ? " " : "", ev.flag ? fw_flag_name(ev.flag) : "");
        return;
    }
    const char *flag = ev.flag ? fw_flag_name(ev.flag) : "";
    /* the names the system resolver gave for that address (at most 4, so the Monitor can offer
     * "block this domain" with the real name) */
    char hosts[4 * (FW_NAME_MAX * 2 + 4) + 4] = "[]";
    if (ev.fam == 4 || ev.fam == 6) {
        char names[4][FW_NAME_MAX];
        int nh = dnsmap_lookup(G.names, NAMES_APP, ev.fam, ev.addr, mono(), names, 4);
        size_t w = 0;
        hosts[w++] = '[';
        for (int i = 0; i < nh; i++) {
            char q[FW_NAME_MAX * 2 + 4];
            json_quote(q, sizeof q, names[i]);
            w += (size_t)snprintf(hosts + w, sizeof hosts - w, "%s%s", i ? "," : "", q);
        }
        snprintf(hosts + w, sizeof hosts - w, "]");
    }
    char app[FW_APP_MAX * 2 + 4], j[700 + sizeof hosts];
    json_quote(app, sizeof app, sc->app);
    int n = snprintf(j, sizeof j, "{\"app\":%s,\"v\":\"%s\",\"dir\":\"%s\",\"proto\":\"%s\",\"addr\":\"%s\",\"port\":%u,\"by\":\"%s\",\"rule\":%u,\"flag\":\"%s\",\"record\":%d,\"hosts\":%s}",
                     app, ev.verdict == NP_V_DENY ? "deny" : "allow", ev.dir == NP_D_IN ? "in" : "out",
                     ev.proto == NP_P_TCP ? "tcp" : ev.proto == NP_P_UDP ? "udp" : "other", addr, ev.port, by_name(ev.reason),
                     ev.rule_id, flag ? flag : "", ev.record, hosts);
    if (n > 0 && (size_t)n < sizeof j) send_uid(s->uid, NP_MSG_EVENT, j, (size_t)n, 1);
    if (ev.verdict == NP_V_DENY)
        logf_("uid %u %s: %s %s %s %s:%u (%s%s%s)", s->uid, sc->app, cut ? "cut open" : ev.cut ? "dropping open" : "denied", ev.dir == NP_D_IN ? "in" : "out",
              ev.proto == NP_P_TCP ? "tcp" : ev.proto == NP_P_UDP ? "udp" : "other", addr[0] ? addr : "?", ev.port, by_name(ev.reason),
              ev.flag ? " " : "", ev.flag && flag ? flag : "");
}

/* ---- the essential system services (0.8.5) ----------------------------------------------------
 *
 * Name resolution, time and the network service are not apps and can't be blocked, but the Monitor
 * shows their traffic: the accounting programs alone are attached to resolved's and timesyncd's
 * cgroups (lord-netd's leaf, lordos.service/netd, is already under lordos.service), and np_sysc
 * gets an entry for each, keyed by its cgroup id (the programs only add to existing entries). */
static const struct { const char *id, *parent, *leaf; } SYS[3] = {
    { "system:dns", CG_ROOT "/system.slice", "systemd-resolved.service" },
    { "system:time", CG_ROOT "/system.slice", "systemd-timesyncd.service" },
    { "system:network", CG_ROOT "/" CG_SERVICE, "netd" },
};

static void sync_system(void) {
    if (!G.bpf_ok || G.acct_ok != 2) return;
    for (int i = 0; i < 3; i++) {
        int dfd = open(SYS[i].parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        uint64_t id = dfd >= 0 ? bpfs_cgroup_id(dfd, SYS[i].leaf) : 0;
        if (id == G.sys[i].cgid) { if (dfd >= 0) close(dfd); continue; }
        for (int k = 0; k < G.sys[i].nlinks; k++) close(G.sys[i].links[k]);
        if (G.sys[i].cgid) bpfs_map_delete(G.m_np_sysc, &G.sys[i].cgid);
        memset(&G.sys[i], 0, sizeof G.sys[i]);
        if (!id) { if (dfd >= 0) close(dfd); continue; }
        NpCount z = { 0, 0, 0, 0, 0 };
        bpfs_map_update(G.m_np_sysc, &id, &z, BPF_ANY);
        if (strcmp(SYS[i].leaf, "netd")) {
            int cfd = openat(dfd, SYS[i].leaf, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            for (int p = 0; cfd >= 0 && p < G.nprogs && G.sys[i].nlinks < 4; p++) {
                if (!np_attach_system(G.attach[p])) continue;
                int l = bpfs_link_create(G.progs[p], cfd, G.attach[p]);
                if (l < 0) { logf_("%s: could not attach the accounting: %s", SYS[i].leaf, strerror(errno)); break; }
                G.sys[i].links[G.sys[i].nlinks++] = l;
            }
            if (cfd >= 0) close(cfd);
        }
        close(dfd);
        G.sys[i].cgid = id;
        logf_("counting %s (%s)", SYS[i].id, SYS[i].leaf);
    }
}

/* ---- traffic per address (np_flows) ------------------------------------------------------- */

typedef struct {
    uint64_t rx, tx, blocked;           /* the last reading */
    double rrx, rtx;                    /* bytes per second over the last tick */
    long long active;                   /* tick of the last change */
    long long tick;                     /* tick of the last reading that saw it */
} FlowSt;

#define FLOW_KEEP_TICKS 120             /* an address idle for 2 min stops being sent (its bytes stay in the app's total) */
#define FLOWS_PER_APP   64              /* the busiest addresses of each app, per COUNTS message */

typedef struct { const NpFlowKey *k; const FlowSt *f; } FlowRef;

static int flow_cmp(const void *a, const void *b) {
    const FlowSt *x = ((const FlowRef *)a)->f, *y = ((const FlowRef *)b)->f;
    double rx = x->rrx + x->rtx, ry = y->rrx + y->rtx;
    if (rx != ry) return rx < ry ? 1 : -1;
    uint64_t bx = x->rx + x->tx, by = y->rx + y->tx;
    return bx < by ? 1 : bx > by ? -1 : 0;
}

/* Rereads np_flows (one batch call) and updates each address's rate. dt in seconds. */
static void read_flows(double dt) {
    static NpFlowKey keys[NP_MAX_FLOWS];
    static NpFlow vals[NP_MAX_FLOWS];
    int n = bpfs_map_dump(G.m_np_flows, keys, sizeof keys[0], vals, sizeof vals[0], NP_MAX_FLOWS);
    for (int i = 0; i < n; i++) {
        void **slot = fwmap_slot(&G.flows, &keys[i], sizeof keys[i], 1);
        if (!slot) break;
        FlowSt *f = *slot;
        if (!f && !(f = *slot = calloc(1, sizeof *f))) break;   /* new in the kernel since the last dump: all its bytes are new */
        if (f->tick == G.tick) continue;                         /* the same entry twice in one dump */
        uint64_t drx = vals[i].rx >= f->rx ? vals[i].rx - f->rx : vals[i].rx;   /* LRU reuse: started over */
        uint64_t dtx = vals[i].tx >= f->tx ? vals[i].tx - f->tx : vals[i].tx;
        f->rrx = dt > 0 ? (double)drx / dt : 0;
        f->rtx = dt > 0 ? (double)dtx / dt : 0;
        if (drx || dtx || vals[i].blocked != f->blocked || !f->active) f->active = G.tick;
        f->rx = vals[i].rx;
        f->tx = vals[i].tx;
        f->blocked = vals[i].blocked;
        f->tick = G.tick;
    }
    /* the table mirrors the kernel's: what the LRU evicted (or a closed scope left) goes */
    FWMAP_EACH(&G.flows, e) {
        FlowSt *f = e->val;
        if (f->tick != G.tick) free(fwmap_take(&G.flows, e->key, e->klen));
    }
}

/* No bytes moved this second: every address is idle (no need to read the kernel's table). */
static void idle_flows(void) {
    FWMAP_EACH(&G.flows, e) {
        FlowSt *f = e->val;
        f->rrx = f->rtx = 0;
    }
}

/* [["addr",rx,tx,rxRate,txRate,blocked,[[port,"tcp"|"udp"|"other","out"|"in",rx,tx,rxRate,txRate],...]],...]
 * of one app, busiest address first (0.8.5: np_flows is per address and port; this folds the ports
 * of each address together and lists its 8 busiest under it). */
#define PORTS_PER_ADDR 8
typedef struct { const NpFlowKey *k; uint64_t rx, tx, blocked; double rrx, rtx; int first, n; } AddrAgg;

static int agg_cmp(const void *a, const void *b) {
    const AddrAgg *x = a, *y = b;
    double rx = x->rrx + x->rtx, ry = y->rrx + y->rtx;
    if (rx != ry) return rx < ry ? 1 : -1;
    uint64_t bx = x->rx + x->tx, by = y->rx + y->tx;
    return bx < by ? 1 : bx > by ? -1 : 0;
}

static int same_addr(const NpFlowKey *a, const NpFlowKey *b) { return a->fam == b->fam && !memcmp(a->addr, b->addr, 16); }

static int ref_addr_cmp(const void *a, const void *b) {
    const NpFlowKey *x = ((const FlowRef *)a)->k, *y = ((const FlowRef *)b)->k;
    if (x->fam != y->fam) return x->fam - y->fam;
    int c = memcmp(x->addr, y->addr, 16);
    if (c) return c;
    return flow_cmp(a, b);
}

static size_t flows_json(uint64_t cgid, char *j, size_t cap) {
    static FlowRef refs[NP_MAX_FLOWS];
    static AddrAgg agg[NP_MAX_FLOWS];
    int n = 0;
    FWMAP_EACH(&G.flows, e) {
        const NpFlowKey *k = (const NpFlowKey *)(const void *)e->key;
        const FlowSt *f = e->val;
        if (k->cgid != cgid || n >= NP_MAX_FLOWS || G.tick - f->active > FLOW_KEEP_TICKS) continue;
        refs[n].k = k;
        refs[n++].f = e->val;
    }
    /* group by address (each group's ports busiest first) */
    qsort(refs, (size_t)n, sizeof refs[0], ref_addr_cmp);
    int na = 0;
    for (int i = 0; i < n; i++) {
        if (!na || !same_addr(agg[na - 1].k, refs[i].k)) {
            AddrAgg *a = &agg[na++];
            memset(a, 0, sizeof *a);
            a->k = refs[i].k;
            a->first = i;
        }
        AddrAgg *a = &agg[na - 1];
        a->rx += refs[i].f->rx;
        a->tx += refs[i].f->tx;
        a->blocked += refs[i].f->blocked;
        a->rrx += refs[i].f->rrx;
        a->rtx += refs[i].f->rtx;
        a->n++;
    }
    qsort(agg, (size_t)na, sizeof agg[0], agg_cmp);
    size_t w = 0;
    w += (size_t)snprintf(j + w, cap - w, "[");
    for (int i = 0; i < na && i < FLOWS_PER_APP && w + 170 + PORTS_PER_ADDR * 80 < cap; i++) {
        char addr[INET6_ADDRSTRLEN] = "";
        inet_ntop(agg[i].k->fam == 4 ? AF_INET : AF_INET6, agg[i].k->addr, addr, sizeof addr);
        w += (size_t)snprintf(j + w, cap - w, "%s[\"%s\",%llu,%llu,%.0f,%.0f,%llu,[", i ? "," : "", addr,
                              (unsigned long long)agg[i].rx, (unsigned long long)agg[i].tx, agg[i].rrx, agg[i].rtx,
                              (unsigned long long)agg[i].blocked);
        for (int p = 0; p < agg[i].n && p < PORTS_PER_ADDR; p++) {
            const FlowRef *r = &refs[agg[i].first + p];
            const char *pr = r->k->proto == NP_P_TCP ? "tcp" : r->k->proto == NP_P_UDP ? "udp" : "other";
            w += (size_t)snprintf(j + w, cap - w, "%s[%u,\"%s\",\"%s\",%llu,%llu,%.0f,%.0f]", p ? "," : "", r->k->port, pr,
                                  r->k->dir == NP_D_IN ? "in" : "out", (unsigned long long)r->f->rx, (unsigned long long)r->f->tx,
                                  r->f->rrx, r->f->rtx);
        }
        w += (size_t)snprintf(j + w, cap - w, "]]");
    }
    w += (size_t)snprintf(j + w, cap - w, "]");
    return w;
}

/* Every second: {"app":[allowed,denied,rx,tx,rxRate,txRate,[[addr,rx,tx,rxRate,txRate,blocked],...],blocked],...}
 * to each user with something new (the rates are bytes per second over the last second). With
 * no traffic and no new connections nothing is sent. */
static void push_counts(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    long long now = (long long)ts.tv_sec * 1000000000ll + ts.tv_nsec;
    double dt = G.counts_ns ? (double)(now - G.counts_ns) / 1e9 : 0;
    G.counts_ns = now;
    int bytes_moved = 0;
    for (int i = 0; i < G.nsess; i++) {
        Session *s = &G.sess[i];
        for (int k = 0; k < s->nsc; k++) {
            Scope *sc = &s->sc[k];
            NpCount c = { 0, 0, 0, 0, 0 };
            if (G.bpf_ok) bpfs_map_lookup(G.m_np_counts, &sc->cgid, &c);
            c.denied += sc->cuts;           /* the sockets a sweep destroyed are denied connections too */
            if (c.allowed != sc->allowed || c.denied != sc->denied || c.blocked != sc->blocked) { s->dirty_counts = 1; bytes_moved = 1; }
            double rrx = dt > 0 && c.rx >= sc->rx ? (double)(c.rx - sc->rx) / dt : 0;
            double rtx = dt > 0 && c.tx >= sc->tx ? (double)(c.tx - sc->tx) / dt : 0;
            if (c.rx != sc->rx || c.tx != sc->tx || (rrx == 0) != (sc->rrx == 0) || (rtx == 0) != (sc->rtx == 0)) { s->dirty_counts = 1; bytes_moved = 1; }
            sc->allowed = c.allowed;
            sc->denied = c.denied;
            sc->blocked = c.blocked;
            sc->rx = c.rx;
            sc->tx = c.tx;
            sc->rrx = rrx;
            sc->rtx = rtx;
        }
    }
    int sys_moved = 0;
    for (int i = 0; i < 3; i++) {
        if (!G.sys[i].cgid) continue;
        NpCount c = { 0, 0, 0, 0, 0 };
        bpfs_map_lookup(G.m_np_sysc, &G.sys[i].cgid, &c);
        double rrx = dt > 0 && c.rx >= G.sys[i].rx ? (double)(c.rx - G.sys[i].rx) / dt : 0;
        double rtx = dt > 0 && c.tx >= G.sys[i].tx ? (double)(c.tx - G.sys[i].tx) / dt : 0;
        if (c.rx != G.sys[i].rx || c.tx != G.sys[i].tx || (rrx == 0) != (G.sys[i].rrx == 0) || (rtx == 0) != (G.sys[i].rtx == 0)) sys_moved = 1;
        G.sys[i].rx = c.rx;
        G.sys[i].tx = c.tx;
        G.sys[i].rrx = rrx;
        G.sys[i].rtx = rtx;
    }
    if (sys_moved) for (int i = 0; i < G.nsess; i++) G.sess[i].dirty_counts = 1;
    /* the per-address table only while bytes move (one batch call per second) */
    if (G.bpf_ok && G.acct_ok == 2) {
        if (bytes_moved) read_flows(dt);
        else idle_flows();
    }
    for (int i = 0; i < G.nsess; i++) {
        Session *s = &G.sess[i];
        if (!s->dirty_counts) continue;
        int has_client = 0;
        for (int c = 0; c < G.ncl && !has_client; c++) has_client = G.cl[c].uid == s->uid;
        if (!has_client) continue;                                  /* stays dirty until someone connects */
        s->dirty_counts = 0;
        size_t per = FW_APP_MAX * 2 + 200 + (size_t)FLOWS_PER_APP * (170 + PORTS_PER_ADDR * 80);
        size_t cap = 64 + (size_t)s->nsc * per + 3 * 160, w = 0;
        char *j = malloc(cap);
        if (!j) continue;
        j[w++] = '{';
        for (int k = 0; k < s->nsc; k++) {
            Scope *sc = &s->sc[k];
            char app[FW_APP_MAX * 2 + 4];
            json_quote(app, sizeof app, sc->app);
            w += (size_t)snprintf(j + w, cap - w, "%s%s:[%llu,%llu,%llu,%llu,%.0f,%.0f,", w > 1 ? "," : "", app,
                                  (unsigned long long)sc->allowed, (unsigned long long)sc->denied,
                                  (unsigned long long)sc->rx, (unsigned long long)sc->tx, sc->rrx, sc->rtx);
            w += flows_json(sc->cgid, j + w, cap - w - 40);
            w += (size_t)snprintf(j + w, cap - w, ",%llu]", (unsigned long long)sc->blocked);
        }
        /* the essential system services: the same shape, never a connection count or a rule */
        for (int k = 0; k < 3; k++) {
            if (!G.sys[k].cgid) continue;
            w += (size_t)snprintf(j + w, cap - w, "%s\"%s\":[0,0,%llu,%llu,%.0f,%.0f,[],0]", w > 1 ? "," : "", SYS[k].id,
                                  (unsigned long long)G.sys[k].rx, (unsigned long long)G.sys[k].tx, G.sys[k].rrx, G.sys[k].rtx);
        }
        j[w++] = '}';
        send_uid(s->uid, NP_MSG_COUNTS, j, w, 0);
        free(j);
    }
}

/* ---- names from the resolver ----------------------------------------------------------------- */

static void on_dns(void *u, const uint8_t *pkt, size_t len) {
    (void)u;
    long long now = mono();
    if (dnsmap_learn(G.names, NAMES_APP, pkt, len, now) <= 0 || !G.bpf_ok) return;
    DnsResponse *r = malloc(sizeof *r);
    if (!r || dns_parse_response(pkt, len, r)) { free(r); return; }
    /* only the nodes of these addresses, for the apps with domain rules (before the app connects:
     * resolved notifies the monitor before answering the app) */
    for (int a = 0; a < r->naddr; a++) {
        char names[DNSMAP_PER_IP][FW_NAME_MAX];
        int n = dnsmap_lookup(G.names, NAMES_APP, r->addr[a].fam, r->addr[a].addr, now, names, DNSMAP_PER_IP);
        if (!n) continue;
        for (int i = 0; i < G.nsess; i++) {
            Session *s = &G.sess[i];
            if (s->lists && fwl_domains(s->lists)) {           /* blocklists: before the app connects */
                const char *p[DNSMAP_PER_IP];
                for (int k = 0; k < n; k++) p[k] = names[k];
                if (fwl_block_names(s->lists, p, n)) blk_want(s, r->addr[a].fam, r->addr[a].addr, r->addr[a].fam == 4 ? 32 : 128, 1);
                else blk_unwant_learned(s, r->addr[a].fam, r->addr[a].addr);
            }
            for (int k = 0; k < s->nsc; k++) {
                Scope *sc = &s->sc[k];
                if (!sc->has_domains) continue;
                NpNode node;
                int tr = 0;
                if (np_compile_host(store_of(s), sc->app, r->addr[a].fam, r->addr[a].addr, (const char (*)[FW_NAME_MAX])names, n, &node, &tr) != 1) continue;
                if (install(sc->idx, sc->gen, &node)) continue;
                np_u8 *has = node.fam == 4 ? &sc->app_v.has4 : &sc->app_v.has6;
                if (!*has) { *has = 1; bpfs_map_update(G.m_np_apps, &sc->cgid, &sc->app_v, BPF_ANY); }
            }
        }
    }
    free(r);
}

/* ---- BPF ------------------------------------------------------------------------------------- */

#ifdef NP_HAVE_BPF
/* Major of /dev/hidraw* (dynamic): the "hidraw" line of /proc/devices, or 0. */
static uint32_t hidraw_major(void) {
    FILE *f = fopen("/proc/devices", "re");
    char line[128];
    unsigned m = 0, v;
    char name[64];
    int chr = 0;
    while (f && fgets(line, sizeof line, f)) {
        if (!strncmp(line, "Character devices:", 18)) { chr = 1; continue; }
        if (!strncmp(line, "Block devices:", 14)) chr = 0;
        if (chr && sscanf(line, "%u %63s", &v, name) == 2 && !strcmp(name, "hidraw")) m = v;
    }
    if (f) fclose(f);
    return m;
}
#endif

static int bpf_setup(void) {
#ifndef NP_HAVE_BPF
    lerr_set(G.bpf_err, sizeof G.bpf_err, "fw.bpfNotBuilt", NULL);
    return -1;
#else
#define NP_MKMAP(n, t, k, v, m, f) G.m_##n = bpfs_map_create(t, k, v, m, f, #n);
    NP_MAPS(NP_MKMAP)
#undef NP_MKMAP
    if (G.m_np_blk4 < 0 || G.m_np_blk6 < 0 || G.m_np_sysc < 0 || G.m_np_bound < 0 || G.m_np_flows < 0 || G.m_np_snd < 0 || G.m_np_cfg < 0 || G.m_np_apps < 0 || G.m_np_union < 0 || G.m_np_lpm4 < 0 || G.m_np_lpm6 < 0 || G.m_np_seen < 0 || G.m_np_counts < 0 || G.m_np_events < 0) {
        lerr_set(G.bpf_err, sizeof G.bpf_err, "fw.bpfMaps", "errno", lerr_errno(errno), NULL);
        return -1;
    }
    if (bpfs_ring_open(&G.ring, G.m_np_events, NP_RINGBUF)) {
        lerr_set(G.bpf_err, sizeof G.bpf_err, "fw.bpfRing", "errno", lerr_errno(errno), NULL);
        return -1;
    }
    static char log[1 << 20];
    int np = 0;
    for (int w = 0; w < NP_NSPECS; w++) {
        const NpProgSpec *want = np_prog_specs;
        const NpProgDesc *d = NULL;
        for (int i = 0; i < NP_NPROGS; i++) if (!strcmp(np_progs[i].section, want[w].sec)) d = &np_progs[i];
        if (!d) { lerr_set(G.bpf_err, sizeof G.bpf_err, "fw.bpfMissing", "program", want[w].sec, NULL); return -1; }
        uint32_t btf_id = 0;
        if (want[w].btf && !(btf_id = bpfs_btf_func_id(NULL, want[w].btf))) {
            logf_("%s: %s is not in the kernel's BTF: \"no Bluetooth\" is not enforced", want[w].sec, want[w].btf);
            continue;
        }
        int fd = bpfs_prog_load_btf(want[w].type, want[w].attach, btf_id, np_prog_license(&want[w]), d, map_fd, log, sizeof log);
        if (fd < 0 && want[w].opt) {
            int e = errno;
            size_t ll = strlen(log);
            fprintf(stderr, "%s\n", ll > 3000 ? log + ll - 3000 : log);
            logf_("%s: the kernel refused it (%s); that restriction is not enforced", want[w].sec, strerror(e));
            continue;
        }
        if (fd < 0) {
            int e = errno;
            size_t ll = strlen(log);
            fprintf(stderr, "%s\n", ll > 3000 ? log + ll - 3000 : log);
            lerr_set(G.bpf_err, sizeof G.bpf_err, "fw.bpfRejected", "program", want[w].sec, "errno", lerr_errno(e), NULL);
            return -1;
        }
        if (want[w].attach == BPF_CGROUP_DEVICE) G.dev_ok = 1;
        if (want[w].attach == BPF_LSM_CGROUP) G.bt_ok = 1;
        if (want[w].attach == BPF_CGROUP_INET_INGRESS || want[w].attach == BPF_CGROUP_INET_EGRESS) G.acct_ok++;
        G.attach[np] = want[w].attach;
        G.progs[np++] = fd;
    }
    G.nprogs = np;
    NpCfg cfg = { SCOPE_LEVEL, 1, USER_LEVEL, hidraw_major() };
    uint32_t zero = 0;
    if (bpfs_map_update(G.m_np_cfg, &zero, &cfg, BPF_ANY)) { lerr_set(G.bpf_err, sizeof G.bpf_err, "fw.bpfConfig", "errno", lerr_errno(errno), NULL); return -1; }
    logf_("%d BPF programs loaded (devices: %s, Bluetooth sockets: %s, traffic accounting: %s, hidraw major %u)", np, G.dev_ok ? "yes" : "no",
          G.bt_ok ? "yes" : "no", G.acct_ok == 2 ? "yes" : "no", cfg.hidraw_major);
    return 0;
#endif
}

/* Attaches the programs to the lordos.service cgroup. systemd deletes and recreates it every
 * time the service restarts (and it may not exist yet at startup), so this is called at start,
 * on every inotify notice in system.slice and once per second: if the cgroup is a different one
 * (another id), the old links are dropped and attached again. */
static void attach_service(void) {
    if (!G.bpf_ok) return;
    int dfd = open(CG_ROOT "/system.slice", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    uint64_t id = dfd >= 0 ? bpfs_cgroup_id(dfd, "lordos.service") : 0;
    if (dfd >= 0) close(dfd);
    if (id == G.attached_cg) return;
    for (int k = 0; k < G.nlinks; k++) close(G.links[k]);
    G.nlinks = 0;
    if (G.cgfd >= 0) close(G.cgfd);
    G.cgfd = -1;
    G.attached_cg = 0;
    if (!id) { logf_("waiting for lordos.service (its cgroup doesn't exist)"); return; }
    G.cgfd = open(CG_ROOT "/" CG_SERVICE, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (G.cgfd < 0) return;
    for (int i = 0; i < G.nprogs; i++) {
        int l = bpfs_link_create(G.progs[i], G.cgfd, G.attach[i]);
        if (l < 0) {
            logf_("could not attach a program: %s", strerror(errno));
            for (int k = 0; k < G.nlinks; k++) close(G.links[k]);
            G.nlinks = 0;
            return;
        }
        G.links[G.nlinks++] = l;
    }
    G.attached_cg = id;
    if (G.root_wd >= 0) inotify_rm_watch(G.ifd, G.root_wd);
    G.root_wd = inotify_add_watch(G.ifd, CG_ROOT "/" CG_SERVICE, IN_CREATE | IN_DELETE | IN_ONLYDIR);
    logf_("%d BPF programs attached to " CG_SERVICE " (scopes at level %d)", G.nlinks, SCOPE_LEVEL);
}

/* ---- main ------------------------------------------------------------------------------------ */

static volatile sig_atomic_t quit;
static void on_term(int s) { (void)s; quit = 1; }

static int listen_sock(void) {
    mkdir("/run/lordos", 0755);
    struct stat st;
    if (lstat("/run/lordos", &st) || !S_ISDIR(st.st_mode) || st.st_uid != 0) { logf_("/run/lordos is not a root-owned folder"); return -1; }
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", NP_SOCK_PATH);
    unlink(NP_SOCK_PATH);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) || chmod(NP_SOCK_PATH, 0666) || listen(fd, 16)) {
        logf_("could not open %s: %s", NP_SOCK_PATH, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static void watch_dns(void) {
    int fd = G.dns ? npdns_fd(G.dns) : -1;
    if (fd == G.dns_fd) return;
    if (G.dns_fd >= 0) epoll_ctl(G.ep, EPOLL_CTL_DEL, G.dns_fd, NULL);
    G.dns_fd = fd;
    if (fd >= 0) {
        struct epoll_event ev = { .events = EPOLLIN, .data.u64 = 5 };
        epoll_ctl(G.ep, EPOLL_CTL_ADD, fd, &ev);
    }
    char st[400];
    status_json(st, sizeof st);
    for (int i = 0; i < G.ncl; i++) cl_send(&G.cl[i], NP_MSG_STATUS, st, strlen(st), 0);
}

int main(int argc, char **argv) {
    (void)argv;
    if (argc != 1) { fprintf(stderr, "lord-netperm takes no arguments (see docs/FIREWALL.md)\n"); return 2; }
    if (geteuid() != 0) { fprintf(stderr, "lord-netperm: must run as root (lord-netperm.service)\n"); return 1; }
    setvbuf(stderr, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa = { .sa_handler = on_term };
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    G.dns_fd = -1;
    G.cgfd = -1;
    G.diag_fd = -1;
    G.empty = fw_store_new();
    G.names = dnsmap_new(16384);
    G.ep = epoll_create1(EPOLL_CLOEXEC);
    G.ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    G.tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    G.lfd = listen_sock();
    if (!G.empty || !G.names || G.ep < 0 || G.ifd < 0 || G.tfd < 0 || G.lfd < 0) { logf_("could not start"); return 1; }

    G.bpf_ok = bpf_setup() == 0;
    if (!G.bpf_ok) logf_("BPF not available (%s): no rule is enforced; the desktop carries on as usual", G.bpf_err);
    else G.dns = npdns_new(on_dns, NULL);

    struct itimerspec its = { { 1, 0 }, { 1, 0 } };
    timerfd_settime(G.tfd, 0, &its, NULL);
    struct epoll_event ev = { .events = EPOLLIN, .data.u64 = 1 };
    epoll_ctl(G.ep, EPOLL_CTL_ADD, G.lfd, &ev);
    ev.data.u64 = 2;
    epoll_ctl(G.ep, EPOLL_CTL_ADD, G.ifd, &ev);
    ev.data.u64 = 3;
    epoll_ctl(G.ep, EPOLL_CTL_ADD, G.tfd, &ev);
    if (G.bpf_ok) { ev.data.u64 = 4; epoll_ctl(G.ep, EPOLL_CTL_ADD, G.m_np_events, &ev); }
    G.root_wd = -1;
    G.sys_wd = inotify_add_watch(G.ifd, CG_ROOT "/system.slice", IN_CREATE | IN_ONLYDIR);
    attach_service();
    if (G.root_wd < 0) G.root_wd = inotify_add_watch(G.ifd, CG_ROOT "/" CG_SERVICE, IN_CREATE | IN_DELETE | IN_ONLYDIR);
    scan_all();
    sweep((unsigned)-1);                /* what was opened while this service wasn't running, and the servers' ports */
    watch_dns();

    while (!quit) {
        struct epoll_event evs[32];
        int n = epoll_wait(G.ep, evs, 32, -1);
        if (n < 0 && errno != EINTR) break;
        for (int i = 0; i < n; i++) {
            uint64_t t = evs[i].data.u64;
            if (t == 1) accept_client();
            else if (t == 2) {
                char buf[4096];
                while (read(G.ifd, buf, sizeof buf) > 0) {}
                attach_service();                   /* did lordos.service start again? */
                scan_all();                         /* a scope or a session that's new or gone */
            } else if (t == 3) {
                uint64_t x;
                if (read(G.tfd, &x, sizeof x) < 0) {}
                G.tick++;
                attach_service();
                sync_system();
                if (G.dns) { npdns_tick(G.dns); watch_dns(); }
                if (G.tick % 5 == 0) scan_all();    /* in case an inotify notice was lost */
                if (G.tick % 30 == 0) {
                    dnsmap_expire(G.names, mono());
                    for (int s = 0; s < G.nsess; s++) if (G.sess[s].lists || G.sess[s].blkin.n) sync_blk(&G.sess[s]);
                    for (int s = 0; s < G.nsess; s++)
                        for (int k = 0; k < G.sess[s].nsc; k++)
                            if (G.sess[s].sc[k].has_domains) sync_scope(&G.sess[s], &G.sess[s].sc[k]);
                }
                push_counts();
            } else if (t == 4) bpfs_ring_drain(&G.ring, on_event, NULL);
            else if (t == 5) { if (G.dns) npdns_ready(G.dns); watch_dns(); }
            else if (t >= 1000) {
                int fd = (int)(t - 1000);
                for (int k = 0; k < G.ncl; k++) {
                    if (G.cl[k].fd != fd) continue;
                    if (evs[i].events & (EPOLLHUP | EPOLLERR)) { cl_drop(k); break; }
                    if (evs[i].events & EPOLLOUT) cl_flush(&G.cl[k]);
                    if (evs[i].events & EPOLLIN) client_read(k);
                    break;
                }
            }
        }
    }
    logf_("exiting: the programs detach (everything is allowed)");
    unlink(NP_SOCK_PATH);
    return 0;
}
