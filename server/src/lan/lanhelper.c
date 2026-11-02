/* lanhelper.c — axia-lan: reads what the local network answers, for Files → Network (see lanproto.h
 * and docs/FILES.md). No sockets of its own and no files: sandbox_apply_helper before anything is
 * read. Everything that comes in is untrusted; a hostile machine on the LAN can at most bring this
 * process down. */
#include "lanaddr.h"
#include "lanparse.h"
#include "smbauth.h"
#include "lanproto.h"
#include "lanservices.h"
#include "../doc/wire.h"
#include "../print/mdns.h"
#include "../sec/json.h"
#include "../sec/sandbox.h"

#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <strings.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SOCK 3
#define JOB_MS 4000                     /* one server, one answer: the most a job may wait */
#define MSG_MAX (128 * 1024)

static long long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000LL + t.tv_nsec / 1000000;
}

static int wait_fd(int fd, short ev, long long deadline) {
    for (;;) {
        long long left = deadline - now_ms();
        if (left <= 0) { errno = ETIMEDOUT; return -1; }
        struct pollfd p = { fd, ev, 0 };
        int r = poll(&p, 1, (int)left);
        if (r > 0) return 0;
        if (r < 0 && errno != EINTR) return -1;
    }
}

static int write_all(int fd, const uint8_t *b, size_t n, long long deadline) {
    size_t o = 0;
    while (o < n) {
        ssize_t w = write(fd, b + o, n - o);
        if (w > 0) { o += (size_t)w; continue; }
        if (w < 0 && errno != EAGAIN && errno != EINTR) return -1;
        if (wait_fd(fd, POLLOUT, deadline)) return -1;
    }
    return 0;
}

static int read_all(int fd, uint8_t *b, size_t n, long long deadline) {
    size_t o = 0;
    while (o < n) {
        ssize_t r = read(fd, b + o, n - o);
        if (r > 0) { o += (size_t)r; continue; }
        if (!r) return -1;
        if (errno != EAGAIN && errno != EINTR) return -1;
        if (wait_fd(fd, POLLIN, deadline)) return -1;
    }
    return 0;
}

static void reply(uint32_t op, uint32_t id, JBuf *b) { doc_send(SOCK, op, id, b->p ? b->p : "{}", b->p ? b->n : 2, NULL, 0); }

static void fail(uint32_t id, const char *code) {
    JBuf b;
    jb_init(&b);
    jb_raw(&b, "{\"error\":");
    jb_str(&b, code);
    jb_raw(&b, "}");
    reply(LR_ERR, id, &b);
    jb_free(&b);
}

/* ---- mDNS ---- */

static MdnsCache *cache;

/* what answered is where the service is, when the answer didn't bring an address record */
static void note_sender(uint32_t src) {
    char addr[16];
    struct in_addr in = { src };
    if (!inet_ntop(AF_INET, &in, addr, sizeof addr)) return;
    for (int i = 0; i < cache->nsrv; i++) {
        int have = 0;
        for (int k = 0; k < cache->na; k++) if (!strcasecmp(cache->a[k].host, cache->srv[i].host)) have = 1;
        if (have || cache->na >= MDNS_MAX) continue;
        memcpy(cache->a[cache->na].host, cache->srv[i].host, sizeof cache->a[0].host);
        snprintf(cache->a[cache->na].addr, sizeof cache->a[0].addr, "%s", addr);
        cache->na++;
    }
}

static void mdns_in(const char *p, size_t len) {
    if (len < 16) return;
    uint32_t src;
    memcpy(&src, p, 4);
    if (!mdns_parse((const uint8_t *)p + 4, len - 4, cache)) note_sender(src);
}

