/* apphost_bc.c — the app host (assets/app-host.bundle.js: React, the runtime and the SDK) compiled
 * to QuickJS bytecode once per session, so an isolated app does not parse 430 KB of JavaScript
 * every time it starts (~80 ms on a 1.8 GHz core; reading the bytecode takes ~3 ms).
 *
 * axia-server reads the bundle once through the verified-code check (codesig_load: the bytes that
 * were hashed against the signed manifest are the bytes returned) and compiles exactly those bytes,
 * both on a thread of its own, with its own QuickJS runtime, a few seconds after the session starts,
 * into a sealed memfd. app_spawn() hands the app process a read-only fd of it
 * (LORD_APPHOST_BC_FD). The app process loads and verifies the source itself as before and uses
 * the bytecode only if its header carries the size and SHA-256 of those same bytes; otherwise, or
 * with no fd, it parses the source. The bytecode comes from axia-server (the same binary and
 * QuickJS), never from the app or from a file a user can write. */
#include "apphost_bc.h"
#include "../pkg/crypto.h"
#include "../sec/codesig.h"
#include "quickjs.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define BC_MAGIC "LORDQBC1"
#define BC_MAX   (64u << 20)

typedef struct {
    char magic[8];
    uint64_t size;                   /* source bytes it was compiled from */
    uint8_t sha256[32];              /* and their SHA-256 */
    uint64_t len;                    /* bytecode bytes that follow */
} BcHeader;

static _Atomic int bc_fd = -1;
static char bc_path[1024];

static void header_of(const char *src, size_t len, BcHeader *h) {
    memset(h, 0, sizeof *h);
    memcpy(h->magic, BC_MAGIC, 8);
    h->size = (uint64_t)len;
    sha256(src, len, h->sha256);
}

static void *compile_thread(void *arg) {
    (void)arg;
    struct timespec pause = { 3, 0 };              /* after the desktop has drawn its first frames */
    nanosleep(&pause, NULL);
    /* read and hashed here, off the main thread (~15 ms cold at session start); the state and the
     * manifest were set up on the main thread before it started (apphost_bc_start) */
    char why[160];
    size_t got = 0;
    char *src = codesig_load(bc_path, BC_MAX, &got, why, sizeof why);
    if (!src) { fprintf(stderr, "axia-server: no app host bytecode: %s\n", why); return NULL; }
    JSRuntime *rt = JS_NewRuntime();
    JSContext *cx = rt ? JS_NewContext(rt) : NULL;
    uint8_t *bc = NULL;
    size_t bl = 0;
    if (cx) {
        JSValue fn = JS_Eval(cx, src, got, bc_path, JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
        if (!JS_IsException(fn)) bc = JS_WriteObject(cx, &bl, fn, JS_WRITE_OBJ_BYTECODE);
        else JS_FreeValue(cx, JS_GetException(cx));
        JS_FreeValue(cx, fn);
    }
    int mfd = -1;
    if (bc && bl && bl < BC_MAX) {
        BcHeader h;
        header_of(src, got, &h);
        h.len = bl;
        mfd = memfd_create("lordos-apphost-bc", MFD_CLOEXEC | MFD_ALLOW_SEALING);
        int ok = mfd >= 0 && write(mfd, &h, sizeof h) == (ssize_t)sizeof h;
        size_t off = 0;
        while (ok && off < bl) { ssize_t w = write(mfd, bc + off, bl - off); if (w <= 0) ok = 0; else off += (size_t)w; }
        ok = ok && !fcntl(mfd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL);
        if (!ok && mfd >= 0) { close(mfd); mfd = -1; }
    }
    free(src);
    if (bc) js_free(cx, bc);
    if (cx) JS_FreeContext(cx);
    if (rt) JS_FreeRuntime(rt);
    if (mfd >= 0) {
        atomic_store(&bc_fd, mfd);
        fprintf(stderr, "axia-server: app host compiled to bytecode (%zu KB) for isolated apps\n", bl / 1024);
    }
    return NULL;
}

void apphost_bc_start(const char *bundle_path) {
    static int started;
    if (started || !bundle_path || strlen(bundle_path) >= sizeof bc_path) return;
    started = 1;
    snprintf(bc_path, sizeof bc_path, "%s", bundle_path);
    /* the verified-code state is set up once, on this thread (main.c); without it, no bytecode:
     * the apps parse the source, as before */
    if (codesig_state() == CODE_UNCHECKED) { fprintf(stderr, "axia-server: no app host bytecode: code check not set up\n"); return; }
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &at, compile_thread, NULL)) {
        fprintf(stderr, "axia-server: no thread for the app host bytecode\n");
    }
    pthread_attr_destroy(&at);
}

int apphost_bc_dup_ro(void) {
    int fd = atomic_load(&bc_fd);
    if (fd < 0) return -1;
    char proc[64];
    snprintf(proc, sizeof proc, "/proc/self/fd/%d", fd);
    return open(proc, O_RDONLY | O_CLOEXEC);       /* read-only: the app can only map it PROT_READ */
}

int apphost_bc_eval(JSContext *ctx, const char *src, size_t len, int fd, int *ok) {
    *ok = 0;
    struct stat fst;
    if (fd < 0 || !src || fstat(fd, &fst) || fst.st_size < (off_t)sizeof(BcHeader) || fst.st_size > (off_t)BC_MAX) return -1;
    void *m = mmap(NULL, (size_t)fst.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (m == MAP_FAILED) return -1;
    BcHeader want, h;
    memcpy(&h, m, sizeof h);
    if (memcmp(h.magic, BC_MAGIC, 8) || h.size != (uint64_t)len || h.len != (uint64_t)fst.st_size - sizeof h) { munmap(m, (size_t)fst.st_size); return -1; }
    header_of(src, len, &want);                    /* compiled from exactly these bytes? */
    if (memcmp(want.sha256, h.sha256, sizeof h.sha256)) { munmap(m, (size_t)fst.st_size); return -1; }
    JSValue fn = JS_ReadObject(ctx, (const uint8_t *)m + sizeof h, (size_t)h.len, JS_READ_OBJ_BYTECODE);
    munmap(m, (size_t)fst.st_size);
    if (JS_IsException(fn)) { JS_FreeValue(ctx, JS_GetException(ctx)); return -1; }
    JSValue r = JS_EvalFunction(ctx, fn);       /* takes fn */
    *ok = !JS_IsException(r);
    if (!*ok) {                                  /* it ran: the caller reports the exception, not a fallback */
        JS_FreeValue(ctx, r);
        return 1;
    }
    JS_FreeValue(ctx, r);
    return 0;
}
