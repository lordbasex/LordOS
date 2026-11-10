/* secctx_test.c — wp_security_context_v1 (src/wl/secctx.c): the string bounds, the hidden globals, which
 * listen_fd is taken, and the protocol end to end. A server with the manager and two stand-in globals (one
 * every client sees, one named like a trusted-only interface) runs here; a forked client
 * (tests/secctx_test_client.c) talks to it over socketpairs, makes a context on a listening socket, connects
 * through it and checks what it sees. The server records, from the visible global's bind, how each client is
 * classified. */
#include "../src/wl/secctx.h"
#include "secctx_test.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>

static int fails;
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); fails++; } } while (0)

static const struct wl_interface visible_iface = { SECT_VISIBLE, 1, 0, NULL, 0, NULL };
static const struct wl_interface hidden_iface = { SECT_HIDDEN, 1, 0, NULL, 0, NULL };

static struct wl_client *denied;        /* may_create says no to this one */
static int binds, tagged;
static SecMeta seen;
static int may_create(struct wl_client *c) { return c != denied; }
static void bind_visible(struct wl_client *c, void *data, uint32_t ver, uint32_t id) {
    (void)data;
    struct wl_resource *r = wl_resource_create(c, &visible_iface, (int)ver, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    binds++;
    const SecMeta *m = secctx_of(c);
    if (m) { tagged++; seen = *m; }
}
static void bind_hidden(struct wl_client *c, void *data, uint32_t ver, uint32_t id) {
    (void)data;
    if (!wl_resource_create(c, &hidden_iface, (int)ver, id)) wl_client_post_no_memory(c);
}

static void test_strings(void) {
    char s[SECCTX_STR_MAX + 1];
    CHECK(!secctx_str_ok(NULL, SECCTX_STR_MAX));
    CHECK(!secctx_str_ok("", SECCTX_STR_MAX));
    CHECK(secctx_str_ok("a", SECCTX_STR_MAX));
    CHECK(secctx_str_ok("org.flatpak", SECCTX_STR_MAX));
    memset(s, 'a', SECCTX_STR_MAX - 1);
    s[SECCTX_STR_MAX - 1] = 0;
    CHECK(secctx_str_ok(s, SECCTX_STR_MAX));       /* 255 bytes */
    s[SECCTX_STR_MAX - 1] = 'a';
    s[SECCTX_STR_MAX] = 0;
    CHECK(!secctx_str_ok(s, SECCTX_STR_MAX));      /* 256 */
    CHECK(!secctx_str_ok("ab", 2));
    CHECK(secctx_str_ok("a", 2));
    CHECK(!secctx_str_ok("org flatpak", SECCTX_STR_MAX));
    CHECK(!secctx_str_ok("a\tb", SECCTX_STR_MAX));
    CHECK(!secctx_str_ok("a\nb", SECCTX_STR_MAX));
    CHECK(!secctx_str_ok("a\x7f", SECCTX_STR_MAX));
    CHECK(!secctx_str_ok("caf\xc3\xa9", SECCTX_STR_MAX));
    CHECK(secctx_str_ok("~!#$%&*+-./:;<=>?@[]^_{|}", SECCTX_STR_MAX));

    SecMeta m;
    unsigned set = 0;
    memset(&m, 0, sizeof m);
    CHECK(secctx_meta_set(&m, &set, SECCTX_ENGINE, "org.flatpak") == SECCTX_OK);
    CHECK(!strcmp(m.engine, "org.flatpak"));
    CHECK(secctx_meta_set(&m, &set, SECCTX_ENGINE, "io.other") == SECCTX_ERR_SET);
    CHECK(!strcmp(m.engine, "org.flatpak"));
    CHECK(secctx_meta_set(&m, &set, SECCTX_APP_ID, "bad id") == SECCTX_ERR_BAD);
    CHECK(!m.app_id[0]);
    CHECK(secctx_meta_set(&m, &set, SECCTX_APP_ID, "org.example.App") == SECCTX_OK);   /* a refused one sets nothing */
    CHECK(secctx_meta_set(&m, &set, SECCTX_APP_ID, "org.example.App") == SECCTX_ERR_SET);
    CHECK(secctx_meta_set(&m, &set, SECCTX_INSTANCE, s) == SECCTX_ERR_BAD);             /* too long */
    CHECK(secctx_meta_set(&m, &set, SECCTX_INSTANCE, "42") == SECCTX_OK);
    CHECK(!strcmp(m.instance_id, "42"));
    CHECK(secctx_meta_set(&m, &set, 3, "x") == SECCTX_ERR_BAD);
    CHECK(secctx_meta_set(&m, &set, -1, "x") == SECCTX_ERR_BAD);
}

static void test_hidden(void) {
    CHECK(secctx_hidden("wp_security_context_manager_v1"));
    CHECK(secctx_hidden(SECT_HIDDEN));
    CHECK(secctx_hidden("zwlr_screencopy_manager_v1"));
    CHECK(secctx_hidden("zwp_virtual_keyboard_manager_v1"));
    CHECK(secctx_hidden("ext_foreign_toplevel_list_v1"));
    CHECK(secctx_hidden(NULL));
    CHECK(!secctx_hidden("wl_compositor"));
    CHECK(!secctx_hidden("xdg_wm_base"));
    CHECK(!secctx_hidden("wl_seat"));
    CHECK(!secctx_hidden("zwp_text_input_manager_v3"));
    CHECK(!secctx_hidden(SECT_VISIBLE));
}

static void test_listen_fd(void) {
    int sv[2], p[2];
    CHECK(!secctx_listen_ok(-1));
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) == 0);
    CHECK(!secctx_listen_ok(sv[0]));               /* connected, not listening */
    close(sv[0]);
    close(sv[1]);
    CHECK(pipe2(p, O_CLOEXEC) == 0);
    CHECK(!secctx_listen_ok(p[0]));
    close(p[0]);
    close(p[1]);
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    CHECK(bind(s, (struct sockaddr *)&a, sizeof(sa_family_t)) == 0);    /* an abstract address of its own */
    CHECK(!secctx_listen_ok(s));                   /* bound, not listening yet */
    CHECK(listen(s, 4) == 0);
    CHECK(secctx_listen_ok(s));
    close(s);
    s = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    CHECK(!secctx_listen_ok(s));
    close(s);
}

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static void test_protocol(void) {
    struct wl_display *dpy = wl_display_create();
    CHECK(dpy != NULL);
    if (!dpy) return;
    CHECK(wl_global_create(dpy, &visible_iface, 1, NULL, bind_visible) != NULL);
    CHECK(wl_global_create(dpy, &hidden_iface, 1, NULL, bind_hidden) != NULL);
    CHECK(secctx_init(dpy, may_create) == 0);
    int fds[SECT_CONNS];
    for (int i = 0; i < SECT_CONNS; i++) {
        int sv[2];
        CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) == 0);
        struct wl_client *c = wl_client_create(dpy, sv[0]);
        CHECK(c != NULL);
        if (i == SECT_DENIED) denied = c;
        fds[i] = sv[1];
    }
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) _exit(sect_client(fds) ? 1 : 0);
    for (int i = 0; i < SECT_CONNS; i++) close(fds[i]);
    struct wl_event_loop *loop = wl_display_get_event_loop(dpy);
    int status = -1;
    double end = now_ms() + 20000;
    while (now_ms() < end) {
        wl_event_loop_dispatch(loop, 20);
        wl_display_flush_clients(dpy);
        if (waitpid(pid, &status, WNOHANG) == pid) break;
    }
    if (status == -1) { kill(pid, SIGKILL); waitpid(pid, &status, 0); printf("FAIL: the client timed out\n"); fails++; }
    else CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    /* the launcher and the denied one bound it untagged; the client of the context, tagged with its metadata */
    CHECK(binds == 3);
    CHECK(tagged == 1);
    CHECK(!strcmp(seen.engine, "org.flatpak"));
    CHECK(!strcmp(seen.app_id, "org.example.App"));
    CHECK(!strcmp(seen.instance_id, "42"));
    wl_display_destroy_clients(dpy);
    wl_display_destroy(dpy);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    test_strings();
    test_hidden();
    test_listen_fd();
    test_protocol();
    if (fails) { printf("secctx-test: %d failures\n", fails); return 1; }
    printf("secctx-test: ok\n");
    return 0;
}
