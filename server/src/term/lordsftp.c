/* lordsftp.c — lord-sftp, the Terminal's Files mode helper: speaks SFTP (sftp.h) to the server on the
 * tab's own ssh connection and answers lord-server (sftpproto.h). fd 3 is lord-server's channel, fd 4
 * what the server sends, fd 5 what it reads. It holds no path of the computer's and opens nothing:
 * a transfer's local file is an fd passed with the request. sandbox_apply_helper goes on before the
 * first byte from the server is read; from then on it can only read, write, poll and pass fds. */
#include "sftp.h"
#include "sftpproto.h"
#include "../doc/wire.h"
#include "../sec/json.h"
#include "../sec/sandbox.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>

#define CHAN 3
#define RD   4
#define WR   5
#define WINDOW 16                      /* reads or writes in flight in a transfer */

static uint32_t next_id = 1;
static MuxBuf out;

/* ---- the server ---- */

static int write_all(int fd, const void *p, size_t n) {
    const uint8_t *b = p;
    while (n) {
        ssize_t k = write(fd, b, n);
        if (k < 0) { if (errno == EINTR) continue; return -1; }
        b += k;
        n -= (size_t)k;
    }
    return 0;
}

static int read_all(int fd, void *p, size_t n) {
    uint8_t *b = p;
    while (n) {
        ssize_t k = read(fd, b, n);
        if (k == 0) return -1;
        if (k < 0) { if (errno == EINTR) continue; return -1; }
        b += k;
        n -= (size_t)k;
    }
    return 0;
}

static int send_pkt(const MuxBuf *b) {
    if (b->oom) return -1;
    uint8_t h[4] = { (uint8_t)(b->n >> 24), (uint8_t)(b->n >> 16), (uint8_t)(b->n >> 8), (uint8_t)b->n };
    return write_all(WR, h, 4) || write_all(WR, b->p, b->n) ? -1 : 0;
}

/* one packet from the server: *buf malloc'd (free it), r over it, type and id read */
static uint8_t *pkt;
static int recv_pkt(MuxRd *r, int *type, uint32_t *id) {
    free(pkt);
    pkt = NULL;
    uint8_t h[4];
    if (read_all(RD, h, 4)) return -1;
    uint32_t n = (uint32_t)h[0] << 24 | (uint32_t)h[1] << 16 | (uint32_t)h[2] << 8 | h[3];
    if (n < 5 || n > SFTP_PACKET_MAX) return -1;
    pkt = malloc(n);
    if (!pkt || read_all(RD, pkt, n)) return -1;
    *r = (MuxRd){ pkt, n, 0 };
    return sf_reply_head(r, type, id);
}

/* ---- answers to lord-server ---- */

static void reply(uint32_t rid, const char *json, size_t n) { doc_send(CHAN, SE_OK, rid, json, n, NULL, 0); }

static void reply_err(uint32_t rid, const char *code, const char *msg) {
    JBuf b;
    jb_init(&b);
    jb_raw(&b, "{\"error\":");
    jb_str(&b, code);
    if (msg && msg[0]) { jb_raw(&b, ",\"message\":"); jb_str(&b, msg); }
    jb_raw(&b, "}");
    if (!b.oom) doc_send(CHAN, SE_ERR, rid, b.p, b.n, NULL, 0);
    jb_free(&b);
}

static const char *code_of(uint32_t st) {
    return st == SSH_FX_NO_SUCH_FILE ? "nofile" : st == SSH_FX_PERMISSION_DENIED ? "denied" : st == SSH_FX_OP_UNSUPPORTED ? "unsupported" : "failed";
}

/* the request's answer: 0 OK, else an error was already sent; *closed set when the server is gone */
static int closed;
static int status_reply(uint32_t rid, uint32_t want) {
    MuxRd r;
    int type;
    uint32_t id, code;
    char msg[160];
    if (recv_pkt(&r, &type, &id)) { closed = 1; reply_err(rid, "closed", NULL); return -1; }
    if (id != want || type != SSH_FXP_STATUS || sf_status(&r, &code, msg, sizeof msg)) { closed = 1; reply_err(rid, "bad", NULL); return -1; }
    if (code != SSH_FX_OK) { reply_err(rid, code_of(code), msg); return -1; }
    return 0;
}

