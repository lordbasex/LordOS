/* vault.c — axia-vault: the passwords and keyring helper (see docs/PASSWORDS.md).
 *
 * axia-server (vclient.c) launches it from a fixed path (next to its own binary), without
 * arguments, with the control channel on fd 3. Before reading a single vault:
 *   - no core dumps or ptrace (PR_SET_DUMPABLE 0, RLIMIT_CORE 0), it dies with axia-server;
 *   - it receives the vaults folder (an fd) over the control channel and, if there is one, the
 *     socket already connected to the session's D-Bus bus; and confines itself with
 *     sandbox_apply_helper: no network, no new sockets, no exec, no files except that folder
 *     (and the fds passed to it later).
 * Decrypted data lives only in the secrets arena (smem.h: mlock, MADV_DONTDUMP, wiped on
 * lock). It serves several channels: the control one (axia-server) and those of the Passwords app.
 * Slow work that waits for a person (touching the FIDO2 key) runs in a separate thread; Argon2id
 * (≈0.5–1 s) runs in the loop. */
#include "vault_int.h"
#include "smem.h"
#include "vcrypto.h"
#include "vgen.h"
#include "totp.h"
#include "vimport.h"
#include "vstb.h"
#include "qr.h"
#include "ctap2.h"
#include "../sec/sandbox.h"
#include "monocypher.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define CTL_FD 3
#define MAX_CHANS 8

extern const char _binary_words_txt_start[], _binary_words_txt_end[];
extern const char _binary_common_txt_start[], _binary_common_txt_end[];

Vault vaults[V_COUNT] = { { .file = "contrasenas.lvault", .fd = -1, .autolock_s = 300 }, { .file = "login.lvault", .fd = -1 } };
int vault_dirfd = -1;
static Chan chans[MAX_CHANS];
static int nchans;
static int fido_fd = -1;
static char fido_name[128];
static int debug;

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static int64_t now_s(void) { return (int64_t)time(NULL); }

/* ================= replies ================= */

static const VwAlloc smalloc = { sm_alloc, sm_realloc, sm_free };

void reply(Chan *c, uint32_t id, VjB *b) {
    if (b->oom) { vb_free(b); reply_err(c, id, "memory"); return; }
    vw_send(c->fd, VW_REP, id, b->p ? b->p : "{}", b->p ? b->n : 2, NULL, 0);
    vb_free(b);
}

void reply_err(Chan *c, uint32_t id, const char *code) {
    char buf[200];
    int n = snprintf(buf, sizeof buf, "{\"error\":\"%s\"}", code);
    vw_send(c->fd, VW_REP, id, buf, (size_t)n, NULL, 0);
}

void reply_ok(Chan *c, uint32_t id) { vw_send(c->fd, VW_REP, id, "{\"ok\":true}", 11, NULL, 0); }

void broadcast_event(const char *json) {
    for (int i = 0; i < nchans; i++) vw_send(chans[i].fd, VW_EVT, 0, json, strlen(json), NULL, 0);
}

static void event_v(const char *ev, int v) {
    char b[96];
    snprintf(b, sizeof b, "{\"ev\":\"%s\",\"v\":\"%s\"}", ev, v == V_MAIN ? "main" : "login");
    broadcast_event(b);
}

/* ================= vaults ================= */

static uint8_t *read_fd_all(int fd, size_t max, size_t *len, int secure) {
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || (uint64_t)st.st_size > max) return NULL;
    size_t n = (size_t)st.st_size;
    uint8_t *b = secure ? sm_alloc(n + 1) : malloc(n + 1);
    if (!b) return NULL;
    size_t got = 0;
    while (got < n) {
        ssize_t r = pread(fd, b + got, n - got, (off_t)got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        got += (size_t)r;
    }
    if (got != n) { if (secure) sm_free(b); else free(b); return NULL; }
    b[n] = 0;
    *len = n;
    return b;
}

static uint8_t *read_stream(int fd, size_t max, size_t *len) {
    /* an fd that may not be a regular file (a powerbox pipe): read up to the end */
    size_t cap = 65536, n = 0;
    uint8_t *b = sm_alloc(cap);
    if (!b) return NULL;
    for (;;) {
        if (n + 1 >= cap) {
            if (cap >= max) { sm_free(b); return NULL; }
            uint8_t *nb = sm_realloc(b, cap * 2);
            if (!nb) { sm_free(b); return NULL; }
            b = nb; cap *= 2;
        }
        ssize_t r = read(fd, b + n, cap - n - 1);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) { sm_free(b); return NULL; }
        if (r == 0) break;
        n += (size_t)r;
    }
    b[n] = 0;
    *len = n;
    return b;
}

