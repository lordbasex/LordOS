/* vmkeysrv.c — lord-server's side of an encrypted machine's key (vmkeysrv.h). */
#include "vmkeysrv.h"
#include "machine.h"
#include "../sec/json.h"
#include "../vault/vclient.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

const char *const VMK_NAMES[] = { "none", "pending", "ready", "locked", "missing", "error", NULL };

#define SLOTS 8
#define KR_WAIT_MS 10000
typedef struct { char id[37]; int state; char key[65]; time_t until; unsigned gen; } Slot;
static Slot *slots;                     /* one locked page, MADV_DONTDUMP */
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;

static int ready_page(void) {
    if (slots) return 0;
    void *pg = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (pg == MAP_FAILED) return -1;
    mlock(pg, 4096);
    madvise(pg, 4096, MADV_DONTDUMP);
    memset(pg, 0, 4096);
    slots = pg;
    return 0;
}
/* under mu: drop expired keys */
static void sweep(void) {
    time_t now = time(NULL);
    for (int i = 0; i < SLOTS; i++)
        if (slots[i].id[0] && slots[i].state != VMK_PENDING && slots[i].until && now > slots[i].until) explicit_bzero(&slots[i], sizeof slots[i]);
}
/* under mu: id's slot, made (or the oldest one reused) when make */
static Slot *slot_of(const char *id, int make) {
    sweep();
    for (int i = 0; i < SLOTS; i++) if (!strcmp(slots[i].id, id)) return &slots[i];
    if (!make) return NULL;
    Slot *best = NULL;
    for (int i = 0; i < SLOTS; i++) if (!slots[i].id[0]) { best = &slots[i]; break; }
    if (!best) for (int i = 0; i < SLOTS; i++) if (slots[i].state != VMK_PENDING && (!best || slots[i].until < best->until)) best = &slots[i];
    if (!best) return NULL;
    explicit_bzero(best, sizeof *best);
    snprintf(best->id, sizeof best->id, "%s", id);
    return best;
}

/* one keyring request (lord-vault, private channel) and its answer: 0 ok (secret into out when asked), 1 not found,
 * -2 locked, -1 anything else */
static int kr(const char *op, const char *id, const char *label, const char *secret, char out[65]) {
    char ql[2 * VM_NAME_MAX * 6 + 32] = "", j[1024];
    if (label) json_quote(ql, sizeof ql, label);
    int len = snprintf(j, sizeof j, "{\"op\":\"%s\",\"owner\":\"machines\",\"service\":\"machines:%s\",\"account\":\"\"%s%s%s%s%s}", op, id,
                       label ? ",\"label\":" : "", label ? ql : "", secret ? ",\"secret\":\"" : "", secret ? secret : "", secret ? "\"" : "");
    char *rep = NULL;
    size_t rl = 0;
    int r = len > 0 && (size_t)len < sizeof j ? vc_request_wait(j, (size_t)len, &rep, &rl, KR_WAIT_MS) : -1;
    explicit_bzero(j, sizeof j);
    if (r || !rep) return -1;
    JDoc d;
    r = -1;
    if (!json_parse(rep, rl, &d, NULL, 0)) {
        const char *e = json_str(d.root, "error");
        if (json_bool(d.root, "ok", 0)) {
            r = 0;
            if (out) {
                const char *s = json_str(json_get(d.root, "item"), "secret");
                if (s && vm_key_valid(s, strlen(s))) memcpy(out, s, 65); else r = -1;
            }
        } else if (e) r = !strcmp(e, "locked") || !strcmp(e, "no-keyring") ? -2 : !strcmp(e, "not-found") ? 1 : -1;
        json_free(&d);
    }
    explicit_bzero(rep, rl);
    free(rep);
    return r;
}

typedef struct { int op; char id[37], from[37], name[VM_NAME_MAX + 1], key[65]; unsigned gen; } Job;
enum { OP_GET, OP_NEW, OP_COPY, OP_SET, OP_DEL };

static void finish(const Job *jb, int state, const char *key) {
    pthread_mutex_lock(&mu);
    for (int i = 0; i < SLOTS; i++) {
        Slot *s = &slots[i];
        if (strcmp(s->id, jb->id) || s->gen != jb->gen) continue;
        s->state = state;
        if (key && state == VMK_READY) memcpy(s->key, key, 65); else explicit_bzero(s->key, sizeof s->key);
        s->until = time(NULL) + VMKEY_TTL_S;
    }
    pthread_mutex_unlock(&mu);
}
static int code_state(int r) { return r == 0 ? VMK_READY : r == 1 ? VMK_MISSING : r == -2 ? VMK_LOCKED : VMK_ERROR; }