static void mdns_end(uint32_t id) {
    static MdnsSvc svc[256];
    const char *types[LAN_SERVICES];
    for (int i = 0; i < LAN_SERVICES; i++) types[i] = lan_services[i].type;
    int n = mdns_collect(cache, types, LAN_SERVICES, svc, 256);
    JBuf b;
    jb_init(&b);
    jb_raw(&b, "{\"services\":[");
    char v[256];
    for (int i = 0; i < n; i++) {
        if (i) jb_raw(&b, ",");
        jb_raw(&b, "{\"type\":");
        jb_str(&b, svc[i].type);
        jb_raw(&b, ",\"name\":");
        jb_str(&b, svc[i].name);
        jb_raw(&b, ",\"host\":");
        jb_str(&b, svc[i].host);
        jb_raw(&b, ",\"addr\":");
        jb_str(&b, svc[i].addr);
        jb_printf(&b, ",\"port\":%d,\"path\":", svc[i].port);
        jb_str(&b, mdns_txt(&svc[i], "path", v, sizeof v));
        jb_raw(&b, ",\"model\":");
        jb_str(&b, mdns_txt(&svc[i], "ty", v, sizeof v)[0] ? v : mdns_txt(&svc[i], "model", v, sizeof v));
        jb_raw(&b, "}");
    }
    /* answers to a reverse question: 24.0.17.172.in-addr.arpa → host.local */
    jb_raw(&b, "],\"names\":[");
    int k = 0;
    for (int i = 0; i < cache->nptr; i++) {
        unsigned o[4];
        char tail[32] = "";
        if (sscanf(cache->ptr[i].name, "%u.%u.%u.%u.%31s", &o[3], &o[2], &o[1], &o[0], tail) != 5 || strcasecmp(tail, "in-addr.arpa") ||
            o[0] > 255 || o[1] > 255 || o[2] > 255 || o[3] > 255) continue;
        char addr[16];
        snprintf(addr, sizeof addr, "%u.%u.%u.%u", o[0], o[1], o[2], o[3]);
        if (k++) jb_raw(&b, ",");
        jb_raw(&b, "{\"addr\":");
        jb_str(&b, addr);
        jb_raw(&b, ",\"host\":");
        jb_str(&b, cache->ptr[i].first);
        jb_raw(&b, "}");
    }
    jb_raw(&b, "]}");
    reply(LR_MDNS, id, &b);
    jb_free(&b);
    memset(cache, 0, sizeof *cache);
}

/* ---- SMB: who it is and, if it lets an anonymous session in, what it shares ---- */

static uint8_t rxbuf[MSG_MAX];
static size_t rxlen;

static int smb_exchange(int fd, const uint8_t *out, size_t n, SmbMsg *m, long long deadline) {
    if (!n || write_all(fd, out, n, deadline)) return -1;
    for (;;) {
        uint8_t h[4];
        if (read_all(fd, h, 4, deadline) || h[0]) return -1;
        size_t l = (size_t)h[1] << 16 | (size_t)h[2] << 8 | h[3];
        if (l < SMB_HDR || l > sizeof rxbuf || read_all(fd, rxbuf, l, deadline) || smb_parse(rxbuf, l, m)) return -1;
        rxlen = l;
        if (m->status != SMB_STATUS_PENDING) return 0;  /* an interim answer: the real one follows */
    }
}

