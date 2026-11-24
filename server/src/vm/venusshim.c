/* venusshim.c — lordos-venus-shim.so, preloaded into a venus machine's QEMU by lord-vm (docs/VIRTUALIZATION.md 6.10,
 * "venus"). Never loaded into anything else.
 *
 * Why: with venus, QEMU 10.0 asks virglrenderer for its render server, and virglrenderer forks and execs
 * /usr/libexec/virgl_render_server itself unless the caller gives it an already connected socket through the
 * callbacks' get_server_fd (virglrenderer.h, callbacks version 3: "It allows the client to start the render server
 * externally. It can be used to sandbox the render server, or is required when the client process is sandboxed and
 * cannot fork/exec/socketpair"). QEMU doesn't set that callback, so this library puts it in: it stands in for
 * virgl_renderer_init, copies QEMU's callbacks, adds get_server_fd, and calls the real one. lord-vm starts the render
 * server itself, in a jail of its own (vmsandbox.h vm_jail_venus), and passes QEMU its end of the connection as the
 * descriptor named by LORDOS_VENUS_FD. QEMU then never forks or execs: its jail is the same as any 3D machine's.
 *
 * The descriptor is taken once, and only when it is a connected AF_UNIX SOCK_SEQPACKET socket; otherwise the callback
 * isn't added, virglrenderer falls back to forking, and QEMU's allowlist stops that (the machine reports it). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* virglrenderer 1.1's struct virgl_renderer_callbacks (src/virglrenderer.h), as far as version 4 */
struct cbs {
    int version;
    void (*write_fence)(void *cookie, uint32_t fence);
    void *(*create_gl_context)(void *cookie, int scanout_idx, void *param);
    void (*destroy_gl_context)(void *cookie, void *ctx);
    int (*make_current)(void *cookie, int scanout_idx, void *ctx);
    int (*get_drm_fd)(void *cookie);                                                        /* v2 */
    void (*write_context_fence)(void *cookie, uint32_t ctx_id, uint32_t ring_idx, uint64_t fence_id);  /* v3 */
    int (*get_server_fd)(void *cookie, uint32_t version);                                    /* v3 */
    void *(*get_egl_display)(void *cookie);                                                  /* v4 */
};

static struct cbs mine;
static int server_fd = -2;          /* -2: not read yet; -1: none (or already handed over) */

/* LORDOS_VENUS_FD: a decimal descriptor number from 3 to 1023, a connected SOCK_SEQPACKET AF_UNIX socket */
static int take_fd(void) {
    const char *v = getenv("LORDOS_VENUS_FD");
    if (!v || !*v || strlen(v) > 4) return -1;
    int n = 0;
    for (const char *p = v; *p; p++) {
        if (*p < '0' || *p > '9') return -1;
        n = n * 10 + (*p - '0');
    }
    if (n < 3 || n > 1023 || fcntl(n, F_GETFD) < 0) return -1;
    int type = 0, dom = 0;
    socklen_t l = sizeof type, l2 = sizeof dom;
    struct sockaddr_storage peer;
    socklen_t pl = sizeof peer;
    if (getsockopt(n, SOL_SOCKET, SO_TYPE, &type, &l) || type != SOCK_SEQPACKET) return -1;
    if (getsockopt(n, SOL_SOCKET, SO_DOMAIN, &dom, &l2) || dom != AF_UNIX) return -1;
    if (getpeername(n, (struct sockaddr *)&peer, &pl)) return -1;           /* connected */
    fcntl(n, F_SETFD, FD_CLOEXEC);
    return n;
}

/* virglrenderer takes the descriptor's ownership; given once. `version` is the render server protocol virglrenderer
 * speaks (its RENDER_SERVER_VERSION); the server lord-vm started is the same package's, so any is accepted */
static int get_server_fd(void *cookie, uint32_t version) {
    (void)cookie;
    (void)version;
    int fd = server_fd;
    server_fd = -1;
    return fd;
}

__attribute__((visibility("default"))) int virgl_renderer_init(void *cookie, int flags, struct cbs *cb) {
    /* QEMU's module brought virglrenderer in with dlopen (a local scope RTLD_NEXT doesn't search): taken from it by name,
     * already loaded, never loaded here */
    void *h = dlopen("libvirglrenderer.so.1", RTLD_LAZY | RTLD_NOLOAD);
    int (*real)(void *, int, struct cbs *) = h ? (int (*)(void *, int, struct cbs *))dlsym(h, "virgl_renderer_init") : NULL;
    if (h) dlclose(h);          /* (only the reference RTLD_NOLOAD added) */
    if (!real || (void *)real == (void *)virgl_renderer_init) return -1;
    if (server_fd == -2) { server_fd = take_fd(); unsetenv("LORDOS_VENUS_FD"); }
    if (!cb || server_fd < 0 || cb->version < 1) return real(cookie, flags, cb);
    memset(&mine, 0, sizeof mine);
    size_t have = cb->version >= 4 ? sizeof mine : cb->version == 3 ? offsetof(struct cbs, get_egl_display)
                : cb->version == 2 ? offsetof(struct cbs, write_context_fence) : offsetof(struct cbs, get_drm_fd);
    memcpy(&mine, cb, have);
    mine.version = cb->version >= 4 ? 4 : 3;
    mine.get_server_fd = get_server_fd;
    return real(cookie, flags, &mine);
}