/* an answer that should be `want` (HANDLE, NAME, ATTRS): r positioned after the id; STATUS is an error */
static int typed_reply(uint32_t rid, uint32_t qid, int want, MuxRd *r) {
    int type;
    uint32_t id, code;
    char msg[160];
    if (recv_pkt(r, &type, &id)) { closed = 1; reply_err(rid, "closed", NULL); return -1; }
    if (id != qid) { closed = 1; reply_err(rid, "bad", NULL); return -1; }
    if (type == want) return 0;
    if (type == SSH_FXP_STATUS && !sf_status(r, &code, msg, sizeof msg)) { reply_err(rid, code_of(code), msg); return -1; }
    closed = 1;
    reply_err(rid, "bad", NULL);
    return -1;
}

static int get_handle(MuxRd *r, char *h, size_t *hn) {
    const char *s;
    size_t n;
    if (mxr_str(r, &s, &n) || !n || n > SFTP_HANDLE_MAX) return -1;
    memcpy(h, s, n);
    *hn = n;
    return 0;
}

static void close_handle(const char *h, size_t hn) {
    uint32_t id = next_id++;
    sf_req_handle(&out, SSH_FXP_CLOSE, id, h, hn);
    MuxRd r;
    int type;
    uint32_t rid;
    if (send_pkt(&out) || recv_pkt(&r, &type, &rid)) closed = 1;
}

static void attrs_json(JBuf *b, const SfAttrs *a, char kind) {
    jb_printf(b, "\"kind\":\"%c\"", kind);
    if (a->flags & SSH_FILEXFER_ATTR_SIZE) jb_printf(b, ",\"size\":%llu", (unsigned long long)a->size);
    if (a->flags & SSH_FILEXFER_ATTR_ACMODTIME) jb_printf(b, ",\"mtime\":%u", a->mtime);
    if (a->flags & SSH_FILEXFER_ATTR_PERMISSIONS) jb_printf(b, ",\"perm\":%u", a->perm & 07777);
    if (a->flags & SSH_FILEXFER_ATTR_UIDGID) jb_printf(b, ",\"uid\":%u,\"gid\":%u", a->uid, a->gid);
}

/* REALPATH: the server's canonical form of path (checked like a path from the user) */
static int realpath_of(uint32_t rid, const char *path, char *outp, size_t cap) {
    uint32_t id = next_id++;
    sf_req_path(&out, SSH_FXP_REALPATH, id, path);
    MuxRd r;
    uint32_t count;
    const char *s;
    size_t n;
    if (send_pkt(&out)) { closed = 1; reply_err(rid, "closed", NULL); return -1; }
    if (typed_reply(rid, id, SSH_FXP_NAME, &r)) return -1;
    if (mxr_u32(&r, &count) || count < 1 || mxr_str(&r, &s, &n) || !n || n >= cap) { reply_err(rid, "bad", NULL); return -1; }
    memcpy(outp, s, n);
    outp[n] = 0;
    if (!sf_path_ok(outp) || strlen(outp) != n) { reply_err(rid, "bad", NULL); return -1; }
    return 0;
}

/* ---- the requests ---- */

static const char *str_of(const JVal *o, const char *k) { const char *s = json_str(o, k); return s && sf_path_ok(s) ? s : NULL; }