static void smb_job(uint32_t id, const char *ip, int fd) {
    static uint8_t tx[8192];
    long long deadline = now_ms() + JOB_MS;
    char name[128] = "", domain[64] = "";
    const char *session = "none";
    int dialect = 0, signing = 0, listed = 0, count = 0;
    static LanShare shares[LAN_MAX_LIST];
    SmbMsg m;
    uint64_t mid = 0, sid = 0;
    if (smb_exchange(fd, tx, smb_negotiate(tx, sizeof tx, mid++), &m, deadline) || !(dialect = smb_negotiate_reply(&m, &signing))) { fail(id, "smb-negotiate"); return; }
    uint8_t blob[128];
    size_t bl = ntlm_negotiate(blob, sizeof blob);
    if (smb_exchange(fd, tx, smb_session_setup(tx, sizeof tx, mid++, 0, blob, bl), &m, deadline) || m.status != SMB_STATUS_MORE) goto done;
    const uint8_t *sb;
    size_t sl;
    unsigned flags;
    if (smb_session_reply(&m, &sb, &sl, &flags) || !sb) goto done;
    sid = m.session;
    if (ntlm_challenge_names(sb, sl, name, sizeof name, domain, sizeof domain)) goto done;
    bl = ntlm_anonymous_auth(blob, sizeof blob);
    session = "refused";
    if (smb_exchange(fd, tx, smb_session_setup(tx, sizeof tx, mid++, sid, blob, bl), &m, deadline) || m.status) goto done;
    if (smb_session_reply(&m, &sb, &sl, &flags)) goto done;
    session = flags & 1 ? "guest" : "anonymous";
    if (smb_exchange(fd, tx, smb_tree_connect(tx, sizeof tx, mid++, sid, ip), &m, deadline) || m.status) goto done;
    uint32_t tree = m.tree;
    uint8_t fid[16];
    if (smb_exchange(fd, tx, smb_create_pipe(tx, sizeof tx, mid++, sid, tree), &m, deadline) || smb_create_reply(&m, fid)) goto done;
    uint8_t dce[256];
    const uint8_t *o;
    size_t ol;
    if (smb_exchange(fd, tx, smb_pipe_ioctl(tx, sizeof tx, mid++, sid, tree, fid, dce, dce_bind(dce, sizeof dce)), &m, deadline) ||
        smb_ioctl_reply(&m, &o, &ol) || dce_bind_ack(o, ol)) goto done;
    if (smb_exchange(fd, tx, smb_pipe_ioctl(tx, sizeof tx, mid++, sid, tree, fid, dce, dce_share_enum(dce, sizeof dce, ip)), &m, deadline) ||
        smb_ioctl_reply(&m, &o, &ol)) goto done;
    count = dce_share_reply(o, ol, shares, LAN_MAX_LIST);
    if (count < 0) count = 0; else listed = 1;
done:;
    JBuf b;
    jb_init(&b);
    jb_raw(&b, "{\"name\":");
    jb_str(&b, name);
    jb_raw(&b, ",\"domain\":");
    jb_str(&b, domain);
    jb_printf(&b, ",\"dialect\":%d,\"signing\":%d,\"session\":\"%s\",\"listed\":%d,\"shares\":[", dialect, signing, session, listed);
    for (int i = 0; i < count; i++) {
        if (i) jb_raw(&b, ",");
        jb_raw(&b, "{\"name\":");
        jb_str(&b, shares[i].name);
        jb_raw(&b, ",\"comment\":");
        jb_str(&b, shares[i].comment);
        jb_raw(&b, "}");
    }
    jb_raw(&b, "]}");
    reply(LR_SMB, id, &b);
    jb_free(&b);
}

/* ---- SMB after signing in ---- */

static int signed_exchange(int fd, uint8_t *tx, size_t n, const uint8_t key[16], SmbMsg *m, long long deadline) {
    if (!n) return -1;
    smb_sign(tx + 4, n - 4, key);
    return smb_exchange(fd, tx, n, m, deadline);
}

/* The shares a user sees. What comes back is only shown, so answers aren't checked against the signature;
 * the mount itself is the kernel's. */