/* reads the file header (without unlocking): exists, slots */
static void vault_probe(Vault *v) {
    vf_header_free(&v->h);
    memset(&v->h, 0, sizeof v->h);
    v->exists = 0;
    int fd = openat(vault_dirfd, v->file, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    size_t len;
    uint8_t *d = read_fd_all(fd, VF_MAX_BODY + (512u << 20), &len, 0);
    close(fd);
    if (!d) return;
    if (vf_parse(d, len, &v->h) == VF_OK) v->exists = 1;
    else v->exists = -1;                          /* exists but cannot be read */
    free(d);
}

void vault_lock(Vault *v) {
    if (!v->unlocked) return;
    vm_free(&v->m);
    sm_free(v->vk);
    v->vk = NULL;
    for (int i = 0; i < v->npend; i++) free(v->pend[i].cipher);
    free(v->pend);
    v->pend = NULL;
    v->npend = 0;
    if (v->fd >= 0) close(v->fd);
    v->fd = -1;
    v->unlocked = 0;
    event_v("locked", (int)(v - vaults));
    if (v == &vaults[V_LOGIN]) keyring_lock_changed();
}

static const char *slot_type_name(int t) {
    switch (t) {
    case VS_PASSWORD: return "password";
    case VS_KEYFILE: return "keyfile";
    case VS_PASSKEY: return "passkey";
    case VS_FIDO2: return "fido";
    case VS_RECOVERY: return "recovery";
    case VS_SESSION: return "session";
    }
    return "?";
}

static int slot_type_of(const char *s) {
    if (!s) return 0;
    if (!strcmp(s, "password")) return VS_PASSWORD;
    if (!strcmp(s, "keyfile")) return VS_KEYFILE;
    if (!strcmp(s, "passkey")) return VS_PASSKEY;
    if (!strcmp(s, "fido")) return VS_FIDO2;
    if (!strcmp(s, "recovery")) return VS_RECOVERY;
    if (!strcmp(s, "session")) return VS_SESSION;
    return 0;
}

/* unlocks with the factors; err receives the code for the interface */
int vault_unlock_with(Vault *v, int type, const VFactors *f, char *err, size_t errn) {
    if (v->unlocked) return 0;
    int fd = openat(vault_dirfd, v->file, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { snprintf(err, errn, "not-found"); return -1; }
    size_t len;
    uint8_t *d = read_fd_all(fd, VF_MAX_BODY + (512u << 20), &len, 0);
    if (!d) { close(fd); snprintf(err, errn, "io"); return -1; }
    VHeader h;
    int r = vf_parse(d, len, &h);
    if (r != VF_OK) { free(d); close(fd); snprintf(err, errn, "%s", vf_strerror(r)); return -1; }
    uint8_t *vk = sm_alloc(32);
    int slot = -1;
    double t0 = now_ms();
    r = vk ? vf_open_slot(&h, type, f, vk, &slot) : VF_ENOMEM;
    double kdf_ms = now_ms() - t0;
    if (r != VF_OK) {
        sm_free(vk); vf_header_free(&h); free(d); close(fd);
        snprintf(err, errn, "%s", r == VF_EBADKEY || r == VF_ENOSLOT ? "bad-key" : vf_strerror(r));
        return -1;
    }
    uint8_t *plain;
    size_t plen;
    r = vf_read_body(d, len, &h, vk, &plain, &plen);
    free(d);
    if (r != VF_OK) { sm_free(vk); vf_header_free(&h); close(fd); snprintf(err, errn, "%s", vf_strerror(r)); return -1; }
    VModel m;
    if (vm_load(&m, (const char *)plain, plen)) { sm_free(plain); sm_free(vk); vf_header_free(&h); close(fd); snprintf(err, errn, "corrupt"); return -1; }
    sm_free(plain);
    vf_header_free(&v->h);
    v->h = h;
    v->vk = vk;
    v->m = m;
    v->fd = fd;
    v->unlocked = 1;
    v->exists = 1;
    v->last_use = now_ms();
    if (v == &vaults[V_MAIN]) {
        /* the auto-lock comes from the vault's settings */
        Vj j;
        if (m.meta && !vj_parse(&j, m.meta, strlen(m.meta))) {
            v->autolock_s = (int)vj_getn(&j, 0, "autolock", 300);
            vj_free(&j);
        }
    }
    if (debug) fprintf(stderr, "axia-vault: %s abierta (ranura %d, %s, %.0f ms de KDF, %d entradas)\n",
                       v->file, slot, slot_type_name(h.slots[slot].type), kdf_ms, m.n);
    event_v("unlocked", (int)(v - vaults));
    if (v == &vaults[V_LOGIN]) keyring_lock_changed();
    return 0;
}

int vault_save(Vault *v) {
    if (!v->unlocked) return VF_EIO;
    size_t plen;
    char *plain = vm_dump(&v->m, &plen);
    if (!plain) return VF_ENOMEM;
    /* attachments: those of the entries; new (in memory) or from the current file */
    int total = 0;
    for (int i = 0; i < v->m.n; i++) total += v->m.e[i]->natt;
    VAttOut *outs = calloc((size_t)(total ? total : 1), sizeof *outs);
    int no = 0;
    for (int i = 0; outs && i < v->m.n; i++) {
        VEntry *e = v->m.e[i];
        for (int a = 0; a < e->natt; a++) {
            VAtt *at = &e->att[a];
            VAttOut *o = &outs[no];
            memcpy(o->id, at->id, 16);
            memcpy(o->nonce, at->nonce, 24);
            memcpy(o->mac, at->mac, 16);
            int found = 0;
            for (int p = 0; p < v->npend && !found; p++)
                if (!memcmp(v->pend[p].id, at->id, 16)) { o->cipher = v->pend[p].cipher; o->len = v->pend[p].len; found = 1; }
            for (int p = 0; p < v->h.natts && !found; p++)
                if (!memcmp(v->h.atts[p].id, at->id, 16)) { o->srcfd = v->fd; o->off = v->h.atts[p].off; o->len = v->h.atts[p].len; found = 1; }
            if (found) no++;
        }
    }
    int r = outs ? vf_save(vault_dirfd, v->file, &v->h, v->vk, (const uint8_t *)plain, plen, outs, no) : VF_ENOMEM;
    free(outs);
    sm_free(plain);
    if (r != VF_OK) return r;
    /* reopen the new file (attachments now come from there) */
    int fd = openat(vault_dirfd, v->file, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        size_t len;
        uint8_t *d = read_fd_all(fd, VF_MAX_BODY + (512u << 20), &len, 0);
        VHeader h;
        if (d && vf_parse(d, len, &h) == VF_OK) {
            vf_header_free(&v->h);
            v->h = h;
            if (v->fd >= 0) close(v->fd);
            v->fd = fd;
            fd = -1;
            for (int i = 0; i < v->npend; i++) free(v->pend[i].cipher);
            free(v->pend);
            v->pend = NULL;
            v->npend = 0;
        }
        free(d);
        if (fd >= 0) close(fd);
    }
    v->m.dirty = 0;
    v->exists = 1;
    return VF_OK;
}

int vault_create(Vault *v, int type, const VFactors *f, const VKdf *kdf, const char *label, const char *meta_json, char *recovery_out) {
    if (v->unlocked) vault_lock(v);
    VHeader h;
    vf_header_new(&h);
    uint8_t *vk = sm_alloc(32);
    if (!vk) return VF_ENOMEM;
    vc_random(vk, 32);
    VSlot s;
    memset(&s, 0, sizeof s);
    if (type == VS_FIDO2) s = h.slots[0];        /* the caller left cred/salt in h.slots[0] */
    int r = vf_make_slot(&h, &s, type, f, vk, kdf, label);
    if (r != VF_OK) { sm_free(vk); return r; }
    h.slots[h.nslots++] = s;
    if (recovery_out) {
        vf_recovery_new(recovery_out);
        VFactors rf = { .recovery = recovery_out };
        VSlot rs;
        memset(&rs, 0, sizeof rs);
        if (vf_make_slot(&h, &rs, VS_RECOVERY, &rf, vk, kdf, "") == VF_OK) h.slots[h.nslots++] = rs;
    }
    vf_header_free(&v->h);
    v->h = h;
    v->vk = vk;
    vm_init(&v->m);
    v->m.meta = sm_strdup(meta_json ? meta_json : "{}");
    v->m.cats = sm_strdup("[]");
    v->m.tags = sm_strdup("[]");
    v->unlocked = 1;
    v->fd = -1;
    r = vault_save(v);
    if (r != VF_OK) { vault_lock(v); return r; }
    v->last_use = now_ms();
    event_v("unlocked", (int)(v - vaults));
    return VF_OK;
}

/* ================= FIDO2 in a thread ================= */

enum { FJ_UNLOCK = 1, FJ_CREATE, FJ_ADD };
typedef struct {
    int busy, kind, v;
    Chan *c; uint32_t id;
    char pin[72];
    int fd;
    VKdf kdf;
    char label[64];
    /* result */
    int err;
    uint8_t secret[32];
    uint8_t cred[FIDO_CRED_MAX]; size_t cred_len;
    uint8_t hmac_salt[32];
    int slot_tried;
} FidoJob;

static FidoJob fj;
static int fpipe[2] = { -1, -1 };
static volatile int fido_touch_note;

static void fido_status(void *ud, int st) {
    (void)ud;
    if (st == FIDO_ST_TOUCH && !fido_touch_note) { fido_touch_note = 1; char b = 't'; if (write(fpipe[1], &b, 1) < 0) { /* */ } }
}

static void *fido_thread(void *arg) {
    (void)arg;
    const char *pin = fj.pin[0] ? fj.pin : NULL;
    if (fj.kind == FJ_UNLOCK) {
        fj.err = FIDO_ERR_NO_CREDENTIAL;
        Vault *v = &vaults[fj.v];
        for (int i = 0; i < v->h.nslots; i++) {
            VSlot *s = &v->h.slots[i];
            if (s->type != VS_FIDO2) continue;
            fj.slot_tried = i;
            fj.err = fido_hmac_secret(fj.fd, s->fido_pin ? pin : NULL, s->cred, s->cred_len, s->hmac_salt, fj.secret, 30000, fido_status, NULL);
            if (fj.err == FIDO_OK || (fj.err != FIDO_ERR_NO_CREDENTIAL)) break;
        }
    } else {
        fj.cred_len = 0;
        fj.err = fido_make_credential(fj.fd, pin, fj.cred, &fj.cred_len, 30000, fido_status, NULL);
        if (fj.err == FIDO_OK) {
            vc_random(fj.hmac_salt, 32);
            fido_touch_note = 0;
            fj.err = fido_hmac_secret(fj.fd, pin, fj.cred, fj.cred_len, fj.hmac_salt, fj.secret, 30000, fido_status, NULL);
        }
    }
    char b = 'd';
    if (write(fpipe[1], &b, 1) < 0) { /* */ }
    return NULL;
}

static const char *fido_code(int e) {
    const char *s = fido_strerror(e);
    return s ? s : "fido";
}

/* the thread finished: complete the request in the loop */
static void fido_done(void) {
    fj.busy = 0;
    Vault *v = &vaults[fj.v];
    Chan *c = fj.c;
    if (fj.err != FIDO_OK) { char e[64]; snprintf(e, sizeof e, "fido-%s", fido_code(fj.err)); reply_err(c, fj.id, e); goto out; }
    VFactors f = { .fido = fj.secret };
    if (fj.kind == FJ_UNLOCK) {
        char err[64];
        if (vault_unlock_with(v, VS_FIDO2, &f, err, sizeof err)) reply_err(c, fj.id, err); else reply_ok(c, fj.id);
    } else {
        VSlot s;
        memset(&s, 0, sizeof s);
        memcpy(s.cred, fj.cred, fj.cred_len);
        s.cred_len = (uint8_t)fj.cred_len;
        memcpy(s.hmac_salt, fj.hmac_salt, 32);
        s.fido_pin = fj.pin[0] ? 1 : 0;
        if (fj.kind == FJ_CREATE) {
            VHeader *h = &v->h;
            memset(h, 0, sizeof *h);
            h->slots[0] = s;
            char rec[48];
            char meta[64];
            snprintf(meta, sizeof meta, "{\"autolock\":300,\"clip\":30}");
            int r = vault_create(v, VS_FIDO2, &f, &fj.kdf, fj.label, meta, rec);
            if (r != VF_OK) reply_err(c, fj.id, vf_strerror(r));
            else {
                VjB b; vb_init(&b); vb_open(&b, '{'); vb_kb(&b, "ok", 1); vb_ks(&b, "recovery", rec); vb_close(&b, '}');
                reply(c, fj.id, &b);
            }
            sm_wipe(rec, sizeof rec);
        } else {
            if (!v->unlocked || v->h.nslots >= VF_MAX_SLOTS) { reply_err(c, fj.id, "locked"); goto out; }
            VSlot ns = s;
            int r = vf_make_slot(&v->h, &ns, VS_FIDO2, &f, v->vk, &fj.kdf, fj.label);
            if (r == VF_OK) { v->h.slots[v->h.nslots++] = ns; r = vault_save(v); }
            if (r != VF_OK) reply_err(c, fj.id, vf_strerror(r)); else reply_ok(c, fj.id);
        }
    }
out:
    sm_wipe(fj.secret, sizeof fj.secret);
    sm_wipe(fj.pin, sizeof fj.pin);
}

static int fido_start(Chan *c, uint32_t id, int kind, int vi, const Vj *j, int o) {
    if (fido_fd < 0) { reply_err(c, id, "no-device"); return 0; }
    if (fj.busy) { reply_err(c, id, "busy"); return 0; }
    memset(&fj, 0, sizeof fj);
    fj.busy = 1; fj.kind = kind; fj.v = vi; fj.c = c; fj.id = id; fj.fd = fido_fd;
    vj_getsbuf(j, o, "pin", fj.pin, sizeof fj.pin);
    vj_getsbuf(j, o, "label", fj.label, sizeof fj.label);
    if (!fj.label[0]) snprintf(fj.label, sizeof fj.label, "%.63s", fido_name[0] ? fido_name : "FIDO2");
    vf_calibrate(700, &fj.kdf, NULL);
    fido_touch_note = 0;
    pthread_t t;
    if (pthread_create(&t, NULL, fido_thread, NULL)) { fj.busy = 0; reply_err(c, id, "thread"); return 0; }
    pthread_detach(t);
    return 0;
}

/* ================= requests ================= */

static int vault_of(const Vj *j) {
    int k = vj_get(j, 0, "v");
    return vj_streq(j, k, "login") ? V_LOGIN : V_MAIN;
}

static void status_json(VjB *b) {
    vb_open(b, '{');
    vb_kb(b, "ok", 1);
    for (int i = 0; i < V_COUNT; i++) {
        Vault *v = &vaults[i];
        vb_key(b, i == V_MAIN ? "main" : "login");
        vb_open(b, '{');
        vb_kb(b, "exists", v->exists == 1);
        vb_kb(b, "unreadable", v->exists == -1);
        vb_kb(b, "unlocked", v->unlocked);
        if (v->unlocked) vb_ki(b, "count", v->m.n);
        vb_ki(b, "autolock", v->autolock_s);
        vb_key(b, "slots");
        vb_open(b, '[');
        for (int s = 0; s < v->h.nslots; s++) {
            const VSlot *sl = &v->h.slots[s];
            char idh[17];
            for (int k = 0; k < 8; k++) snprintf(idh + 2 * k, 3, "%02x", sl->id[k]);
            vb_sep(b);
            vb_open(b, '{');
            vb_ks(b, "id", idh);
            vb_ks(b, "type", slot_type_name(sl->type));
            vb_ks(b, "label", sl->label);
            vb_kn(b, "created", (double)sl->created);
            if (sl->kdf == VK_ARGON2ID) { vb_ki(b, "m", sl->p.m_kib); vb_ki(b, "t", sl->p.t_cost); }
            if (sl->type == VS_FIDO2) vb_kb(b, "pin", sl->fido_pin);
            vb_close(b, '}');
        }
        vb_close(b, ']');
        vb_close(b, '}');
    }
    vb_kb(b, "fido", fido_fd >= 0);
    if (fido_name[0]) vb_ks(b, "fidoName", fido_name);
    SmStats st;
    sm_stats(&st);
    vb_key(b, "mem");
    vb_open(b, '{');
    vb_ki(b, "inUse", (long long)st.in_use);
    vb_ki(b, "locked", (long long)st.locked);
    vb_kb(b, "allLocked", st.all_locked);
    vb_close(b, '}');
    vb_close(b, '}');
}

/* factors of a request: password, recovery and the key file (fd) */
typedef struct { char *password, *recovery; uint8_t kf[32]; int has_kf; } Factors;

static int get_factors(const Vj *j, const VMsg *m, int kf_index, Factors *F) {
    memset(F, 0, sizeof *F);
    F->password = vj_gets(j, 0, "password");
    F->recovery = vj_gets(j, 0, "recovery");
    if (kf_index >= 0 && kf_index < m->nfds && m->fds[kf_index] >= 0) {
        size_t len;
        uint8_t *d = read_stream(m->fds[kf_index], 16u << 20, &len);
        if (!d) return -1;
        if (len < 32) { sm_free(d); return -2; }               /* too short to be a key */
        vf_keyfile_hash(d, len, F->kf);
        sm_free(d);
        F->has_kf = 1;
    }
    return 0;
}

static void factors_free(Factors *F) { sm_free(F->password); sm_free(F->recovery); sm_wipe(F->kf, sizeof F->kf); }

static VFactors vfactors(const Factors *F) {
    VFactors f = { .password = F->password && F->password[0] ? F->password : NULL, .keyfile = F->has_kf ? F->kf : NULL,
                   .recovery = F->recovery && F->recovery[0] ? F->recovery : NULL };
    return f;
}

static int write_all_fd(int fd, const void *p, size_t n) {
    const uint8_t *b = p;
    while (n) {
        ssize_t w = write(fd, b, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        b += w; n -= (size_t)w;
    }
    return 0;
}

static void op_create(Chan *c, uint32_t id, const Vj *j, const VMsg *m) {
    int vi = vault_of(j);
    Vault *v = &vaults[vi];
    vault_probe(v);
    if (v->exists && !vj_getb(j, 0, "replace", 0)) { reply_err(c, id, "exists"); return; }
    char method[16] = "";
    vj_getsbuf(j, 0, "method", method, sizeof method);
    int type = slot_type_of(method);
    if (!type || type == VS_RECOVERY || type == VS_SESSION) { reply_err(c, id, "bad-method"); return; }
    if (type == VS_FIDO2) { fido_start(c, id, FJ_CREATE, vi, j, 0); return; }
    Factors F;
    /* key file: "newkey" → generated and written to fds[0]; otherwise read from fds[0] */
    int newkey = vj_getb(j, 0, "newkey", 0);
    uint8_t key[64];
    if ((type == VS_KEYFILE || type == VS_PASSKEY) && newkey) {
        if (m->nfds < 1) { reply_err(c, id, "no-keyfile"); return; }
        vc_random(key, sizeof key);
        if (write_all_fd(m->fds[0], key, sizeof key) || fsync(m->fds[0]) < 0) {
            if (errno != EINVAL) { sm_wipe(key, sizeof key); reply_err(c, id, "keyfile-write"); return; }
        }
        get_factors(j, m, -1, &F);
        vf_keyfile_hash(key, sizeof key, F.kf);
        F.has_kf = 1;
        sm_wipe(key, sizeof key);
    } else if (get_factors(j, m, (type == VS_KEYFILE || type == VS_PASSKEY) ? 0 : -1, &F)) { reply_err(c, id, "keyfile-read"); return; }
    VFactors f = vfactors(&F);
    if ((type == VS_PASSWORD || type == VS_PASSKEY) && (!f.password || strlen(f.password) < 1)) { factors_free(&F); reply_err(c, id, "no-password"); return; }
    if ((type == VS_KEYFILE || type == VS_PASSKEY) && !f.keyfile) { factors_free(&F); reply_err(c, id, "no-keyfile"); return; }
    VKdf kdf;
    int ms = 0;
    int target = (int)vj_getn(j, 0, "targetMs", 700);
    vf_calibrate(target, &kdf, &ms);
    char rec[48];
    char meta[96];
    snprintf(meta, sizeof meta, "{\"autolock\":%d,\"clip\":30}", vi == V_MAIN ? 300 : 0);
    char label[64] = "";
    vj_getsbuf(j, 0, "label", label, sizeof label);
    int r = vault_create(v, type, &f, &kdf, label, meta, vi == V_MAIN ? rec : NULL);
    factors_free(&F);
    if (r != VF_OK) { reply_err(c, id, vf_strerror(r)); return; }
    VjB b;
    vb_init(&b);
    vb_open(&b, '{');
    vb_kb(&b, "ok", 1);
    if (vi == V_MAIN) vb_ks(&b, "recovery", rec);
    vb_ki(&b, "kdfMs", ms);
    vb_ki(&b, "m", kdf.m_kib);
    vb_ki(&b, "t", kdf.t_cost);
    vb_close(&b, '}');
    sm_wipe(rec, sizeof rec);
    reply(c, id, &b);
}

static void op_unlock(Chan *c, uint32_t id, const Vj *j, const VMsg *m) {
    int vi = vault_of(j);
    Vault *v = &vaults[vi];
    if (v->unlocked) { reply_ok(c, id); return; }
    char method[16] = "";
    vj_getsbuf(j, 0, "method", method, sizeof method);
    int type = slot_type_of(method);
    if (type == VS_FIDO2) { fido_start(c, id, FJ_UNLOCK, vi, j, 0); return; }
    Factors F;
    int kfi = (type == VS_KEYFILE || type == VS_PASSKEY) ? 0 : -1;
    int g = get_factors(j, m, kfi, &F);
    if (g) { factors_free(&F); reply_err(c, id, g == -2 ? "keyfile-short" : "keyfile-read"); return; }
    VFactors f = vfactors(&F);
    char err[64];
    double t0 = now_ms();
    int r = vault_unlock_with(v, type, &f, err, sizeof err);
    double ms = now_ms() - t0;
    factors_free(&F);
    if (r) { reply_err(c, id, err); return; }
    VjB b;
    vb_init(&b);
    vb_open(&b, '{');
    vb_kb(&b, "ok", 1);
    vb_kn(&b, "ms", round(ms));
    vb_close(&b, '}');
    reply(c, id, &b);
}

static int need_unlocked(Chan *c, uint32_t id, Vault *v) {
    if (!v->unlocked) { reply_err(c, id, "locked"); return 0; }
    v->last_use = now_ms();
    return 1;
}

static void op_list(Chan *c, uint32_t id, Vault *v) {
    VjB b;
    vb_init(&b);
    vb_open(&b, '{');
    vb_kb(&b, "ok", 1);
    vb_key(&b, "meta"); vb_raw(&b, v->m.meta ? v->m.meta : "{}");
    vb_key(&b, "cats"); vb_raw(&b, v->m.cats ? v->m.cats : "[]");
    vb_key(&b, "tags"); vb_raw(&b, v->m.tags ? v->m.tags : "[]");
    vb_key(&b, "entries");
    vm_list_json(&b, &v->m);
    vb_close(&b, '}');
    reply(c, id, &b);
}

static void list_item(VjB *b, const VEntry *e) {
    VModel one = { .e = (VEntry **)&e, .n = 1 };
    VjB t;
    vb_init(&t);
    vm_list_json(&t, &one);
    /* "[{…}]" → "{…}" */
    if (t.p && t.n > 2) vb_rawn(b, t.p + 1, t.n - 2); else vb_raw(b, "null");
    vb_free(&t);
}

static void after_change(Vault *v, Chan *c, uint32_t id, const VEntry *e) {
    int r = vault_save(v);
    if (r != VF_OK) { reply_err(c, id, vf_strerror(r)); return; }
    VjB b;
    vb_init(&b);
    vb_open(&b, '{');
    vb_kb(&b, "ok", 1);
    if (e) { vb_ks(&b, "id", e->id); vb_key(&b, "item"); list_item(&b, e); }
    vb_close(&b, '}');
    reply(c, id, &b);
    event_v("changed", (int)(v - vaults));
}

static void op_report(Chan *c, uint32_t id, Vault *v, const VMsg *m) {
    int64_t now = now_s();
    int hibp = m->nfds > 0 ? m->fds[0] : -1;
    VjB b;
    vb_init(&b);
    vb_open(&b, '{');
    vb_kb(&b, "ok", 1);
    vb_key(&b, "weak");
    vb_open(&b, '[');
    for (int i = 0; i < v->m.n; i++) {
        VEntry *e = v->m.e[i];
        const char *pw = vm_field(e, "password");
        if (e->trash || !pw || !pw[0]) continue;
        double bits;
        int sc = vg_strength(pw, &bits);
        int rank = vg_common_rank(pw);
        if (sc > 1 && !rank) continue;
        vb_sep(&b);
        vb_open(&b, '{');
        vb_ks(&b, "id", e->id);
        vb_ki(&b, "score", sc);
        vb_kn(&b, "bits", round(bits));
        if (rank) vb_kb(&b, "common", 1);
        vb_close(&b, '}');
    }
    vb_close(&b, ']');
    /* reused: grouped by the password's hash */
    vb_key(&b, "reused");
    vb_open(&b, '[');
    char *seen = calloc((size_t)(v->m.n ? v->m.n : 1), 1);
    static const uint8_t zero[16];
    for (int i = 0; seen && i < v->m.n; i++) {
        VEntry *e = v->m.e[i];
        if (seen[i] || e->trash || !memcmp(e->pwhash, zero, 16)) continue;
        int cnt = 0;
        for (int k = i + 1; k < v->m.n; k++) if (!seen[k] && !v->m.e[k]->trash && !memcmp(v->m.e[k]->pwhash, e->pwhash, 16)) cnt++;
        if (!cnt) continue;
        vb_sep(&b);
        vb_open(&b, '[');
        vb_sep(&b); vb_str(&b, e->id);
        for (int k = i + 1; k < v->m.n; k++)
            if (!seen[k] && !v->m.e[k]->trash && !memcmp(v->m.e[k]->pwhash, e->pwhash, 16)) { seen[k] = 1; vb_sep(&b); vb_str(&b, v->m.e[k]->id); }
        vb_close(&b, ']');
    }
    free(seen);
    vb_close(&b, ']');
    /* old: more days than their reminder (or a year without a reminder) */
    vb_key(&b, "old");
    vb_open(&b, '[');
    for (int i = 0; i < v->m.n; i++) {
        VEntry *e = v->m.e[i];
        const char *pw = vm_field(e, "password");
        if (e->trash || !pw || !pw[0]) continue;
        int64_t since = e->pwch ? e->pwch : e->created;
        int days = (int)((now - since) / 86400);
        int limit = e->rotate > 0 ? e->rotate : 365;
        if (since <= 0 || days < limit) continue;
        vb_sep(&b);
        vb_open(&b, '{');
        vb_ks(&b, "id", e->id);
        vb_ki(&b, "days", days);
        vb_ki(&b, "limit", limit);
        vb_close(&b, '}');
    }
    vb_close(&b, ']');
    if (hibp >= 0) {
        vb_key(&b, "breached");
        vb_open(&b, '[');
        for (int i = 0; i < v->m.n; i++) {
            VEntry *e = v->m.e[i];
            const char *pw = vm_field(e, "password");
            if (e->trash || !pw || !pw[0]) continue;
            long n = vg_hibp_lookup(hibp, pw);
            if (n < 0) break;
            if (!n) continue;
            vb_sep(&b);
            vb_open(&b, '{');
            vb_ks(&b, "id", e->id);
            vb_ki(&b, "count", n);
            vb_close(&b, '}');
        }
        vb_close(&b, ']');
    }
    vb_close(&b, '}');
    reply(c, id, &b);
}

static void op_totp(Chan *c, uint32_t id, Vault *v, const Vj *j) {
    int64_t now = now_s();
    int ids = vj_get(j, 0, "ids");
    VjB b;
    vb_init(&b);
    vb_open(&b, '{');
    vb_kb(&b, "ok", 1);
    vb_kn(&b, "now", (double)now);
    vb_key(&b, "codes");
    vb_open(&b, '[');
    for (int i = 0; i < v->m.n; i++) {
        VEntry *e = v->m.e[i];
        if (e->trash) continue;
        if (vj_type(j, ids) == VJ_ARR) {
            int want = 0;
            for (int a = 0, k = ids + 1; a < j->t[ids].count && !want; a++, k = j->t[k].next) if (vj_streq(j, k, e->id)) want = 1;
            if (!want) continue;
        }
        for (int f = 0; f < e->nf; f++) {
            if (strcmp(e->f[f].t, "totp") || !e->f[f].v[0]) continue;
            Totp t;
            if (totp_parse(e->f[f].v, &t)) continue;
            int rem = 0;
            char code[12];
            if (t.hotp) { snprintf(code, sizeof code, "—"); rem = 0; }
            else totp_format(&t, totp_code(&t, (uint64_t)now, &rem), code);
            vb_sep(&b);
            vb_open(&b, '{');
            vb_ks(&b, "id", e->id);
            vb_ks(&b, "k", e->f[f].k);
            vb_ks(&b, "code", code);
            vb_ki(&b, "period", t.period);
            vb_ki(&b, "remaining", rem);
            vb_ki(&b, "digits", t.digits);
            if (t.issuer[0]) vb_ks(&b, "issuer", t.issuer);
            if (t.account[0]) vb_ks(&b, "account", t.account);
            vb_close(&b, '}');
            totp_wipe(&t);
            sm_wipe(code, sizeof code);
        }
    }
    vb_close(&b, ']');
    vb_close(&b, '}');
    reply(c, id, &b);
}

/* QR: from a given text or an entry's Wi-Fi; replies with the rows as hex */
static void wifi_escape(VjB *b, const char *s) {
    for (; s && *s; s++) {
        if (strchr("\\;,:\"", *s)) vb_raw(b, "\\");
        char t[2] = { *s, 0 };
        vb_raw(b, t);
    }
}

static void op_qr(Chan *c, uint32_t id, Vault *v, const Vj *j) {
    VjB txt;
    vb_init(&txt);
    char eid[40] = "";
    vj_getsbuf(j, 0, "id", eid, sizeof eid);
    if (eid[0]) {
        if (!need_unlocked(c, id, v)) { vb_free(&txt); return; }
        VEntry *e = vm_find(&v->m, eid);
        if (!e) { vb_free(&txt); reply_err(c, id, "not-found"); return; }
        const char *ssid = vm_field(e, "ssid"), *pw = vm_field(e, "password"), *sec = vm_field(e, "security"), *hid = vm_field(e, "hidden");
        const char *T = !sec || !sec[0] ? "WPA" : (!strncasecmp(sec, "wep", 3) ? "WEP" : !strncasecmp(sec, "none", 4) || !strncasecmp(sec, "open", 4) || !strcmp(sec, "nopass") ? "nopass" : "WPA");
        vb_raw(&txt, "WIFI:T:");
        vb_raw(&txt, T);
        vb_raw(&txt, ";S:");
        wifi_escape(&txt, ssid);
        if (strcmp(T, "nopass")) { vb_raw(&txt, ";P:"); wifi_escape(&txt, pw); }
        if (hid && (!strcmp(hid, "true") || !strcmp(hid, "1"))) vb_raw(&txt, ";H:true");
        vb_raw(&txt, ";;");
    } else {
        char *t = vj_gets(j, 0, "text");
        if (!t) { vb_free(&txt); reply_err(c, id, "no-text"); return; }
        vb_raw(&txt, t);
        sm_free(t);
    }
    uint8_t *mods = sm_alloc(QR_MAX_SIZE * QR_MAX_SIZE);
    int size = 0;
    int ver = mods && txt.p ? qr_encode((const uint8_t *)txt.p, txt.n, QR_ECC_M, mods, &size) : -1;
    vb_free(&txt);
    if (ver < 0) { sm_free(mods); reply_err(c, id, "qr-too-long"); return; }
    VjB b;
    vb_init(&b);
    vb_open(&b, '{');
    vb_kb(&b, "ok", 1);
    vb_ki(&b, "size", size);
    vb_ki(&b, "version", ver);
    vb_key(&b, "rows");
    vb_open(&b, '[');
    char row[QR_MAX_SIZE / 4 + 2];
    for (int y = 0; y < size; y++) {
        int k = 0;
        for (int x = 0; x < size; x += 4) {
            int nib = 0;
            for (int q = 0; q < 4; q++) nib = nib << 1 | (x + q < size ? mods[y * size + x + q] : 0);
            row[k++] = "0123456789abcdef"[nib];
        }
        row[k] = 0;
        vb_sep(&b);
        vb_str(&b, row);
    }
    vb_close(&b, ']');
    vb_close(&b, '}');
    sm_free(mods);
    reply(c, id, &b);
}

static void op_qr_scan(Chan *c, uint32_t id, const VMsg *m) {
    if (m->nfds < 1) { reply_err(c, id, "no-file"); return; }
    size_t len;
    uint8_t *d = read_stream(m->fds[0], 32u << 20, &len);
    if (!d) { reply_err(c, id, "read"); return; }
    int w, h;
    uint32_t *px = vstb_decode_argb(d, len, &w, &h);
    sm_free(d);
    if (!px) { reply_err(c, id, "not-an-image"); return; }
    uint8_t *out = sm_alloc(4097);
    int n = out ? qr_decode(px, w, h, out, 4096) : -1;
    sm_free(px);
    if (n < 0) { sm_free(out); reply_err(c, id, "no-qr"); return; }
    out[n] = 0;
    VjB b;
    vb_init(&b);
    vb_open(&b, '{');
    vb_kb(&b, "ok", 1);
    vb_ks(&b, "text", (const char *)out);
    Totp t;
    if (!totp_parse((const char *)out, &t)) {
        vb_kb(&b, "totp", 1);
        vb_ks(&b, "issuer", t.issuer);
        vb_ks(&b, "account", t.account);
        totp_wipe(&t);
    }
    vb_close(&b, '}');
    sm_free(out);
    reply(c, id, &b);
}

static void op_generate(Chan *c, uint32_t id, const Vj *j) {
    VGenOpts o = { .length = (int)vj_getn(j, 0, "length", 20), .upper = vj_getb(j, 0, "upper", 1), .lower = vj_getb(j, 0, "lower", 1),
                   .digits = vj_getb(j, 0, "digits", 1), .symbols = vj_getb(j, 0, "symbols", 1), .no_ambiguous = vj_getb(j, 0, "noAmbiguous", 0),
                   .words = (int)vj_getn(j, 0, "words", 0), .capitalize = vj_getb(j, 0, "capitalize", 0), .add_digit = vj_getb(j, 0, "addDigit", 0) };
    if (!vj_getsbuf(j, 0, "sep", o.sep, sizeof o.sep) && vj_get(j, 0, "sep") < 0) snprintf(o.sep, sizeof o.sep, "-");
    char *pw = sm_alloc(512);
    double bits = 0;
    if (!pw || vg_generate(&o, pw, 512, &bits)) { sm_free(pw); reply_err(c, id, "bad-options"); return; }
    double sb;
    int score = vg_strength(pw, &sb);
    if (o.words) score = bits >= 80 ? 4 : bits >= 60 ? 3 : bits >= 36 ? 2 : 1;
    VjB b;
    vb_init(&b);
    vb_open(&b, '{');
    vb_kb(&b, "ok", 1);
    vb_ks(&b, "password", pw);
    vb_kn(&b, "bits", round(bits));
    vb_ki(&b, "score", score);
    vb_close(&b, '}');
    sm_free(pw);
    reply(c, id, &b);
}

static void op_strength(Chan *c, uint32_t id, const Vj *j) {
    char *pw = vj_gets(j, 0, "password");
    double bits = 0;
    int sc = pw ? vg_strength(pw, &bits) : 0;
    int rank = pw ? vg_common_rank(pw) : 0;
    sm_free(pw);
    VjB b;
    vb_init(&b);
    vb_open(&b, '{');
    vb_kb(&b, "ok", 1);
    vb_ki(&b, "score", sc);
    vb_kn(&b, "bits", round(bits));
    vb_kb(&b, "common", rank > 0);
    vb_close(&b, '}');
    reply(c, id, &b);
}

/* ---- attachments ---- */

static int hexbytes(const char *s, uint8_t *out, size_t n) {
    if (!s || strlen(s) != n * 2) return -1;
    for (size_t i = 0; i < n; i++) { unsigned v; if (sscanf(s + 2 * i, "%2x", &v) != 1) return -1; out[i] = (uint8_t)v; }
    return 0;
}

static int att_find(VEntry *e, const char *hid) {
    uint8_t aid[16];
    if (hexbytes(hid, aid, 16)) return -1;
    for (int i = 0; i < e->natt; i++) if (!memcmp(e->att[i].id, aid, 16)) return i;
    return -1;
}

/* decrypts an attachment → smem */
static uint8_t *att_plain(Vault *v, const VAtt *a, size_t *len) {
    const uint8_t *cipher = NULL;
    uint8_t *tmp = NULL;
    size_t clen = 0;
    for (int p = 0; p < v->npend && !cipher; p++) if (!memcmp(v->pend[p].id, a->id, 16)) { cipher = v->pend[p].cipher; clen = v->pend[p].len; }
    for (int p = 0; p < v->h.natts && !cipher; p++) {
        if (memcmp(v->h.atts[p].id, a->id, 16)) continue;
        clen = v->h.atts[p].len;
        tmp = malloc(clen ? clen : 1);
        if (!tmp) return NULL;
        size_t got = 0;
        while (got < clen) {
            ssize_t r = pread(v->fd, tmp + got, clen - got, (off_t)(v->h.atts[p].off + got));
            if (r <= 0) { if (r < 0 && errno == EINTR) continue; break; }
            got += (size_t)r;
        }
        if (got != clen || memcmp(v->h.atts[p].mac, a->mac, 16)) { free(tmp); return NULL; }
        cipher = tmp;
    }
    if (!cipher) return NULL;
    uint8_t *pt = sm_alloc(clen + 1);
    if (pt && vf_att_open(v->vk, v->h.vault_id, a->id, cipher, clen, a->nonce, a->mac, pt) != VF_OK) { sm_free(pt); pt = NULL; }
    free(tmp);
    if (pt) *len = clen;
    return pt;
}

static int att_add_bytes(Vault *v, VEntry *e, const char *name, const char *mime, const uint8_t *data, size_t len) {
    if (len > VF_MAX_ATT || e->natt >= 64) return -1;
    VAtt *na = sm_realloc(e->att, sizeof(VAtt) * (size_t)(e->natt + 1));
    if (!na) return -1;
    e->att = na;
    VAtt *a = &e->att[e->natt];
    memset(a, 0, sizeof *a);
    vc_random(a->id, 16);
    PendAtt *np = realloc(v->pend, sizeof(PendAtt) * (size_t)(v->npend + 1));
    if (!np) return -1;
    v->pend = np;
    PendAtt *p = &v->pend[v->npend];
    p->cipher = malloc(len ? len : 1);
    if (!p->cipher) return -1;
    p->len = len;
    memcpy(p->id, a->id, 16);
    vf_att_seal(v->vk, v->h.vault_id, a->id, data, len, p->cipher, a->nonce, a->mac);
    memcpy(p->nonce, a->nonce, 24);
    memcpy(p->mac, a->mac, 16);
    v->npend++;
    a->name = sm_strdup(name && name[0] ? name : "file");
    a->mime = sm_strdup(mime && mime[0] ? mime : "application/octet-stream");
    a->size = len;
    e->natt++;
    e->modified = now_s();
    v->m.dirty = 1;
    return 0;
}

static void op_att(Chan *c, uint32_t id, Vault *v, const char *op, const Vj *j, const VMsg *m) {
    char eid[40] = "", aid[40] = "";
    vj_getsbuf(j, 0, "id", eid, sizeof eid);
    vj_getsbuf(j, 0, "att", aid, sizeof aid);
    VEntry *e = vm_find(&v->m, eid);
    if (!e) { reply_err(c, id, "not-found"); return; }
    if (!strcmp(op, "att-add")) {
        if (m->nfds < 1) { reply_err(c, id, "no-file"); return; }
        size_t len;
        uint8_t *d = read_stream(m->fds[0], VF_MAX_ATT, &len);
        if (!d) { reply_err(c, id, "too-big"); return; }
        char *name = vj_gets(j, 0, "name"), *mime = vj_gets(j, 0, "mime");
        int r = att_add_bytes(v, e, name, mime, d, len);
        sm_free(d); sm_free(name); sm_free(mime);
        if (r) { reply_err(c, id, "memory"); return; }
        after_change(v, c, id, e);
        return;
    }
    int ai = att_find(e, aid);
    if (ai < 0) { reply_err(c, id, "no-attachment"); return; }
    if (!strcmp(op, "att-del")) {
        sm_free(e->att[ai].name); sm_free(e->att[ai].mime);
        memmove(e->att + ai, e->att + ai + 1, sizeof(VAtt) * (size_t)(e->natt - ai - 1));
        e->natt--;
        e->modified = now_s();
        after_change(v, c, id, e);
        return;
    }
    size_t len;
    uint8_t *pt = att_plain(v, &e->att[ai], &len);
    if (!pt) { reply_err(c, id, "corrupt"); return; }
    if (!strcmp(op, "att-get")) {
        int ok = m->nfds >= 1 && write_all_fd(m->fds[0], pt, len) == 0;
        sm_free(pt);
        if (!ok) { reply_err(c, id, "write"); return; }
        VjB b; vb_init(&b); vb_open(&b, '{'); vb_kb(&b, "ok", 1); vb_kn(&b, "size", (double)len); vb_close(&b, '}');
        reply(c, id, &b);
        return;
    }
    if (!strcmp(op, "att-text")) {           /* text (licenses, keys): up to 256 KB */
        if (len > 262144) { sm_free(pt); reply_err(c, id, "too-big"); return; }
        VjB b; vb_init(&b); vb_open(&b, '{'); vb_kb(&b, "ok", 1); vb_key(&b, "text"); vb_strn(&b, (const char *)pt, len); vb_close(&b, '}');
        sm_free(pt);
        reply(c, id, &b);
        return;
    }
    /* att-preview: image shrunk to max px, RGBA in base64 */
    int w, h;
    uint32_t *px = vstb_decode_argb(pt, len, &w, &h);
    sm_free(pt);
    if (!px) { reply_err(c, id, "not-an-image"); return; }
    int max = (int)vj_getn(j, 0, "max", 320);
    if (max < 16) max = 16;
    if (max > 1024) max = 1024;
    double sc = w > h ? (double)max / w : (double)max / h;
    if (sc > 1) sc = 1;
    int ow = (int)(w * sc + 0.5), oh = (int)(h * sc + 0.5);
    if (ow < 1) ow = 1;
    if (oh < 1) oh = 1;
    uint8_t *rgba = sm_alloc((size_t)ow * oh * 4);
    for (int y = 0; rgba && y < oh; y++)
        for (int x = 0; x < ow; x++) {
            /* average of the source box */
            int x0 = (int)(x / sc), x1 = (int)((x + 1) / sc), y0 = (int)(y / sc), y1 = (int)((y + 1) / sc);
            if (x1 <= x0) x1 = x0 + 1;
            if (y1 <= y0) y1 = y0 + 1;
            if (x1 > w) x1 = w;
            if (y1 > h) y1 = h;
            uint32_t r = 0, g = 0, bl = 0, a = 0, n = 0;
            for (int yy = y0; yy < y1; yy += (y1 - y0 > 8 ? (y1 - y0) / 8 : 1))
                for (int xx = x0; xx < x1; xx += (x1 - x0 > 8 ? (x1 - x0) / 8 : 1)) {
                    uint32_t p = px[(size_t)yy * w + xx];
                    a += p >> 24; r += p >> 16 & 255; g += p >> 8 & 255; bl += p & 255; n++;
                }
            uint8_t *o = rgba + ((size_t)y * ow + x) * 4;
            o[0] = (uint8_t)(r / n); o[1] = (uint8_t)(g / n); o[2] = (uint8_t)(bl / n); o[3] = (uint8_t)(a / n);
        }
    sm_free(px);
    if (!rgba) { reply_err(c, id, "memory"); return; }
    char *b64 = b64_encode(rgba, (size_t)ow * oh * 4);
    sm_free(rgba);
    VjB b; vb_init(&b); vb_open(&b, '{'); vb_kb(&b, "ok", 1); vb_ki(&b, "w", ow); vb_ki(&b, "h", oh); vb_ks(&b, "rgba", b64); vb_close(&b, '}');
    sm_free(b64);
    reply(c, id, &b);
}

/* ---- import/export ---- */

static const char *kind_cat(const ImpEntry *ie) {
    const char *k = ie->kind ? ie->kind : "";
    if (!strcmp(k, "card")) return "cards";
    if (!strcmp(k, "identity")) return "identity";
    if (!strcmp(k, "note")) return "notes";
    if (!strcmp(k, "wifi")) return "wifi";
    if (!strcmp(k, "ssh")) return "ssh";
    if (ie->url || ie->username || ie->otp) return "internet";
    return "general";
}

/* adds to cats (JSON) a user category for each imported folder; returns its id */
typedef struct { char path[256]; char id[40]; } GroupMap;

static const char *group_cat(Vault *v, GroupMap **gm, int *ngm, const char *path, int *added) {
    if (!path || !path[0]) return NULL;
    for (int i = 0; i < *ngm; i++) if (!strcmp((*gm)[i].path, path)) return (*gm)[i].id;
    /* the parent first */
    char parent[256];
    snprintf(parent, sizeof parent, "%s", path);
    char *slash = strrchr(parent, '/');
    const char *pid = NULL, *name = path;
    if (slash) { *slash = 0; pid = group_cat(v, gm, ngm, parent, added); name = strrchr(path, '/') + 1; }
    GroupMap *ng = realloc(*gm, sizeof(GroupMap) * (size_t)(*ngm + 1));
    if (!ng) return NULL;
    *gm = ng;
    GroupMap *g = &ng[(*ngm)++];
    snprintf(g->path, sizeof g->path, "%s", path);
    uint8_t h[8];
    crypto_blake2b(h, 8, (const uint8_t *)path, strlen(path));
    snprintf(g->id, sizeof g->id, "g%02x%02x%02x%02x%02x%02x", h[0], h[1], h[2], h[3], h[4], h[5]);
    /* already in cats? */
    if (v->m.cats && strstr(v->m.cats, g->id)) return g->id;
    VjB b;
    vb_init(&b);
    size_t cl = v->m.cats ? strlen(v->m.cats) : 0;
    if (cl >= 2 && v->m.cats[cl - 1] == ']') {
        vb_rawn(&b, v->m.cats, cl - 1);
        if (cl > 2) vb_raw(&b, ",");
    } else vb_raw(&b, "[");
    b.first[0] = 1;
    vb_open(&b, '{');
    vb_ks(&b, "id", g->id);
    vb_ks(&b, "name", name);
    vb_ks(&b, "icon", "\xf0\x9f\x93\x81");
    vb_ks(&b, "color", "#8e8e93");
    vb_ks(&b, "template", "general");
    if (pid) vb_ks(&b, "parent", pid);
    vb_close(&b, '}');
    vb_raw(&b, "]");
    if (!b.oom) { sm_free(v->m.cats); v->m.cats = b.p; (*added)++; } else vb_free(&b);
    return g->id;
}

static void add_field(VjB *b, const char *k, const char *t, const char *l, const char *val) {
    if (!val || !val[0]) return;
    vb_sep(b);
    vb_open(b, '{');
    vb_ks(b, "k", k);
    vb_ks(b, "t", t);
    if (l) vb_ks(b, "l", l);
    vb_ks(b, "v", val);
    vb_close(b, '}');
}

static int import_list(Vault *v, ImpList *L, int *cats_added) {
    GroupMap *gm = NULL;
    int ngm = 0, count = 0;
    for (int i = 0; i < L->ngroups; i++) group_cat(v, &gm, &ngm, L->groups[i], cats_added);
    for (int i = 0; i < L->n; i++) {
        ImpEntry *ie = &L->v[i];
        const char *cat = group_cat(v, &gm, &ngm, ie->group, cats_added);
        if (!cat) cat = kind_cat(ie);
        VjB b;
        vb_init(&b);
        vb_open(&b, '{');
        vb_ks(&b, "cat", cat);
        vb_ks(&b, "title", ie->title && ie->title[0] ? ie->title : (ie->url ? ie->url : ""));
        if (ie->notes) vb_ks(&b, "notes", ie->notes);
        vb_kb(&b, "fav", ie->favorite);
        if (ie->created) vb_kn(&b, "created", (double)ie->created);
        if (ie->modified) vb_kn(&b, "modified", (double)ie->modified);
        vb_key(&b, "tags");
        vb_open(&b, '[');
        for (int t = 0; t < ie->ntags; t++) { vb_sep(&b); vb_str(&b, ie->tags[t]); }
        vb_close(&b, ']');
        vb_key(&b, "f");
        vb_open(&b, '[');
        add_field(&b, "username", "text", NULL, ie->username);
        add_field(&b, "password", "hidden", NULL, ie->password);
        add_field(&b, "url", "url", NULL, ie->url);
        for (int u = 0; u < ie->nurls; u++) add_field(&b, "url", "url", NULL, ie->urls[u]);
        add_field(&b, "totp", "totp", NULL, ie->otp);
        for (int f = 0; f < ie->nfields; f++) {
            static const char *tn[] = { "text", "hidden", "url", "date", "number", "totp" };
            const ImpField *fi = &ie->fields[f];
            int ty = fi->type >= 0 && fi->type <= 5 ? fi->type : 0;
            char key[24];
            snprintf(key, sizeof key, "c%d", f + 1);
            /* known template fields (cards, identity) come in with their key */
            const char *known = NULL;
            static const char *keys[] = { "holder", "number", "expiry", "cvv", "pin", "brand", "email", "phone", "address", "ssid", "security",
                                          "host", "port", "server", "account", "service", "product", "license", "document", NULL };
            for (int q = 0; keys[q]; q++) if (fi->key && !strcmp(fi->key, keys[q])) known = keys[q];
            add_field(&b, known ? known : key, tn[ty], known ? NULL : fi->key, fi->value);
        }
        vb_close(&b, ']');
        vb_close(&b, '}');
        Vj j;
        VEntry *e = NULL;
        char err[64];
        if (!b.oom && !vj_parse(&j, b.p, b.n)) {
            if (!vm_upsert(&v->m, &j, 0, &e, err, sizeof err)) count++;
            vj_free(&j);
        }
        vb_free(&b);
        for (int a = 0; e && a < ie->natts; a++) att_add_bytes(v, e, ie->atts[a].name, NULL, ie->atts[a].data, ie->atts[a].len);
    }
    free(gm);
    return count;
}

static void op_import(Chan *c, uint32_t id, Vault *v, const Vj *j, const VMsg *m) {
    if (m->nfds < 1) { reply_err(c, id, "no-file"); return; }
    size_t len;
    uint8_t *d = read_stream(m->fds[0], 128u << 20, &len);
    if (!d) { reply_err(c, id, "too-big"); return; }
    char name[256] = "";
    vj_getsbuf(j, 0, "name", name, sizeof name);
    int kind = imp_detect(d, len, name);
    char fmt[32] = "";
    vj_getsbuf(j, 0, "format", fmt, sizeof fmt);
    if (!strcmp(fmt, "kdbx")) kind = IMP_KDBX;
    if (kind == IMP_UNKNOWN) { sm_free(d); reply_err(c, id, "unknown-format"); return; }
    char *pw = vj_gets(j, 0, "password");
    uint8_t *kf = NULL;
    size_t kflen = 0;
    if (m->nfds > 1) kf = read_stream(m->fds[1], 16u << 20, &kflen);
    ImpList L;
    memset(&L, 0, sizeof L);
    double t0 = now_ms();
    int r = imp_parse(kind, d, len, pw, kf, kflen, &L);
    double ms = now_ms() - t0;
    sm_free(d); sm_free(pw); sm_free(kf);
    if (r) { char e[200]; snprintf(e, sizeof e, "%s", L.err[0] ? L.err : "corrupt"); imp_free(&L); reply_err(c, id, e); return; }
    int added = 0;
    int count = import_list(v, &L, &added);
    imp_free(&L);
    int sr = vault_save(v);
    if (sr != VF_OK) { reply_err(c, id, vf_strerror(sr)); return; }
    static const char *kn[] = { "unknown", "kdbx", "keepassxc-csv", "bitwarden-json", "bitwarden-csv", "chrome-csv", "firefox-csv", "apple-csv", "lordos-csv" };
    VjB b;
    vb_init(&b);
    vb_open(&b, '{');
    vb_kb(&b, "ok", 1);
    vb_ki(&b, "count", count);
    vb_ki(&b, "cats", added);
    vb_ks(&b, "format", kind >= 0 && kind <= IMP_LORD_CSV ? kn[kind] : "?");
    vb_kn(&b, "ms", round(ms));
    vb_close(&b, '}');
    reply(c, id, &b);
    event_v("changed", (int)(v - vaults));
}

static void op_export_csv(Chan *c, uint32_t id, Vault *v, const VMsg *m) {
    if (m->nfds < 1) { reply_err(c, id, "no-file"); return; }
    ImpList L;
    memset(&L, 0, sizeof L);
    for (int i = 0; i < v->m.n; i++) {
        VEntry *e = v->m.e[i];
        if (e->trash) continue;
        ImpEntry *ie = imp_add(&L);
        if (!ie) break;
        ie->title = imp_strdup(e->title);
        ie->username = imp_strdup(vm_field(e, "username") ? vm_field(e, "username") : vm_field(e, "email"));
        ie->password = imp_strdup(vm_field(e, "password"));
        ie->url = imp_strdup(vm_field(e, "url"));
        ie->notes = imp_strdup(e->notes);
        ie->otp = imp_strdup(vm_field_type(e, "totp"));
        ie->group = imp_strdup(e->cat);
        ie->favorite = e->fav;
        ie->created = e->created;
        ie->modified = e->modified;
        if (e->ntags) {
            ie->tags = sm_alloc(sizeof(char *) * (size_t)e->ntags);
            for (int t = 0; ie->tags && t < e->ntags; t++) ie->tags[ie->ntags++] = imp_strdup(e->tags[t]);
        }
        int nf = 0;
        for (int f = 0; f < e->nf; f++) {
            const char *k = e->f[f].k;
            if (!strcmp(k, "username") || !strcmp(k, "password") || !strcmp(k, "url") || !strcmp(e->f[f].t, "totp")) continue;
            nf++;
        }
        if (nf) {
            ie->fields = sm_alloc(sizeof(ImpField) * (size_t)nf);
            for (int f = 0; ie->fields && f < e->nf; f++) {
                const char *k = e->f[f].k;
                if (!strcmp(k, "username") || !strcmp(k, "password") || !strcmp(k, "url") || !strcmp(e->f[f].t, "totp")) continue;
                ImpField *fi = &ie->fields[ie->nfields++];
                fi->key = imp_strdup(e->f[f].l && e->f[f].l[0] ? e->f[f].l : k);
                fi->value = imp_strdup(e->f[f].v);
                fi->type = !strcmp(e->f[f].t, "hidden") || !strcmp(e->f[f].t, "pin") ? IMPF_HIDDEN : !strcmp(e->f[f].t, "url") ? IMPF_URL : IMPF_TEXT;
            }
        }
    }
    char *out = NULL;
    size_t olen = 0;
    int r = exp_csv(&L, &out, &olen);
    int n = L.n;
    imp_free(&L);
    if (r) { reply_err(c, id, "memory"); return; }
    int ok = write_all_fd(m->fds[0], out, olen) == 0;
    sm_free(out);
    if (!ok) { reply_err(c, id, "write"); return; }
    VjB b; vb_init(&b); vb_open(&b, '{'); vb_kb(&b, "ok", 1); vb_ki(&b, "count", n); vb_close(&b, '}');
    reply(c, id, &b);
}

static void op_export_vault(Chan *c, uint32_t id, Vault *v, const VMsg *m) {
    if (m->nfds < 1) { reply_err(c, id, "no-file"); return; }
    size_t plen;
    char *plain = vm_dump(&v->m, &plen);
    if (!plain) { reply_err(c, id, "memory"); return; }
    int total = 0;
    for (int i = 0; i < v->m.n; i++) total += v->m.e[i]->natt;
    VAttOut *outs = calloc((size_t)(total ? total : 1), sizeof *outs);
    int no = 0;
    for (int i = 0; outs && i < v->m.n; i++)
        for (int a = 0; a < v->m.e[i]->natt; a++) {
            VAtt *at = &v->m.e[i]->att[a];
            VAttOut *o = &outs[no];
            memcpy(o->id, at->id, 16); memcpy(o->nonce, at->nonce, 24); memcpy(o->mac, at->mac, 16);
            int found = 0;
            for (int p = 0; p < v->npend && !found; p++) if (!memcmp(v->pend[p].id, at->id, 16)) { o->cipher = v->pend[p].cipher; o->len = v->pend[p].len; found = 1; }
            for (int p = 0; p < v->h.natts && !found; p++) if (!memcmp(v->h.atts[p].id, at->id, 16)) { o->srcfd = v->fd; o->off = v->h.atts[p].off; o->len = v->h.atts[p].len; found = 1; }
            if (found) no++;
        }
    VHeader h = v->h;                         /* same header and slots: it unlocks like the original */
    h.atts = NULL; h.natts = 0;
    int r = outs ? vf_write_fd(m->fds[0], &h, v->vk, (const uint8_t *)plain, plen, outs, no) : VF_ENOMEM;
    free(outs);
    sm_free(plain);
    if (r != VF_OK) reply_err(c, id, vf_strerror(r)); else reply_ok(c, id);
}

/* open another .lvault vault (a copy, another machine) and merge its entries */
static void op_import_vault(Chan *c, uint32_t id, Vault *v, const Vj *j, const VMsg *m) {
    if (m->nfds < 1) { reply_err(c, id, "no-file"); return; }
    size_t len;
    uint8_t *d = read_stream(m->fds[0], VF_MAX_BODY + (512u << 20), &len);
    if (!d) { reply_err(c, id, "too-big"); return; }
    VHeader h;
    int r = vf_parse(d, len, &h);
    if (r != VF_OK) { sm_free(d); reply_err(c, id, vf_strerror(r)); return; }
    Factors F;
    if (get_factors(j, m, m->nfds > 1 ? 1 : -1, &F)) { sm_free(d); vf_header_free(&h); reply_err(c, id, "keyfile-read"); return; }
    VFactors f = vfactors(&F);
    uint8_t vk[32];
    r = vf_open_slot(&h, 0, &f, vk, NULL);
    factors_free(&F);
    uint8_t *plain = NULL;
    size_t plen = 0;
    if (r == VF_OK) r = vf_read_body(d, len, &h, vk, &plain, &plen);
    VModel om;
    if (r == VF_OK && vm_load(&om, (const char *)plain, plen)) r = VF_ECORRUPT;
    sm_free(plain);
    if (r != VF_OK) { sm_wipe(vk, 32); sm_free(d); vf_header_free(&h); reply_err(c, id, r == VF_EBADKEY || r == VF_ENOSLOT ? "bad-key" : vf_strerror(r)); return; }
    int count = 0;
    for (int i = 0; i < om.n; i++) {
        VEntry *e = om.e[i];
        if (vm_find(&v->m, e->id)) continue;                   /* already there (same entry) */
        VjB b; vb_init(&b);
        vm_entry_json(&b, e, 1, 0);
        Vj ej; VEntry *ne = NULL; char err[64];
        if (!b.oom && !vj_parse(&ej, b.p, b.n)) { if (!vm_upsert(&v->m, &ej, 0, &ne, err, sizeof err)) count++; vj_free(&ej); }
        vb_free(&b);
        for (int a = 0; ne && a < e->natt; a++)
            for (int p = 0; p < h.natts; p++) {
                if (memcmp(h.atts[p].id, e->att[a].id, 16) || h.atts[p].off + h.atts[p].len > len) continue;
                uint8_t *pt = sm_alloc(h.atts[p].len + 1);
                if (pt && vf_att_open(vk, h.vault_id, e->att[a].id, d + h.atts[p].off, h.atts[p].len, e->att[a].nonce, e->att[a].mac, pt) == VF_OK)
                    att_add_bytes(v, ne, e->att[a].name, e->att[a].mime, pt, h.atts[p].len);
                sm_free(pt);
            }
    }
    vm_free(&om);
    sm_wipe(vk, 32);
    sm_free(d);
    vf_header_free(&h);
    r = vault_save(v);
    if (r != VF_OK) { reply_err(c, id, vf_strerror(r)); return; }
    VjB b; vb_init(&b); vb_open(&b, '{'); vb_kb(&b, "ok", 1); vb_ki(&b, "count", count); vb_close(&b, '}');
    reply(c, id, &b);
    event_v("changed", (int)(v - vaults));
}

/* ---- slots ---- */

static int slot_index(Vault *v, const char *hid) {
    uint8_t sid[8];
    if (hexbytes(hid, sid, 8)) return -1;
    for (int i = 0; i < v->h.nslots; i++) if (!memcmp(v->h.slots[i].id, sid, 8)) return i;
    return -1;
}

static void op_slot(Chan *c, uint32_t id, Vault *v, const char *op, const Vj *j, const VMsg *m) {
    if (!strcmp(op, "slot-del")) {
        char sid[40] = "";
        vj_getsbuf(j, 0, "slot", sid, sizeof sid);
        int si = slot_index(v, sid);
        if (si < 0) { reply_err(c, id, "no-slot"); return; }
        int others = 0;
        for (int i = 0; i < v->h.nslots; i++) if (i != si && v->h.slots[i].type != VS_RECOVERY) others++;
        if (!others) { reply_err(c, id, "last-slot"); return; }
        memmove(v->h.slots + si, v->h.slots + si + 1, sizeof(VSlot) * (size_t)(v->h.nslots - si - 1));
        v->h.nslots--;
        int r = vault_save(v);
        if (r != VF_OK) reply_err(c, id, vf_strerror(r)); else reply_ok(c, id);
        return;
    }
    if (!strcmp(op, "recovery-new")) {
        for (int i = v->h.nslots - 1; i >= 0; i--) if (v->h.slots[i].type == VS_RECOVERY) {
            memmove(v->h.slots + i, v->h.slots + i + 1, sizeof(VSlot) * (size_t)(v->h.nslots - i - 1));
            v->h.nslots--;
        }
        char rec[48];
        vf_recovery_new(rec);
        VFactors rf = { .recovery = rec };
        VSlot s;
        memset(&s, 0, sizeof s);
        VKdf none = { 0, 0, 0 };
        int r = vf_make_slot(&v->h, &s, VS_RECOVERY, &rf, v->vk, &none, "");
        if (r == VF_OK) { v->h.slots[v->h.nslots++] = s; r = vault_save(v); }
        if (r != VF_OK) { sm_wipe(rec, sizeof rec); reply_err(c, id, vf_strerror(r)); return; }
        VjB b; vb_init(&b); vb_open(&b, '{'); vb_kb(&b, "ok", 1); vb_ks(&b, "recovery", rec); vb_close(&b, '}');
        sm_wipe(rec, sizeof rec);
        reply(c, id, &b);
        return;
    }
    /* slot-add / password-change: method + factors; password-change replaces the password slot(s) */
    char method[16] = "";
    vj_getsbuf(j, 0, "method", method, sizeof method);
    int type = slot_type_of(method);
    if (!strcmp(op, "password-change")) type = VS_PASSWORD;
    if (!type || type == VS_RECOVERY) { reply_err(c, id, "bad-method"); return; }
    if (v->h.nslots >= VF_MAX_SLOTS) { reply_err(c, id, "too-many"); return; }
    if (type == VS_FIDO2) { fido_start(c, id, FJ_ADD, (int)(v - vaults), j, 0); return; }
    if (!strcmp(op, "password-change")) {
        /* ask for the current password (even if it is unlocked) */
        char *old = vj_gets(j, 0, "old");
        VFactors of = { .password = old };
        uint8_t tmp[32];
        int ok = 0;
        for (int i = 0; old && i < v->h.nslots; i++) if (v->h.slots[i].type == VS_PASSWORD || v->h.slots[i].type == VS_SESSION) ok = 1;
        int r = ok ? vf_open_slot(&v->h, 0, &of, tmp, NULL) : VF_ENOSLOT;
        sm_wipe(tmp, sizeof tmp);
        sm_free(old);
        if (r == VF_EBADKEY) { reply_err(c, id, "bad-key"); return; }
    }
    Factors F;
    int newkey = vj_getb(j, 0, "newkey", 0);
    if ((type == VS_KEYFILE || type == VS_PASSKEY) && newkey) {
        uint8_t key[64];
        if (m->nfds < 1) { reply_err(c, id, "no-keyfile"); return; }
        vc_random(key, sizeof key);
        if (write_all_fd(m->fds[0], key, sizeof key)) { sm_wipe(key, sizeof key); reply_err(c, id, "keyfile-write"); return; }
        get_factors(j, m, -1, &F);
        vf_keyfile_hash(key, sizeof key, F.kf);
        F.has_kf = 1;
        sm_wipe(key, sizeof key);
    } else if (get_factors(j, m, (type == VS_KEYFILE || type == VS_PASSKEY) ? 0 : -1, &F)) { reply_err(c, id, "keyfile-read"); return; }
    VFactors f = vfactors(&F);
    VKdf kdf;
    vf_calibrate((int)vj_getn(j, 0, "targetMs", 700), &kdf, NULL);
    char label[64] = "";
    vj_getsbuf(j, 0, "label", label, sizeof label);
    VSlot s;
    memset(&s, 0, sizeof s);
    int r = vf_make_slot(&v->h, &s, type, &f, v->vk, &kdf, label);
    factors_free(&F);
    if (r != VF_OK) { reply_err(c, id, r == VF_ENOSLOT ? "missing-factor" : vf_strerror(r)); return; }
    if (!strcmp(op, "password-change") || vj_getb(j, 0, "replaceSame", 0)) {
        for (int i = v->h.nslots - 1; i >= 0; i--) if (v->h.slots[i].type == type) {
            memmove(v->h.slots + i, v->h.slots + i + 1, sizeof(VSlot) * (size_t)(v->h.nslots - i - 1));
            v->h.nslots--;
        }
    }
    /* "password + key file" replaces the password alone (otherwise the second factor is useless) */
    if (type == VS_PASSKEY && vj_getb(j, 0, "dropPassword", 1))
        for (int i = v->h.nslots - 1; i >= 0; i--) if (v->h.slots[i].type == VS_PASSWORD) {
            memmove(v->h.slots + i, v->h.slots + i + 1, sizeof(VSlot) * (size_t)(v->h.nslots - i - 1));
            v->h.nslots--;
        }
    v->h.slots[v->h.nslots++] = s;
    r = vault_save(v);
    if (r != VF_OK) reply_err(c, id, vf_strerror(r)); else reply_ok(c, id);
}

/* ---- dispatch ---- */

static void handle_request(Chan *c, VMsg *m) {
    Vj j;
    if (vj_parse(&j, m->json, m->len) || vj_type(&j, 0) != VJ_OBJ) { vj_free(&j); reply_err(c, m->id, "bad-json"); return; }
    char op[32] = "";
    vj_getsbuf(&j, 0, "op", op, sizeof op);
    uint32_t id = m->id;
    int vi = vault_of(&j);
    Vault *v = &vaults[vi];
    if (debug > 1) fprintf(stderr, "axia-vault: op %s (%s)\n", op, vi == V_MAIN ? "main" : "login");
    if (!strcmp(op, "status")) { vault_probe(&vaults[V_MAIN]); if (!vaults[V_LOGIN].unlocked) vault_probe(&vaults[V_LOGIN]); VjB b; vb_init(&b); status_json(&b); reply(c, id, &b); }
    else if (!strcmp(op, "create")) op_create(c, id, &j, m);
    else if (!strcmp(op, "unlock")) op_unlock(c, id, &j, m);
    else if (!strcmp(op, "lock")) { vault_lock(v); reply_ok(c, id); }
    else if (!strcmp(op, "generate")) op_generate(c, id, &j);
    else if (!strcmp(op, "strength")) op_strength(c, id, &j);
    else if (!strcmp(op, "qr-scan")) op_qr_scan(c, id, m);
    else if (!strcmp(op, "calibrate")) {
        VKdf k; int ms;
        vf_calibrate((int)vj_getn(&j, 0, "targetMs", 700), &k, &ms);
        VjB b; vb_init(&b); vb_open(&b, '{'); vb_kb(&b, "ok", 1); vb_ki(&b, "m", k.m_kib); vb_ki(&b, "t", k.t_cost); vb_ki(&b, "ms", ms); vb_close(&b, '}');
        reply(c, id, &b);
    }
    else if (!strcmp(op, "totp-parse")) {
        char *t = vj_gets(&j, 0, "text");
        Totp tp;
        int ok = t && !totp_parse(t, &tp);
        sm_free(t);
        VjB b; vb_init(&b); vb_open(&b, '{'); vb_kb(&b, "ok", ok);
        if (ok) { vb_ks(&b, "issuer", tp.issuer); vb_ks(&b, "account", tp.account); vb_ki(&b, "digits", tp.digits); vb_ki(&b, "period", tp.period);
                  vb_ks(&b, "algo", tp.algo == TOTP_SHA256 ? "SHA256" : tp.algo == TOTP_SHA512 ? "SHA512" : "SHA1"); vb_kb(&b, "hotp", tp.hotp); totp_wipe(&tp); }
        vb_close(&b, '}');
        reply(c, id, &b);
    }
    else if (keyring_handle(c, id, op, &j)) { /* kr-* */ }
    else if (!strcmp(op, "qr")) op_qr(c, id, v, &j);
    else if (!need_unlocked(c, id, v)) { /* replied "locked" */ }
    else if (!strcmp(op, "list")) op_list(c, id, v);
    else if (!strcmp(op, "get")) {
        char eid[40] = "";
        vj_getsbuf(&j, 0, "id", eid, sizeof eid);
        VEntry *e = vm_find(&v->m, eid);
        if (!e) reply_err(c, id, "not-found");
        else { VjB b; vb_init(&b); vb_open(&b, '{'); vb_kb(&b, "ok", 1); vb_key(&b, "entry"); vm_entry_json(&b, e, vj_getb(&j, 0, "reveal", 0), vj_getb(&j, 0, "hist", 0)); vb_close(&b, '}'); reply(c, id, &b); }
    }
    else if (!strcmp(op, "reveal")) {
        char eid[40] = "", k[72] = "";
        vj_getsbuf(&j, 0, "id", eid, sizeof eid);
        vj_getsbuf(&j, 0, "k", k, sizeof k);
        int idx = (int)vj_getn(&j, 0, "i", 0);
        VEntry *e = vm_find(&v->m, eid);
        const char *val = NULL;
        for (int f = 0, seen = 0; e && f < e->nf; f++) if (!strcmp(e->f[f].k, k) && seen++ == idx) { val = e->f[f].v; break; }
        if (!val) reply_err(c, id, "not-found");
        else { VjB b; vb_init(&b); vb_open(&b, '{'); vb_kb(&b, "ok", 1); vb_ks(&b, "value", val); vb_close(&b, '}'); reply(c, id, &b); }
    }
    else if (!strcmp(op, "save")) {
        int et = vj_get(&j, 0, "entry");
        VEntry *e = NULL;
        char err[64];
        if (vm_upsert(&v->m, &j, et, &e, err, sizeof err)) reply_err(c, id, err);
        else after_change(v, c, id, e);
    }
    else if (!strcmp(op, "trash") || !strcmp(op, "restore") || !strcmp(op, "purge") || !strcmp(op, "fav") || !strcmp(op, "archive")) {
        char eid[40] = "";
        vj_getsbuf(&j, 0, "id", eid, sizeof eid);
        VEntry *e = vm_find(&v->m, eid);
        if (!e) { reply_err(c, id, "not-found"); vj_free(&j); return; }
        if (!strcmp(op, "purge")) { vm_remove(&v->m, eid); e = NULL; }
        else if (!strcmp(op, "trash")) e->trash = now_s();
        else if (!strcmp(op, "restore")) e->trash = 0;
        else if (!strcmp(op, "fav")) e->fav = vj_getb(&j, 0, "on", !e->fav);
        else e->arch = vj_getb(&j, 0, "on", !e->arch);
        if (e && strcmp(op, "fav")) e->modified = now_s();
        v->m.dirty = 1;
        after_change(v, c, id, e);
    }
    else if (!strcmp(op, "empty-trash")) {
        for (int i = v->m.n - 1; i >= 0; i--) if (v->m.e[i]->trash) vm_remove(&v->m, v->m.e[i]->id);
        after_change(v, c, id, NULL);
    }
    else if (!strcmp(op, "set-meta") || !strcmp(op, "set-cats") || !strcmp(op, "set-tags")) {
        const char *key = op[4] == 'm' ? "meta" : op[4] == 'c' ? "cats" : "tags";
        int k = vj_get(&j, 0, key);
        int want = op[4] == 'm' ? VJ_OBJ : VJ_ARR;
        if (vj_type(&j, k) != want) { reply_err(c, id, "bad-json"); vj_free(&j); return; }
        char *raw = sm_strndup(j.s + j.t[k].start, j.t[k].end - j.t[k].start);
        char **dst = op[4] == 'm' ? &v->m.meta : op[4] == 'c' ? &v->m.cats : &v->m.tags;
        sm_free(*dst);
        *dst = raw;
        if (op[4] == 'm' && v == &vaults[V_MAIN]) v->autolock_s = (int)vj_getn(&j, k, "autolock", 300);
        after_change(v, c, id, NULL);
    }
    else if (!strcmp(op, "rename-tag")) {
        char from[128] = "", to[128] = "";
        vj_getsbuf(&j, 0, "from", from, sizeof from);
        vj_getsbuf(&j, 0, "to", to, sizeof to);
        for (int i = 0; i < v->m.n; i++) {
            VEntry *e = v->m.e[i];
            for (int t = 0; t < e->ntags; t++) if (!strcmp(e->tags[t], from)) {
                if (to[0]) { sm_free(e->tags[t]); e->tags[t] = sm_strdup(to); }
                else { sm_free(e->tags[t]); memmove(e->tags + t, e->tags + t + 1, sizeof(char *) * (size_t)(e->ntags - t - 1)); e->ntags--; t--; }
            }
            vm_refresh(e);
        }
        after_change(v, c, id, NULL);
    }
    else if (!strcmp(op, "search")) {
        char *q = vj_gets(&j, 0, "q");
        double t0 = now_ms();
        VjB ids;
        vb_init(&ids);
        vm_search_json(&ids, &v->m, q, (int)vj_getn(&j, 0, "max", 100000));
        double ms = now_ms() - t0;
        sm_free(q);
        VjB b; vb_init(&b); vb_open(&b, '{'); vb_kb(&b, "ok", 1); vb_kn(&b, "ms", ms); vb_key(&b, "ids"); vb_raw(&b, ids.p ? ids.p : "[]"); vb_close(&b, '}');
        vb_free(&ids);
        reply(c, id, &b);
    }
    else if (!strcmp(op, "report")) op_report(c, id, v, m);
    else if (!strcmp(op, "totp")) op_totp(c, id, v, &j);
    else if (!strncmp(op, "att-", 4)) op_att(c, id, v, op, &j, m);
    else if (!strcmp(op, "import")) op_import(c, id, v, &j, m);
    else if (!strcmp(op, "import-vault")) op_import_vault(c, id, v, &j, m);
    else if (!strcmp(op, "export-csv")) op_export_csv(c, id, v, m);
    else if (!strcmp(op, "export-vault")) op_export_vault(c, id, v, m);
    else if (!strcmp(op, "slot-add") || !strcmp(op, "slot-del") || !strcmp(op, "password-change") || !strcmp(op, "recovery-new")) op_slot(c, id, v, op, &j, m);
    else reply_err(c, id, "unknown-op");
    vj_free(&j);
}

/* ================= control (axia-server only) ================= */

static void handle_ctl(Chan *c, VMsg *m) {
    Vj j;
    if (vj_parse(&j, m->json, m->len) || vj_type(&j, 0) != VJ_OBJ) { vj_free(&j); reply_err(c, m->id, "bad-json"); return; }
    char op[32] = "";
    vj_getsbuf(&j, 0, "op", op, sizeof op);
    if (!strcmp(op, "attach") && m->nfds >= 1 && nchans < MAX_CHANS) {
        Chan *n = &chans[nchans++];
        memset(n, 0, sizeof *n);
        n->fd = m->fds[0];
        m->fds[0] = -1;
        fcntl(n->fd, F_SETFL, O_NONBLOCK);
        n->role = ROLE_APP;
        reply_ok(c, m->id);
    } else if (!strcmp(op, "lock-all")) {
        char why[16] = "";
        vj_getsbuf(&j, 0, "why", why, sizeof why);
        vault_lock(&vaults[V_MAIN]);
        if (!strcmp(why, "logout")) vault_lock(&vaults[V_LOGIN]);
        reply_ok(c, m->id);
    } else if (!strcmp(op, "session-password")) {
        char *p = vj_gets(&j, 0, "password");
        if (p) keyring_session_password(p);
        sm_free(p);
        reply_ok(c, m->id);
    } else if (!strcmp(op, "fido-device")) {
        if (fido_fd >= 0 && !fj.busy) close(fido_fd);
        fido_fd = -1;
        if (m->nfds >= 1) { fido_fd = m->fds[0]; m->fds[0] = -1; }
        vj_getsbuf(&j, 0, "name", fido_name, sizeof fido_name);
        reply_ok(c, m->id);
    } else if (!strcmp(op, "bus") && m->nfds >= 1) {
        keyring_start_bus(m->fds[0]);
        m->fds[0] = -1;
        reply_ok(c, m->id);
    } else if (!strcmp(op, "prompt-done")) {
        keyring_prompt_result((unsigned)vj_getn(&j, 0, "prompt", 0), vj_getb(&j, 0, "ok", 0));
        reply_ok(c, m->id);
    } else reply_err(c, m->id, "unknown-ctl");
    vj_free(&j);
}

/* ================= startup ================= */

static void lockdown(void) {
    static const int sys[] = { SYS_mlock, SYS_munlock,
#ifdef SYS_mlock2
                               SYS_mlock2,
#endif
                               SYS_fsync, SYS_fdatasync, -1 };
    int rw[2] = { vault_dirfd, -1 };
    static const char *ro[] = { NULL };
    SandboxHelperCfg cfg = { .name = "lordos-vault", .ro_paths = ro, .extra_syscalls = sys, .mem_max_mb = 1024, .rw_dir_fds = rw, .ro_dir_fds = NULL };
    int mask = sandbox_apply_helper(&cfg);
    if (debug) fprintf(stderr, "axia-vault[%d]: caja=%#x\n", getpid(), (unsigned)mask);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    sm_harden_process();
    debug = getenv("LORD_VAULT_DEBUG") ? atoi(getenv("LORD_VAULT_DEBUG")) + 1 : 0;
    if (debug < 0) debug = 1;
    /* nothing inherited except 0–3 */
    for (int fd = 4; fd < 1024; fd++) close(fd);
    /* 1) init: the vaults folder (and maybe the bus) */
    VMsg m;
    if (vw_recv(CTL_FD, &m, &smalloc)) return 1;
    if (m.kind != VW_CTL || m.nfds < 1) { fprintf(stderr, "axia-vault: the folder is missing\n"); return 1; }
    vault_dirfd = m.fds[0];
    m.fds[0] = -1;
    int busfd = -1;
    if (m.nfds >= 2) { busfd = m.fds[1]; m.fds[1] = -1; }
    uint32_t init_id = m.id;
    vw_msg_free(&m, &smalloc);
    /* 2) confine ourselves before touching a vault */
    lockdown();
    if (pipe2(fpipe, O_CLOEXEC | O_NONBLOCK)) return 1;
    vg_set_lists(_binary_words_txt_start, (size_t)(_binary_words_txt_end - _binary_words_txt_start),
                 _binary_common_txt_start, (size_t)(_binary_common_txt_end - _binary_common_txt_start));
    chans[0].fd = CTL_FD;
    chans[0].role = ROLE_SYSTEM;
    nchans = 1;
    for (int i = 0; i < V_COUNT; i++) vault_probe(&vaults[i]);
    if (busfd >= 0) keyring_start_bus(busfd);
    reply_ok(&chans[0], init_id);
    /* 3) loop */
    for (;;) {
        struct pollfd pf[MAX_CHANS + 2];
        int n = 0;
        for (int i = 0; i < nchans; i++) { pf[n].fd = chans[i].fd; pf[n].events = POLLIN; n++; }
        pf[n].fd = fpipe[0]; pf[n].events = POLLIN; n++;
        int bfd = keyring_bus_fd();
        if (bfd >= 0) { pf[n].fd = bfd; pf[n].events = POLLIN | (keyring_bus_wants_out() ? POLLOUT : 0); n++; }
        int r = poll(pf, (nfds_t)n, 1000);
        if (r < 0 && errno != EINTR) break;
        /* auto-lock */
        Vault *mv = &vaults[V_MAIN];
        if (mv->unlocked && mv->autolock_s > 0 && !fj.busy && now_ms() - mv->last_use > mv->autolock_s * 1000.0) {
            if (debug) fprintf(stderr, "axia-vault: automatic lock\n");
            vault_lock(mv);
        }
        if (r <= 0) continue;
        int k = 0;
        for (int i = 0; i < nchans; i++, k++) {
            if (!(pf[k].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            Chan *c = &chans[i];
            for (;;) {
                VMsg msg;
                int fr = vw_feed(c->fd, &c->rx, &msg, &smalloc);
                if (fr == 0) break;
                if (fr < 0) {
                    if (c->role == ROLE_SYSTEM) { for (int v = 0; v < V_COUNT; v++) vault_lock(&vaults[v]); sm_wipe_all(); _exit(0); }
                    close(c->fd);
                    vw_rx_free(&c->rx, &smalloc);
                    /* app channel closed: remove it */
                    chans[i] = chans[--nchans];
                    i--;
                    break;
                }
                if (msg.kind == VW_CTL && c->role == ROLE_SYSTEM) handle_ctl(c, &msg);
                else if (msg.kind == VW_REQ) handle_request(c, &msg);
                vw_msg_free(&msg, &smalloc);
            }
        }
        if (pf[k].revents & POLLIN) {
            char buf[16];
            ssize_t got = read(fpipe[0], buf, sizeof buf);
            for (ssize_t i = 0; i < got; i++) {
                if (buf[i] == 't') broadcast_event("{\"ev\":\"touch\"}");
                else if (buf[i] == 'd') fido_done();
            }
        }
        k++;
        if (bfd >= 0 && k < n && (pf[k].revents & (POLLIN | POLLOUT | POLLHUP | POLLERR))) keyring_bus_dispatch();
    }
    return 0;
}