static void do_list(uint32_t rid, const JVal *q) {
    const char *path = str_of(q, "path");
    if (!path) { reply_err(rid, "bad", NULL); return; }
    static char real[SFTP_NAME_MAX + 1];
    if (realpath_of(rid, path, real, sizeof real)) return;
    uint32_t id = next_id++;
    sf_req_path(&out, SSH_FXP_OPENDIR, id, real);
    MuxRd r;
    char h[SFTP_HANDLE_MAX];
    size_t hn;
    if (send_pkt(&out)) { closed = 1; reply_err(rid, "closed", NULL); return; }
    if (typed_reply(rid, id, SSH_FXP_HANDLE, &r)) return;
    if (get_handle(&r, h, &hn)) { closed = 1; reply_err(rid, "bad", NULL); return; }
    JBuf b;
    jb_init(&b);
    jb_raw(&b, "{\"path\":");
    jb_str(&b, real);
    jb_raw(&b, ",\"entries\":[");
    int count = 0, truncated = 0, err = 0;
    static SfEntry e;
    for (;;) {
        uint32_t qid = next_id++;
        sf_req_handle(&out, SSH_FXP_READDIR, qid, h, hn);
        int type;
        uint32_t aid, n, code;
        char msg[160];
        if (send_pkt(&out) || recv_pkt(&r, &type, &aid)) { closed = 1; err = 1; break; }
        if (aid != qid) { closed = 1; err = 1; break; }
        if (type == SSH_FXP_STATUS) { if (sf_status(&r, &code, msg, sizeof msg) || code != SSH_FX_EOF) err = 1; break; }
        if (type != SSH_FXP_NAME || mxr_u32(&r, &n) || n > 100000) { closed = 1; err = 1; break; }
        for (uint32_t i = 0; i < n; i++) {
            int k = sf_name_entry(&r, &e);
            if (k == -1) { closed = 1; err = 1; break; }
            if (k == -2 || !strcmp(e.name, ".") || !strcmp(e.name, "..")) continue;
            if (count >= SFTP_LIST_MAX) { truncated = 1; continue; }
            jb_raw(&b, count++ ? ",{\"name\":" : "{\"name\":");
            jb_str(&b, e.name);
            jb_raw(&b, ",");
            attrs_json(&b, &e.a, e.kind);
            if (e.owner[0]) { jb_raw(&b, ",\"owner\":"); jb_str(&b, e.owner); }
            if (e.group[0]) { jb_raw(&b, ",\"group\":"); jb_str(&b, e.group); }
            jb_raw(&b, "}");
        }
        if (err || b.oom) break;
    }
    if (!closed) close_handle(h, hn);
    jb_printf(&b, "],\"truncated\":%s}", truncated ? "true" : "false");
    if (err || b.oom) reply_err(rid, closed ? "closed" : b.oom ? "toolarge" : "failed", NULL);
    else if (b.n > DOC_INLINE_MAX) {
        int fd = doc_memfd("sftp-list", b.p, b.n);
        if (fd < 0) reply_err(rid, "toolarge", NULL);
        else { doc_send(CHAN, SE_OK, rid, "{\"big\":1}", 9, &fd, 1); close(fd); }
    } else reply(rid, b.p, b.n);
    jb_free(&b);
}

static int stat_of(uint32_t rid, const char *path, SfAttrs *a) {
    uint32_t id = next_id++;
    sf_req_path(&out, SSH_FXP_STAT, id, path);
    MuxRd r;
    if (send_pkt(&out)) { closed = 1; reply_err(rid, "closed", NULL); return -1; }
    if (typed_reply(rid, id, SSH_FXP_ATTRS, &r)) return -1;
    if (sf_get_attrs(&r, a)) { closed = 1; reply_err(rid, "bad", NULL); return -1; }
    return 0;
}

static void do_stat(uint32_t rid, const JVal *q) {
    const char *path = str_of(q, "path");
    SfAttrs a;
    if (!path) { reply_err(rid, "bad", NULL); return; }
    if (stat_of(rid, path, &a)) return;
    JBuf b;
    jb_init(&b);
    jb_raw(&b, "{\"path\":");
    jb_str(&b, path);
    jb_raw(&b, ",");
    attrs_json(&b, &a, a.flags & SSH_FILEXFER_ATTR_PERMISSIONS ? sf_kind(a.perm) : '?');
    jb_raw(&b, "}");
    if (b.oom) reply_err(rid, "toolarge", NULL); else reply(rid, b.p, b.n);
    jb_free(&b);
}

static void simple(uint32_t rid, uint32_t id) {
    if (send_pkt(&out)) { closed = 1; reply_err(rid, "closed", NULL); return; }
    if (!status_reply(rid, id)) reply(rid, "{}", 2);
}

static uint32_t perm_of(const JVal *q, uint32_t dflt) {
    double v = json_num(q, "perm", -1);
    return v >= 0 && v <= 07777 ? (uint32_t)v : dflt;
}