static void smb_auth_job(uint32_t id, const char *ip, const char *user, const char *domain, const char *password, int fd) {
    static uint8_t tx[8192];
    static LanShare shares[LAN_MAX_LIST];
    long long deadline = now_ms() + JOB_MS;
    SmbMsg m;
    uint64_t mid = 0, sid = 0;
    int signing = 0;
    uint8_t blob[4096], rnd[24], key[16];
    if (getrandom(rnd, sizeof rnd, 0) != sizeof rnd) { fail(id, "random"); return; }
    if (smb_exchange(fd, tx, smb_negotiate_signed(tx, sizeof tx, mid++), &m, deadline) || !smb_negotiate_reply(&m, &signing)) { fail(id, "smb-negotiate"); return; }
    size_t bl = ntlm_user_negotiate(blob, sizeof blob), nl = bl;
    uint8_t neg[64];
    memcpy(neg, blob, nl);
    const uint8_t *sb;
    size_t sl;
    unsigned flags;
    if (smb_exchange(fd, tx, smb_session_setup(tx, sizeof tx, mid++, 0, blob, bl), &m, deadline) || m.status != SMB_STATUS_MORE ||
        smb_session_reply(&m, &sb, &sl, &flags) || !sb) { fail(id, "smb-session"); return; }
    sid = m.session;
    char spn[80];
    snprintf(spn, sizeof spn, "cifs/%s", ip);
    bl = ntlm_user_auth(blob, sizeof blob, neg, nl, sb, sl, user, domain, password, spn, rnd, key);
    if (!bl) { fail(id, "smb-session"); return; }
    size_t n = smb_session_setup(tx, sizeof tx, mid++, sid, blob, bl);
    explicit_bzero(blob, sizeof blob);
    int rc = smb_exchange(fd, tx, n, &m, deadline);
    explicit_bzero(tx, sizeof tx);
    /* a server that takes a bad password as its guest says so with these flags */
    if (rc || m.status || smb_session_reply(&m, &sb, &sl, &flags) || (flags & 3)) { fail(id, "denied"); return; }
    /* the server proves it knew the password: it signs this answer. Whoever can't is not the server the person signed in to */
    if (!smb_verify(rxbuf, rxlen, key)) { fail(id, "denied"); return; }
    uint8_t fid[16], dce[256];
    const uint8_t *o;
    size_t ol;
    if (signed_exchange(fd, tx, smb_tree_connect(tx, sizeof tx, mid++, sid, ip), key, &m, deadline) || m.status) { fail(id, "smb-list"); return; }
    uint32_t tree = m.tree;
    if (signed_exchange(fd, tx, smb_create_pipe(tx, sizeof tx, mid++, sid, tree), key, &m, deadline) || smb_create_reply(&m, fid)) { fail(id, "smb-list"); return; }
    if (signed_exchange(fd, tx, smb_pipe_ioctl(tx, sizeof tx, mid++, sid, tree, fid, dce, dce_bind(dce, sizeof dce)), key, &m, deadline) ||
        smb_ioctl_reply(&m, &o, &ol) || dce_bind_ack(o, ol)) { fail(id, "smb-list"); return; }
    if (signed_exchange(fd, tx, smb_pipe_ioctl(tx, sizeof tx, mid++, sid, tree, fid, dce, dce_share_enum(dce, sizeof dce, ip)), key, &m, deadline) ||
        smb_ioctl_reply(&m, &o, &ol)) { fail(id, "smb-list"); return; }
    int count = dce_share_reply(o, ol, shares, LAN_MAX_LIST);
    explicit_bzero(key, sizeof key);
    if (count < 0) { fail(id, "smb-list"); return; }
    JBuf b;
    jb_init(&b);
    jb_raw(&b, "{\"session\":\"user\",\"listed\":1,\"shares\":[");
    for (int i = 0; i < count; i++) {
        if (i) jb_raw(&b, ",");
        jb_raw(&b, "{\"name\":");
        jb_str(&b, shares[i].name);
        jb_raw(&b, ",\"comment\":");
        jb_str(&b, shares[i].comment);
        jb_raw(&b, "}");
    }
    jb_raw(&b, "]}");
    reply(LR_SMB, id, &b);
    jb_free(&b);
}

/* ---- NFS ---- */

/* The whole reply of one call: record fragments joined, at most MSG_MAX bytes. */
static long rpc_exchange(int fd, const uint8_t *out, size_t n, long long deadline) {
    if (!n || write_all(fd, out, n, deadline)) return -1;
    size_t o = 0;
    for (;;) {
        uint8_t h[4];
        if (read_all(fd, h, 4, deadline)) return -1;
        uint32_t mark = (uint32_t)h[0] << 24 | (uint32_t)h[1] << 16 | (uint32_t)h[2] << 8 | h[3];
        size_t l = mark & 0x7fffffffu;
        if (l > sizeof rxbuf - o || read_all(fd, rxbuf + o, l, deadline)) return -1;
        o += l;
        if (mark >> 31) return (long)o;
    }
}

static void portmap_job(uint32_t id, int fd) {
    uint8_t q[128];
    uint32_t xid = (uint32_t)now_ms() ^ 0x4c4f5244u;
    long n = rpc_exchange(fd, q, rpc_portmap_call(q, sizeof q, xid), now_ms() + JOB_MS);
    int port = n < 0 ? -1 : rpc_portmap_reply(rxbuf, (size_t)n, xid);
    if (port < 0) { fail(id, "rpc"); return; }
    JBuf b;
    jb_init(&b);
    jb_printf(&b, "{\"mountd\":%d}", port);
    reply(LR_PORTMAP, id, &b);
    jb_free(&b);
}

