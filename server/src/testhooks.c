/* testhooks.c — the conformance tool's socket on a headless lord-server (see testhooks.h). */
#include "testhooks.h"

#ifndef LORD_TEST_HOOKS
#error "testhooks.c is only for build/lord-server-test (-DLORD_TEST_HOOKS)"
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define TH_CLIENTS 4

static TestHooks host;
static int lfd = -1;
static int cl[TH_CLIENTS] = { -1, -1, -1, -1 };
static char sock_path[108];

int testhooks_start(const TestHooks *h) {
    const char *rt = getenv("XDG_RUNTIME_DIR");
    struct stat st;
    if (!h || !rt || !*rt || lstat(rt, &st) || !S_ISDIR(st.st_mode) || st.st_uid != getuid()) return -1;
    if ((size_t)snprintf(sock_path, sizeof sock_path, "%s/lordos-test-hooks.sock", rt) >= sizeof sock_path) return -1;
    host = *h;
    lfd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (lfd < 0) return -1;
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    memcpy(a.sun_path, sock_path, strlen(sock_path) + 1);
    unlink(sock_path);
    mode_t old = umask(077);
    int r = bind(lfd, (struct sockaddr *)&a, sizeof a);
    umask(old);
    if (r || listen(lfd, 4)) { close(lfd); lfd = -1; return -1; }
    fprintf(stderr, "lord-server: test hooks on %s (--test-hooks, headless only)\n", sock_path);
    return 0;
}

void testhooks_stop(void) {
    if (lfd < 0) return;
    for (int i = 0; i < TH_CLIENTS; i++) if (cl[i] >= 0) { close(cl[i]); cl[i] = -1; }
    close(lfd);
    lfd = -1;
    unlink(sock_path);
}

int testhooks_pollfds(struct pollfd *p, int max) {
    int n = 0;
    if (lfd < 0 || max < 1) return 0;
    p[n++] = (struct pollfd){ lfd, POLLIN, 0 };
    for (int i = 0; i < TH_CLIENTS && n < max; i++) if (cl[i] >= 0) p[n++] = (struct pollfd){ cl[i], POLLIN, 0 };
    return n;
}

static void reply(int fd, const char *text, int pass_fd) {
    struct iovec iov = { (void *)text, strlen(text) };
    struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1 };
    char cbuf[CMSG_SPACE(sizeof(int))];
    if (pass_fd >= 0) {
        memset(cbuf, 0, sizeof cbuf);
        m.msg_control = cbuf;
        m.msg_controllen = sizeof cbuf;
        struct cmsghdr *c = CMSG_FIRSTHDR(&m);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &pass_fd, sizeof(int));
    }
    if (sendmsg(fd, &m, MSG_NOSIGNAL | MSG_DONTWAIT) < 0) fprintf(stderr, "lord-server: test hooks: reply: %s\n", strerror(errno));
}

/* One command (testhooks.h); the answer goes back on the same connection. */
static void command(int fd, char *line) {
    char verb[16] = "", arg[16] = "";
    long a = 0, b = 0, c = 0;
    int n = sscanf(line, "%15s %ld %ld %ld", verb, &a, &b, &c);
    if (n >= 3 && !strcmp(verb, "ptr") && a >= 0 && b >= 0 && a < 16384 && b < 16384) { host.pointer((int)a, (int)b); reply(fd, "ok", -1); return; }
    if (n >= 3 && !strcmp(verb, "btn") && a >= 0 && a < 8 && (b == 0 || b == 1)) { host.button((int)a, (int)b); reply(fd, "ok", -1); return; }
    if (n >= 2 && !strcmp(verb, "wheel") && a >= -100 && a <= 100) { host.wheel((int)a); reply(fd, "ok", -1); return; }
    if (n >= 3 && !strcmp(verb, "key") && a > 0 && a < 0x300 && (b == 0 || b == 1) && host.key) {
        host.key((int)a, (int)b);
        reply(fd, "ok", -1);
        return;
    }
    if (n >= 4 && (!strcmp(verb, "pen") || !strcmp(verb, "pad")) && a >= 0 && a <= 0x1f && b >= 0 && b < 0x300) {
        int r = host.tablet_event(!strcmp(verb, "pad"), (unsigned)a, (unsigned)b, (int)c);
        reply(fd, r ? "err no tablet (tablet add first)" : "ok", -1);
        return;
    }
    if (sscanf(line, "%15s %15s", verb, arg) == 2 && !strcmp(verb, "tablet") && (!strcmp(arg, "add") || !strcmp(arg, "remove"))) {
        reply(fd, host.tablet(!strcmp(arg, "add")) ? "err tablet" : "ok", -1);
        return;
    }
    if (sscanf(line, "%15s %15s", verb, arg) == 2 && !strcmp(verb, "lease") && (!strcmp(arg, "plug") || !strcmp(arg, "unplug"))) {
        reply(fd, host.lease(!strcmp(arg, "plug")) ? "err lease" : "ok", -1);
        return;
    }
    if (sscanf(line, "%15s %15s %ld", verb, arg, &a) >= 2 && !strcmp(verb, "frame") && host.frame &&
        (!strcmp(arg, "output") || !strcmp(arg, "screenshot") || !strcmp(arg, "mirror") || !strcmp(arg, "portal") || !strcmp(arg, "window"))) {
        reply(fd, host.frame(arg, a) ? "err frame" : "ok", -1);
        return;
    }
    if (!strcmp(line, "where") && host.where) {
        int x = 0, y = 0;
        char r[48];
        host.where(&x, &y);
        snprintf(r, sizeof r, "ok %d %d", x, y);
        reply(fd, r, -1);
        return;
    }
    if (!strcmp(line, "painted") && host.painted) {
        char r[40];
        snprintf(r, sizeof r, "ok %lld", host.painted());
        reply(fd, r, -1);
        return;
    }
    if (!strcmp(line, "xwl")) {
        int f = host.xwl();
        if (f < 0) { reply(fd, "err xwl (Xwayland is running, or no Wayland)", -1); return; }
        reply(fd, "ok", f);
        close(f);
        return;
    }
    reply(fd, "err unknown command", -1);
}

void testhooks_dispatch(const struct pollfd *p, int n) {
    for (int i = 0; i < n; i++) {
        if (!p[i].revents) continue;
        if (p[i].fd == lfd) {
            int fd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
            if (fd < 0) continue;
            struct ucred cr;
            socklen_t len = sizeof cr;
            int slot = -1;
            for (int k = 0; k < TH_CLIENTS; k++) if (cl[k] < 0) { slot = k; break; }
            if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &len) || cr.uid != getuid() || slot < 0) { close(fd); continue; }
            cl[slot] = fd;
            continue;
        }
        for (int k = 0; k < TH_CLIENTS; k++) {
            if (cl[k] != p[i].fd) continue;
            char line[128];
            ssize_t r = recv(cl[k], line, sizeof line - 1, MSG_DONTWAIT);
            if (r <= 0) {
                if (r < 0 && (errno == EAGAIN || errno == EINTR)) break;
                close(cl[k]);
                cl[k] = -1;
                break;
            }
            line[r] = 0;
            while (r > 0 && (line[r - 1] == '\n' || line[r - 1] == ' ')) line[--r] = 0;
            command(cl[k], line);
            break;
        }
    }
}