static void do_mkdir(uint32_t rid, const JVal *q) {
    const char *path = str_of(q, "path");
    if (!path) { reply_err(rid, "bad", NULL); return; }
    uint32_t id = next_id++;
    sf_req_mkdir(&out, id, path, perm_of(q, 0755));
    simple(rid, id);
}

static void do_remove(uint32_t rid, const JVal *q) {
    const char *path = str_of(q, "path");
    const JVal *d = json_get(q, "dir");
    if (!path || !strcmp(path, "/")) { reply_err(rid, "bad", NULL); return; }
    uint32_t id = next_id++;
    sf_req_path(&out, d && d->type == J_BOOL && d->num ? SSH_FXP_RMDIR : SSH_FXP_REMOVE, id, path);
    simple(rid, id);
}

static void do_rename(uint32_t rid, const JVal *q) {
    const char *from = str_of(q, "from"), *to = str_of(q, "to");
    if (!from || !to) { reply_err(rid, "bad", NULL); return; }
    uint32_t id = next_id++;
    sf_req_rename(&out, id, from, to);
    simple(rid, id);
}

static void do_chmod(uint32_t rid, const JVal *q) {
    const char *path = str_of(q, "path");
    double p = json_num(q, "perm", -1);
    if (!path || p < 0 || p > 07777) { reply_err(rid, "bad", NULL); return; }
    SfAttrs a = { .flags = SSH_FILEXFER_ATTR_PERMISSIONS, .perm = (uint32_t)p };
    uint32_t id = next_id++;
    sf_req_setstat(&out, id, path, &a);
    simple(rid, id);
}

static void do_chown(uint32_t rid, const JVal *q) {
    const char *path = str_of(q, "path");
    double u = json_num(q, "uid", -1), g = json_num(q, "gid", -1);
    if (!path || u < 0 || g < 0 || u > 4294967294.0 || g > 4294967294.0) { reply_err(rid, "bad", NULL); return; }
    SfAttrs a = { .flags = SSH_FILEXFER_ATTR_UIDGID, .uid = (uint32_t)u, .gid = (uint32_t)g };
    uint32_t id = next_id++;
    sf_req_setstat(&out, id, path, &a);
    simple(rid, id);
}

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

static double last_progress;
static void progress(uint32_t rid, uint64_t done, uint64_t total, int force) {
    double t = now_ms();
    if (!force && t - last_progress < 100) return;
    last_progress = t;
    char j[96];
    int n = snprintf(j, sizeof j, "{\"done\":%llu,\"total\":%llu}", (unsigned long long)done, (unsigned long long)total);
    doc_send(CHAN, SE_PROGRESS, rid, j, (size_t)n, NULL, 0);
}

static int open_file(uint32_t rid, const char *path, uint32_t pflags, const SfAttrs *a, char *h, size_t *hn) {
    uint32_t id = next_id++;
    sf_req_open(&out, id, path, pflags, a);
    MuxRd r;
    if (send_pkt(&out)) { closed = 1; reply_err(rid, "closed", NULL); return -1; }
    if (typed_reply(rid, id, SSH_FXP_HANDLE, &r)) return -1;
    if (get_handle(&r, h, hn)) { closed = 1; reply_err(rid, "bad", NULL); return -1; }
    return 0;
}

typedef struct { uint32_t id; uint64_t off; uint32_t len; } Flight;