static void exports_job(uint32_t id, int fd) {
    uint8_t q[128];
    uint32_t xid = (uint32_t)now_ms() ^ 0x4e465321u;
    static LanExport ex[LAN_MAX_LIST];
    long n = rpc_exchange(fd, q, rpc_export_call(q, sizeof q, xid), now_ms() + JOB_MS);
    int c = n < 0 ? -1 : rpc_export_reply(rxbuf, (size_t)n, xid, ex, LAN_MAX_LIST);
    if (c < 0) { fail(id, "rpc"); return; }
    JBuf b;
    jb_init(&b);
    jb_raw(&b, "{\"exports\":[");
    for (int i = 0; i < c; i++) {
        if (i) jb_raw(&b, ",");
        jb_raw(&b, "{\"path\":");
        jb_str(&b, ex[i].path);
        jb_printf(&b, ",\"open\":%d}", ex[i].open);
    }
    jb_raw(&b, "]}");
    reply(LR_EXPORTS, id, &b);
    jb_free(&b);
}

int main(void) {
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    prctl(PR_SET_DUMPABLE, 0);
    struct rlimit nocore = { 0, 0 };
    setrlimit(RLIMIT_CORE, &nocore);
    signal(SIGPIPE, SIG_IGN);
    struct stat st;
    if (fstat(SOCK, &st) || !S_ISSOCK(st.st_mode)) { fprintf(stderr, "axia-lan: started by axia-server only\n"); return 2; }
    cache = calloc(1, sizeof *cache);
    if (!cache) return 3;
    SandboxHelperCfg cfg = {
        .name = "lordos-lan",            /* cgroup scope lordos-lan.scope (lord-sessiond creates it) */
        .ro_paths = NULL,
        .extra_syscalls = (int[]){ -1 },
        .allow_ioctl_classes = 0,
        .mem_max_mb = 64,
        .ro_dir_fds = NULL,
        .rw_dir_fds = NULL,
    };
    setpriority(PRIO_PROCESS, 0, 10);
    sandbox_apply_helper(&cfg);
    for (;;) {
        DocMsg m;
        int rc = doc_recv(SOCK, &m);
        if (rc) return rc == 1 ? 0 : 1;
        int fd = m.nfds == 1 ? m.fds[0] : -1;
        switch (m.h.op) {
        case LQ_END: doc_msg_free(&m); return 0;
        case LQ_MDNS: if (m.json) mdns_in(m.json, m.len); break;
        case LQ_MDNS_END: mdns_end(m.h.id); break;
        case LQ_SMB: {
            /* the address only names the server in the protocol's own paths; axia-server sends it */
            char ip[16] = "";
            if (fd >= 0 && m.json && m.len < sizeof ip) { memcpy(ip, m.json, m.len); ip[m.len] = 0; smb_job(m.h.id, ip, fd); }
            else fail(m.h.id, "bad-request");
            break;
        }
        case LQ_SMB_AUTH: {
            JDoc d;
            if (fd >= 0 && m.json && !json_parse(m.json, m.len, &d, NULL, 0)) {
                const char *ip = json_str(d.root, "addr"), *user = json_str(d.root, "user"), *dom = json_str(d.root, "domain"), *pw = json_str(d.root, "password");
                if (ip && user && dom && pw && strlen(ip) < 16) smb_auth_job(m.h.id, ip, user, dom, pw, fd);
                else fail(m.h.id, "bad-request");
                if (pw) explicit_bzero((char *)pw, strlen(pw));
                json_free(&d);
            } else fail(m.h.id, "bad-request");
            if (m.json) explicit_bzero(m.json, m.len);
            break;
        }
        case LQ_PORTMAP: if (fd >= 0) portmap_job(m.h.id, fd); else fail(m.h.id, "bad-request"); break;
        case LQ_EXPORTS: if (fd >= 0) exports_job(m.h.id, fd); else fail(m.h.id, "bad-request"); break;
        default: fail(m.h.id, "bad-request");
        }
        doc_msg_free(&m);
    }
}