static void *worker(void *arg) {
    Job *jb = arg;
    char k[65] = "";
    char label[VM_NAME_MAX + 32];
    snprintf(label, sizeof label, "Machines: %s", jb->name[0] ? jb->name : jb->id);
    int r;
    switch (jb->op) {
    case OP_GET: r = kr("kr-get", jb->id, NULL, NULL, k); finish(jb, code_state(r), k); break;
    case OP_NEW: {
        uint8_t raw[32];
        if (getrandom(raw, sizeof raw, 0) != (ssize_t)sizeof raw) { finish(jb, VMK_ERROR, NULL); break; }
        for (int i = 0; i < 32; i++) snprintf(k + 2 * i, 3, "%02x", raw[i]);
        explicit_bzero(raw, sizeof raw);
        /* never overwrite a key kept under this id: a new id has none */
        char old[65];
        r = kr("kr-get", jb->id, NULL, NULL, old);
        explicit_bzero(old, sizeof old);
        if (r == 0) { finish(jb, VMK_ERROR, NULL); break; }
        r = r == 1 ? kr("kr-set", jb->id, label, k, NULL) : r;
        finish(jb, code_state(r), k);
        break;
    }
    case OP_COPY:
        r = kr("kr-get", jb->from, NULL, NULL, k);
        if (!r) r = kr("kr-set", jb->id, label, k, NULL);
        finish(jb, code_state(r), k);
        break;
    case OP_SET: r = kr("kr-set", jb->id, label, jb->key, NULL); finish(jb, code_state(r), jb->key); break;
    case OP_DEL: kr("kr-del", jb->id, NULL, NULL, NULL); break;
    }
    explicit_bzero(k, sizeof k);
    explicit_bzero(jb, sizeof *jb);
    munlock(jb, sizeof *jb);
    free(jb);
    return NULL;
}

static int start(int op, const char *id, const char *from, const char *name, const char *key) {
    if (!vm_uuid_valid(id) || (from && !vm_uuid_valid(from))) return -1;
    pthread_mutex_lock(&mu);
    if (ready_page()) { pthread_mutex_unlock(&mu); return -1; }
    /* a deletion holds no slot (nothing waits for its answer); the others hold id's, or none is free: -1, try later */
    Slot *s = op == OP_DEL ? NULL : slot_of(id, 1);
    if (!s && op != OP_DEL) { pthread_mutex_unlock(&mu); return -1; }
    if (s && op == OP_GET && (s->state == VMK_READY || s->state == VMK_PENDING)) { pthread_mutex_unlock(&mu); return 0; }
    Job *jb = calloc(1, sizeof *jb);
    if (!jb) { pthread_mutex_unlock(&mu); return -1; }
    mlock(jb, sizeof *jb);
    jb->op = op;
    snprintf(jb->id, sizeof jb->id, "%s", id);
    if (from) snprintf(jb->from, sizeof jb->from, "%s", from);
    if (name) snprintf(jb->name, sizeof jb->name, "%s", name);
    if (key) memcpy(jb->key, key, 65);
    if (s) {
        s->state = VMK_PENDING;
        s->until = 0;
        jb->gen = ++s->gen;
    }
    pthread_mutex_unlock(&mu);
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    int r = pthread_create(&t, &at, worker, jb);
    pthread_attr_destroy(&at);
    if (r) { finish(jb, VMK_ERROR, NULL); explicit_bzero(jb, sizeof *jb); free(jb); return -1; }
    return 0;
}

int vmkey_fetch(const char *id) { return start(OP_GET, id, NULL, NULL, NULL); }
int vmkey_new(const char *id, const char *name) { return start(OP_NEW, id, NULL, name, NULL); }
int vmkey_copy(const char *from, const char *to, const char *name) { return start(OP_COPY, to, from, name, NULL); }
int vmkey_enter(const char *id, const char *typed, const char *name) {
    char k[65];
    size_t n = 0;
    for (const char *c = typed; *c && n <= 64; c++) {
        if (*c == ' ' || *c == '-' || *c == '\n' || *c == '\t') continue;
        char x = *c >= 'A' && *c <= 'F' ? (char)(*c - 'A' + 'a') : *c;
        if (n < 64) k[n] = x;
        n++;
    }
    if (n != 64 || !vm_key_valid(k, 64)) { explicit_bzero(k, sizeof k); return -1; }
    k[64] = 0;
    int r = start(OP_SET, id, NULL, name, k);
    explicit_bzero(k, sizeof k);
    return r;
}
void vmkey_forget(const char *id) {
    pthread_mutex_lock(&mu);
    if (slots) { Slot *s = slot_of(id, 0); if (s) explicit_bzero(s, sizeof *s); }
    pthread_mutex_unlock(&mu);
    start(OP_DEL, id, NULL, NULL, NULL);
}
int vmkey_state(const char *id) {
    pthread_mutex_lock(&mu);
    Slot *s = slots ? slot_of(id, 0) : NULL;
    int st = s ? s->state : VMK_NONE;
    pthread_mutex_unlock(&mu);
    return st;
}
int vmkey_pipe(const char *id) {
    int p[2] = { -1, -1 }, ok = 0;
    pthread_mutex_lock(&mu);
    Slot *s = slots ? slot_of(id, 0) : NULL;
    if (s && s->state == VMK_READY && !pipe2(p, O_CLOEXEC)) {
        ok = write(p[1], s->key, 64) == 64;
        close(p[1]);
    }
    pthread_mutex_unlock(&mu);
    if (!ok && p[0] >= 0) { close(p[0]); return -1; }
    return ok ? p[0] : -1;
}
void vmkey_wipe_all(void) {
    pthread_mutex_lock(&mu);
    if (slots) for (int i = 0; i < SLOTS; i++) if (slots[i].state != VMK_PENDING) explicit_bzero(&slots[i], sizeof slots[i]);
    pthread_mutex_unlock(&mu);
}