/* download: reads pipelined WINDOW deep, each DATA written at its offset (the fd is lord-server's file) */
static void do_get(uint32_t rid, const JVal *q, int fd) {
    const char *path = str_of(q, "path");
    if (!path || fd < 0) { reply_err(rid, "bad", NULL); return; }
    SfAttrs a;
    if (stat_of(rid, path, &a)) return;
    if ((a.flags & SSH_FILEXFER_ATTR_PERMISSIONS) && sf_kind(a.perm) != '-') { reply_err(rid, "failed", "not a file"); return; }
    uint64_t total = a.flags & SSH_FILEXFER_ATTR_SIZE ? a.size : 0;
    char h[SFTP_HANDLE_MAX];
    size_t hn;
    if (open_file(rid, path, SSH_FXF_READ, NULL, h, &hn)) return;
    Flight fl[WINDOW];
    int nf = 0, eof = 0, err = 0;
    uint64_t next = 0, done = 0;
    while (!err && (nf || !eof)) {
        while (!eof && nf < WINDOW) {
            uint32_t id = next_id++;
            sf_req_read(&out, id, h, hn, next, SFTP_CHUNK);
            if (send_pkt(&out)) { closed = err = 1; break; }
            fl[nf++] = (Flight){ id, next, SFTP_CHUNK };
            next += SFTP_CHUNK;
        }
        if (err || !nf) break;
        MuxRd r;
        int type;
        uint32_t id;
        if (recv_pkt(&r, &type, &id)) { closed = err = 1; break; }
        int k = -1;
        for (int i = 0; i < nf; i++) if (fl[i].id == id) k = i;
        if (k < 0) { closed = err = 1; break; }
        Flight f = fl[k];
        fl[k] = fl[--nf];
        if (type == SSH_FXP_STATUS) {
            uint32_t code;
            char msg[160];
            if (sf_status(&r, &code, msg, sizeof msg)) { closed = err = 1; break; }
            if (code == SSH_FX_EOF) { eof = 1; continue; }
            reply_err(rid, code_of(code), msg);
            err = 2;
            break;
        }
        const char *data;
        size_t n;
        if (type != SSH_FXP_DATA || mxr_str(&r, &data, &n) || n > f.len) { closed = err = 1; break; }
        if (n && pwrite(fd, data, n, (off_t)f.off) != (ssize_t)n) { reply_err(rid, "failed", "write"); err = 2; break; }
        done += n;
        if (!n) { eof = 1; continue; }
        if (n < f.len && nf < WINDOW) {                 /* a short read: ask for the rest of that chunk */
            uint32_t id2 = next_id++;
            sf_req_read(&out, id2, h, hn, f.off + n, f.len - (uint32_t)n);
            if (send_pkt(&out)) { closed = err = 1; break; }
            fl[nf++] = (Flight){ id2, f.off + n, f.len - (uint32_t)n };
        }
        progress(rid, done, total > done ? total : done, 0);
    }
    /* what is still in flight is read and dropped, so the next request finds the stream clean */
    while (!closed && nf) {
        MuxRd r;
        int type;
        uint32_t id;
        if (recv_pkt(&r, &type, &id)) { closed = 1; break; }
        for (int i = 0; i < nf; i++) if (fl[i].id == id) { fl[i] = fl[--nf]; break; }
    }
    if (!closed) close_handle(h, hn);
    if (err == 1) { reply_err(rid, closed ? "closed" : "bad", NULL); return; }
    if (err) return;
    progress(rid, done, done, 1);
    char j[48];
    reply(rid, j, (size_t)snprintf(j, sizeof j, "{\"bytes\":%llu}", (unsigned long long)done));
}

