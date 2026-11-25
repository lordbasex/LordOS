/* vmlogind_test.c — lord-vm's logind side (src/vm/vmlogind.c) against a fake logind on a private D-Bus daemon:
 *   - logind's InhibitDelayMaxUSec is read, a delay and a block inhibitor are taken (their descriptors are real);
 *   - PrepareForShutdown(true) reaches the callback within 250 ms of being sent (it took ~30 s on 2026-11-23: a
 *     message queued by a synchronous call left POLLIN out of sd_bus_get_events, docs/VIRTUALIZATION.md 6.1), and
 *     PrepareForShutdown(false) doesn't call it;
 *   - closing the delay inhibitor's descriptor is what logind sees as "let go";
 *   - the bus going away ends the connection without a crash.
 * The old dispatch (events from sd_bus_get_events, sd_bus_process only once poll says so) runs beside it on a second
 * connection, for the record. Needs dbus-daemon: "skipped: needs dbus-daemon" otherwise. Nothing of this computer's
 * system bus is touched: DBUS_SYSTEM_BUS_ADDRESS points at the private daemon. */
#include "vm/vmlogind.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <systemd/sd-bus.h>
#include <time.h>
#include <unistd.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); } } while (0)

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void tlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void tlog(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "  vmlogind: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* ---- the fake logind (a child process): Inhibit, InhibitDelayMaxUSec, and PrepareForShutdown on request ---- */

#define FAKE_DELAY_US 30000000ULL
#define MAX_INH 8
static int inh_w[MAX_INH], inh_shutdown[MAX_INH], n_inh;

static int fake_inhibit(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u; (void)e;
    const char *what, *who, *why, *mode;
    if (sd_bus_message_read(m, "ssss", &what, &who, &why, &mode) < 0) return -EINVAL;
    int pf[2];
    if (n_inh >= MAX_INH || pipe2(pf, O_CLOEXEC) < 0) return -ENOMEM;
    inh_w[n_inh] = pf[1];
    inh_shutdown[n_inh] = !strcmp(what, "shutdown") && !strcmp(mode, "delay");
    n_inh++;
    int r = sd_bus_reply_method_return(m, "h", pf[0]);
    close(pf[0]);
    return r;
}

static int fake_delay(sd_bus *b, const char *path, const char *iface, const char *prop, sd_bus_message *reply, void *u, sd_bus_error *e) {
    (void)b; (void)path; (void)iface; (void)prop; (void)u; (void)e;
    return sd_bus_message_append(reply, "t", (uint64_t)FAKE_DELAY_US);
}

static const sd_bus_vtable fake_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Inhibit", "ssss", "h", fake_inhibit, 0),
    SD_BUS_PROPERTY("InhibitDelayMaxUSec", "t", fake_delay, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_SIGNAL("PrepareForShutdown", "b", 0),
    SD_BUS_VTABLE_END
};

/* ctrl: 'T'/'F' emit PrepareForShutdown(true/false); writes 'R' when the delay inhibitor's descriptor is closed */
static void fake_logind(const char *addr, int ctrl) {
    sd_bus *b = NULL;
    if (sd_bus_new(&b) < 0 || sd_bus_set_address(b, addr) < 0 || sd_bus_set_bus_client(b, 1) < 0 || sd_bus_start(b) < 0) _exit(10);
    if (sd_bus_add_object_vtable(b, NULL, "/org/freedesktop/login1", "org.freedesktop.login1.Manager", fake_vtable, NULL) < 0) _exit(11);
    if (sd_bus_request_name(b, "org.freedesktop.login1", 0) < 0) _exit(12);
    if (write(ctrl, "U", 1) != 1) _exit(13);         /* up */
    for (;;) {
        while (sd_bus_process(b, NULL) > 0) ;
        struct pollfd p[2 + MAX_INH];
        int n = 0;
        p[n++] = (struct pollfd){ sd_bus_get_fd(b), (short)sd_bus_get_events(b), 0 };
        p[n++] = (struct pollfd){ ctrl, POLLIN, 0 };
        for (int i = 0; i < n_inh; i++) p[n++] = (struct pollfd){ inh_w[i], 0, 0 };   /* POLLERR: the reader closed */
        if (poll(p, (nfds_t)n, 1000) < 0 && errno != EINTR) _exit(14);
        if (p[1].revents & (POLLHUP | POLLERR)) _exit(0);
        if (p[1].revents & POLLIN) {
            char c;
            if (read(ctrl, &c, 1) != 1) _exit(0);
            if (c == 'T' || c == 'F')
                sd_bus_emit_signal(b, "/org/freedesktop/login1", "org.freedesktop.login1.Manager", "PrepareForShutdown", "b", c == 'T');
            sd_bus_flush(b);
        }
        for (int i = 0; i < n_inh; i++)
            if (inh_w[i] >= 0 && (p[2 + i].revents & (POLLERR | POLLHUP))) {
                if (inh_shutdown[i] && write(ctrl, "R", 1) != 1) _exit(15);
                close(inh_w[i]);
                inh_w[i] = -1;
            }
        if (sd_bus_is_open(b) <= 0) _exit(0);
    }
}

/* ---- lord-vm's side ---- */

static int shutdowns;
static long long shutdown_at;
static void on_shutdown(void) { shutdowns++; if (!shutdown_at) shutdown_at = now_ms(); }

/* the dispatch lord-vm had before 2026-11-23, on a second connection */
static int old_seen;
static long long old_at;
static int old_cb(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u; (void)e;
    int start = 0;
    if (sd_bus_message_read(m, "b", &start) >= 0 && start) { old_seen = 1; if (!old_at) old_at = now_ms(); }
    return 0;
}

/* one turn of a lord-vm-like loop (tag 0 = vmlogind, 1 = the old dispatch, 2 = the fake's control socket) */
static void turn(sd_bus *old, int ctrl, int wait_ms, char *got) {
    struct pollfd p[3];
    int tag[3], n = 0;
    long long next = now_ms() + wait_ms;
    if (vml_poll(&p[n], now_ms(), &next)) tag[n++] = 0;
    if (old) { p[n] = (struct pollfd){ sd_bus_get_fd(old), (short)sd_bus_get_events(old), 0 }; tag[n++] = 1; }
    p[n] = (struct pollfd){ ctrl, POLLIN, 0 }; tag[n++] = 2;
    long long now = now_ms();
    int to = next <= now ? 0 : (int)(next - now);
    if (poll(p, (nfds_t)n, to) < 0 && errno != EINTR) return;
    for (int i = 0; i < n; i++) {
        if (!p[i].revents) continue;
        if (tag[i] == 0) vml_process();
        else if (tag[i] == 1) { while (sd_bus_process(old, NULL) > 0) ; }
        else if (got && read(ctrl, got, 1) != 1) *got = 0;
    }
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    if (access("/usr/bin/dbus-daemon", X_OK)) { printf("vmlogind-test: skipped: needs dbus-daemon\n"); return 0; }
    char dir[] = "/tmp/vmlogind-test-XXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    char conf[PATH_MAX], sock[PATH_MAX], addr[PATH_MAX + 16];
    snprintf(conf, sizeof conf, "%s/bus.conf", dir);
    snprintf(sock, sizeof sock, "%s/bus", dir);
    snprintf(addr, sizeof addr, "unix:path=%s", sock);
    FILE *f = fopen(conf, "w");
    if (!f) { perror("bus.conf"); return 1; }
    fprintf(f, "<!DOCTYPE busconfig PUBLIC \"-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN\" "
               "\"http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd\">\n"
               "<busconfig><type>session</type><listen>%s</listen><auth>EXTERNAL</auth>"
               "<policy context=\"default\"><allow send_destination=\"*\"/><allow receive_sender=\"*\"/><allow own=\"*\"/><allow user=\"*\"/></policy></busconfig>\n", addr);
    fclose(f);
    pid_t daemon = fork();
    if (!daemon) {
        int null = open("/dev/null", O_RDWR);
        dup2(null, 1);
        char arg[PATH_MAX + 16];
        snprintf(arg, sizeof arg, "--config-file=%s", conf);
        execl("/usr/bin/dbus-daemon", "dbus-daemon", arg, "--nofork", "--nopidfile", (char *)NULL);
        _exit(127);
    }
    for (int i = 0; i < 100 && access(sock, F_OK); i++) { struct timespec ts = { 0, 20000000 }; nanosleep(&ts, NULL); }
    if (access(sock, F_OK)) { fprintf(stderr, "vmlogind-test: the private dbus-daemon didn't start\n"); kill(daemon, SIGKILL); return 1; }

    int sp[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sp) < 0) { perror("socketpair"); return 1; }
    pid_t fake = fork();
    if (!fake) { close(sp[0]); fake_logind(addr, sp[1]); _exit(0); }
    close(sp[1]);
    int ctrl = sp[0];
    char c = 0;
    struct pollfd up = { ctrl, POLLIN, 0 };
    CHECK(poll(&up, 1, 5000) == 1 && read(ctrl, &c, 1) == 1 && c == 'U');

    setenv("DBUS_SYSTEM_BUS_ADDRESS", addr, 1);
    /* lord-vm's order: open (AddMatch, Get), the delay inhibitor, the idle one (awake_update) */
    CHECK(vml_open(tlog, on_shutdown) == 0);
    CHECK(vml_delay_ms() == (long long)(FAKE_DELAY_US / 1000));
    int inh = vml_inhibit("shutdown", "test", "delay");
    CHECK(inh >= 0);
    int idle = vml_inhibit("idle", "test", "block");
    CHECK(idle >= 0);

    /* the old dispatch, set up the way lordvm.c did */
    sd_bus *old = NULL;
    CHECK(sd_bus_open_system(&old) >= 0);
    if (old) {
        sd_bus_match_signal(old, NULL, "org.freedesktop.login1", "/org/freedesktop/login1", "org.freedesktop.login1.Manager",
                            "PrepareForShutdown", old_cb, NULL);
        sd_bus_message *rep = NULL;
        sd_bus_call_method(old, "org.freedesktop.login1", "/org/freedesktop/login1", "org.freedesktop.login1.Manager", "Inhibit", NULL, &rep,
                           "ssss", "shutdown", "LordOS Machines", "old", "delay");
        sd_bus_message_unref(rep);
    }

    /* a while of nothing, as a running machine's loop: nothing may be lost meanwhile */
    for (long long end = now_ms() + 300; now_ms() < end;) turn(old, ctrl, 50, NULL);
    CHECK(shutdowns == 0);

    /* PrepareForShutdown(false) (a shutdown cancelled): no stop */
    CHECK(write(ctrl, "F", 1) == 1);
    for (long long end = now_ms() + 200; now_ms() < end;) turn(old, ctrl, 50, NULL);
    CHECK(shutdowns == 0);

    /* PrepareForShutdown(true): the callback within 250 ms (lord-vm's loop may sleep in poll with no timeout) */
    long long sent = now_ms();
    CHECK(write(ctrl, "T", 1) == 1);
    while (now_ms() - sent < 1500 && (!shutdowns || !old_seen)) turn(old, ctrl, shutdowns ? 100 : 100000, NULL);
    CHECK(shutdowns == 1);
    long long took = shutdown_at ? shutdown_at - sent : -1;
    printf("vmlogind-test: PrepareForShutdown handled %lld ms after it was sent\n", took);
    CHECK(took >= 0 && took < 250);
    if (old_seen) printf("vmlogind-test: the old dispatch saw it after %lld ms\n", old_at - sent);
    else printf("vmlogind-test: the old dispatch didn't see it in 1500 ms (the 2026-11-23 bug: it waited for the bus to hang up)\n");

    /* the machine handled: the delay inhibitor goes, and the fake logind sees it go */
    close(inh);
    char got = 0;
    for (long long end = now_ms() + 2000; now_ms() < end && got != 'R';) turn(NULL, ctrl, 100, &got);
    CHECK(got == 'R');
    close(idle);

    /* the bus goes away: vml_poll says so (no descriptor), without a crash */
    kill(daemon, SIGTERM);
    waitpid(daemon, NULL, 0);
    int gone = 0;
    for (long long end = now_ms() + 2000; now_ms() < end && !gone;) {
        struct pollfd p;
        long long next = -1;
        if (!vml_poll(&p, now_ms(), &next)) { gone = 1; break; }
        if (poll(&p, 1, 100) > 0) vml_process();
    }
    CHECK(gone);
    vml_close();
    if (old) sd_bus_flush_close_unref(old);
    close(ctrl);
    kill(fake, SIGTERM);
    waitpid(fake, NULL, 0);
    unlink(conf);
    unlink(sock);
    rmdir(dir);
    printf("vmlogind-test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