/* upload: the local file read in order, written pipelined WINDOW deep, each WRITE's STATUS checked */
static void do_put(uint32_t rid, const JVal *q, int fd) {
    const char *path = str_of(q, "path");
    if (!path || fd < 0) { reply_err(rid, "bad", NULL); return; }
    double sz = json_num(q, "size", 0);
    uint64_t total = sz > 0 ? (uint64_t)sz : 0;
    SfAttrs a = { .flags = SSH_FILEXFER_ATTR_PERMISSIONS, .perm = perm_of(q, 0644) };
    char h[SFTP_HANDLE_MAX];
    size_t hn;
    if (open_file(rid, path, SSH_FXF_WRITE | SSH_FXF_CREAT | SSH_FXF_TRUNC, &a, h, &hn)) return;
    static uint8_t buf[SFTP_CHUNK];
    uint32_t ids[WINDOW];
    int nf = 0, eof = 0, err = 0;
    uint64_t off = 0, acked = 0;
    while (!err && (nf || !eof)) {
        while (!eof && nf < WINDOW) {
            ssize_t k;
            do k = read(fd, buf, sizeof buf); while (k < 0 && errno == EINTR);
            if (k < 0) { reply_err(rid, "failed", "read"); err = 2; break; }
            if (k == 0) { eof = 1; break; }
            uint32_t id = next_id++;
            sf_req_write(&out, id, h, hn, off, buf, (uint32_t)k);
            if (send_pkt(&out)) { closed = err = 1; break; }
            ids[nf++] = id;
            off += (uint64_t)k;
        }
        if (err || !nf) break;
        MuxRd r;
        int type;
        uint32_t id, code;
        char msg[160];
        if (recv_pkt(&r, &type, &id)) { closed = err = 1; break; }
        int k = -1;
        for (int i = 0; i < nf; i++) if (ids[i] == id) k = i;
        if (k < 0 || type != SSH_FXP_STATUS || sf_status(&r, &code, msg, sizeof msg)) { closed = err = 1; break; }
        ids[k] = ids[--nf];
        if (code != SSH_FX_OK) { reply_err(rid, code_of(code), msg); err = 2; break; }
        acked += SFTP_CHUNK;
        uint64_t shown = acked < off ? acked : off;
        progress(rid, shown, total > shown ? total : shown, 0);
    }
    while (!closed && nf) {
        MuxRd r;
        int type;
        uint32_t id;
        if (recv_pkt(&r, &type, &id)) { closed = 1; break; }
        for (int i = 0; i < nf; i++) if (ids[i] == id) { ids[i] = ids[--nf]; break; }
    }
    if (!closed) close_handle(h, hn);
    if (err == 1) { reply_err(rid, closed ? "closed" : "bad", NULL); return; }
    if (err) return;
    progress(rid, off, off, 1);
    char j[48];
    reply(rid, j, (size_t)snprintf(j, sizeof j, "{\"bytes\":%llu}", (unsigned long long)off));
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    prctl(PR_SET_DUMPABLE, 0);
    SandboxHelperCfg cfg = {
        .name = "lordos-sftp",              /* cgroup scope lordos-sftp.scope */
        .ro_paths = NULL,
        .extra_syscalls = (int[]){ -1 },
        .allow_ioctl_classes = 0,
        .mem_max_mb = 256,
        .ro_dir_fds = NULL,
        .rw_dir_fds = NULL,
    };
    sandbox_apply_helper(&cfg);
    /* from here on, bytes from the server: INIT, then VERSION */
    mxb_reset(&out);
    mxb_u8(&out, SSH_FXP_INIT);
    mxb_u32(&out, 3);
    MuxRd r;
    int type;
    uint32_t ver;
    if (send_pkt(&out) || recv_pkt(&r, &type, &ver) || type != SSH_FXP_VERSION || ver < 3) {
        reply_err(0, "unsupported", "sftp");
        return 1;
    }
    char hello[40];
    reply(0, hello, (size_t)snprintf(hello, sizeof hello, "{\"version\":%u}", ver));
    while (!closed) {
        DocMsg m;
        int k = doc_recv(CHAN, &m);
        if (k) return k == 1 ? 0 : 1;
        JDoc d;
        uint32_t rid = m.h.id;
        int fd = m.nfds ? m.fds[0] : -1;
        if (m.h.op == SP_END) { doc_msg_free(&m); return 0; }
        if (!m.json || json_parse(m.json, m.len, &d, NULL, 0) || !d.root || d.root->type != J_OBJ) {
            reply_err(rid, "bad", NULL);
            doc_msg_free(&m);
            continue;
        }
        switch (m.h.op) {
        case SP_LIST: do_list(rid, d.root); break;
        case SP_STAT: do_stat(rid, d.root); break;
        case SP_MKDIR: do_mkdir(rid, d.root); break;
        case SP_REMOVE: do_remove(rid, d.root); break;
        case SP_RENAME: do_rename(rid, d.root); break;
        case SP_CHMOD: do_chmod(rid, d.root); break;
        case SP_CHOWN: do_chown(rid, d.root); break;
        case SP_GET: do_get(rid, d.root, fd); break;
        case SP_PUT: do_put(rid, d.root, fd); break;
        default: reply_err(rid, "unsupported", NULL); break;
        }
        json_free(&d);
        doc_msg_free(&m);
    }
    return 0;
}
