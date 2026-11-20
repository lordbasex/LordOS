/* lordos-guest — Machines' clipboard helper inside a Linux guest (docs/VIRTUALIZATION.md 6.8, 6.17, 7.1.1). Opt-in: it
 * runs only where the machine's guest agent is on and someone installed it (Configure → Guest agent → Install, through
 * qemu-guest-agent, or by hand). One small static program, no runtime.
 *
 * It holds the guest's end of the virtio-serial port org.lordos.agent (/dev/virtio-ports/org.lordos.agent; a udev rule
 * gives the signed-in user access) and speaks the frames of server/src/vm/vmagent.h: it says hello, sends the
 * clipboard when it changes, and puts what the host sends on the clipboard: text, rich text (text/html, with its plain
 * text) and pictures (image/png), as AGK_CLIPX when the host's hello says "clipx", text alone otherwise. lord-vm
 * decides which way it may go (machine.json "clipboard") and checks what this side sends; this side only does what
 * arrives. It reads and writes the clipboard with the guest's own tools: wl-paste/wl-copy (Wayland) or xclip (X11),
 * started without a shell. It never runs anything the host names, and the host can't ask it for anything else.
 * Pasting rich text or a picture into the guest offers that one form (the tools hold one type each).
 *
 * Apps mode (7.2) on a guest that runs LordOS: the guest's own lord-server lists its windows on a local socket of the
 * signed-in person ($XDG_RUNTIME_DIR/lordos-guestwin.sock, server/src/ui/js_guestwin.c). While that socket answers,
 * the hello says "windows" too, and frames are relayed both ways, each checked with vmagent.c's parsers and written
 * again: the windows, their icons and the launcher's app list toward the host; apps mode on or off, an action on a
 * listed window and the launch of a listed app toward lord-server (which checks them again). Nothing here runs an app.
 *
 *   lordos-guest [--port PATH] [--windows PATH] [--once]
 *     --port: the agent's port (a character device; a Unix socket path for tests); --windows: lord-server's socket
 *     (default $XDG_RUNTIME_DIR/lordos-guestwin.sock); --once: one look at the clipboard and the port, for tests */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../../server/src/vm/vmagent.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define VERSION "3"
#define PORT "/dev/virtio-ports/org.lordos.agent"
#define CLIP_MAX AG_TEXT_MAX

static uint64_t hash(const uint8_t *p, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    return h ^ n;
}

static long long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* one of the clipboard tools: argv, with stdin from `in` (when writing) or stdout into out (when reading), 2 s at most */
static long run(char *const argv[], const uint8_t *in, size_t in_n, uint8_t *out, size_t cap) {
    int ip[2] = { -1, -1 }, op[2] = { -1, -1 };
    if ((in && pipe(ip)) || (out && pipe(op))) return -1;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (!pid) {
        int null = open("/dev/null", O_RDWR);
        dup2(in ? ip[0] : null, 0);
        dup2(out ? op[1] : null, 1);
        dup2(null, 2);
        for (int f = 3; f < 64; f++) close(f);
        execvp(argv[0], argv);
        _exit(127);
    }
    if (in) { close(ip[0]); size_t o = 0; while (o < in_n) { ssize_t w = write(ip[1], in + o, in_n - o); if (w <= 0) break; o += (size_t)w; } close(ip[1]); }
    long got = 0;
    if (out) {
        close(op[1]);
        long long end = now_ms() + 2000;
        for (;;) {
            struct pollfd p = { op[0], POLLIN, 0 };
            long long left = end - now_ms();
            if (left <= 0 || poll(&p, 1, (int)left) <= 0) { kill(pid, SIGKILL); break; }
            ssize_t r = read(op[0], out + got, cap - (size_t)got);
            if (r <= 0) break;
            got += r;
            if ((size_t)got == cap) { kill(pid, SIGKILL); break; }
        }
        close(op[0]);
    }
    int st = 0;
    for (int i = 0; i < 200 && waitpid(pid, &st, WNOHANG) == 0; i++) { struct timespec ts = { 0, 10000000 }; nanosleep(&ts, NULL); }
    if (waitpid(pid, &st, WNOHANG) == 0) { kill(pid, SIGKILL); waitpid(pid, &st, 0); }
    if (!WIFEXITED(st) || WEXITSTATUS(st)) return out ? -1 : -1;
    return got;
}

static int wayland(void) { return getenv("WAYLAND_DISPLAY") != NULL; }

/* one form of the clipboard: "text" (UTF-8), "text/html", "image/png" */
static long clip_read_type(const char *type, uint8_t *out, size_t cap) {
    int text = !strcmp(type, "text");
    if (wayland()) {
        char *a[] = { "wl-paste", "-n", "-t", text ? "text/plain;charset=utf-8" : (char *)type, NULL };
        long n = run(a, NULL, 0, out, cap);
        if (n >= 0) return n;
    }
    char *x[] = { "xclip", "-o", "-selection", "clipboard", "-t", text ? "UTF8_STRING" : (char *)type, NULL };
    return run(x, NULL, 0, out, cap);
}
static long clip_read(uint8_t *out, size_t cap) { return clip_read_type("text", out, cap); }

/* the types the clipboard offers, one per line (wl-paste -l, or xclip's TARGETS) */
static long clip_types(char *out, size_t cap) {
    long n = -1;
    if (wayland()) { char *a[] = { "wl-paste", "-l", NULL }; n = run(a, NULL, 0, (uint8_t *)out, cap - 1); }
    if (n < 0) { char *x[] = { "xclip", "-o", "-selection", "clipboard", "-t", "TARGETS", NULL }; n = run(x, NULL, 0, (uint8_t *)out, cap - 1); }
    out[n > 0 ? n : 0] = 0;
    return n;
}
static int has_type(const char *types, const char *t) {
    size_t l = strlen(t);
    for (const char *p = types; (p = strstr(p, t)); p += l)
        if ((p == types || p[-1] == '\n') && (p[l] == '\n' || p[l] == 0 || p[l] == '\r')) return 1;
    return 0;
}

static int clip_write_type(const char *type, const uint8_t *p, size_t n) {
    int text = !strcmp(type, "text");
    if (wayland()) { char *a[] = { "wl-copy", "--type", text ? "text/plain;charset=utf-8" : (char *)type, NULL }; if (run(a, p, n, NULL, 0) >= 0) return 0; }
    char *x[] = { "xclip", "-i", "-selection", "clipboard", "-t", text ? "UTF8_STRING" : (char *)type, NULL };
    return run(x, p, n, NULL, 0) >= 0 ? 0 : -1;
}
static int clip_write(const uint8_t *p, size_t n) { return clip_write_type("text", p, n); }

static int send_frame(int fd, uint16_t kind, const uint8_t *p, uint32_t n) {
    uint8_t h[AG_HEAD];
    ag_frame_head(h, kind, 0, n);
    struct iovec io[2] = { { h, AG_HEAD }, { (void *)p, n } };
    return writev(fd, io, n ? 2 : 1) == (ssize_t)(AG_HEAD + n) ? 0 : -1;
}

/* ---- apps mode: the relay to the guest's own lord-server ---- */

#define WIN_BUF (AG_HEAD + LAG_APPLIST_BYTES + 1024)
static int wfd = -1;                    /* lord-server's endpoint, or -1 */
static uint8_t *win_in, *win_tmp;
static size_t win_n;
static LagAppList *app_list;            /* the launcher's apps lord-server listed: the only tokens sent on */
static LagWinList *win_list;

static int unix_connect(const char *path) {
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    if (strlen(path) >= sizeof a.sun_path) return -1;
    memcpy(a.sun_path, path, strlen(path) + 1);
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0) return -1;
    if (connect(s, (struct sockaddr *)&a, sizeof a)) { close(s); return -1; }
    return s;
}

static void say_hello(int fd) {
    char h[96];
    snprintf(h, sizeof h, "lordos-guest " VERSION " clipboard clipx%s", wfd >= 0 ? " windows" : "");
    send_frame(fd, AGK_HELLO, (const uint8_t *)h, (uint32_t)strlen(h));
}

static void win_close(int port_fd) {
    if (wfd < 0) return;
    close(wfd);
    wfd = -1;
    win_n = 0;
    if (app_list) app_list->n = 0;
    if (port_fd >= 0) say_hello(port_fd);          /* no "windows" any more */
}

/* a frame from the host for lord-server: checked, written again, sent on */
static void to_server(const AgFrame *f) {
    if (wfd < 0) return;
    uint8_t out[64];
    size_t n = 0;
    if (f->kind == LAG_APPS && f->len == 1 && f->data[0] <= 1) { out[0] = f->data[0]; n = 1; }
    else if (f->kind == LAG_WINACT) {
        LagAct a;
        if (!lag_winact_parse(f->data, f->len, &a)) n = lag_act_write(&a, out, sizeof out);
    } else if (f->kind == LAG_LAUNCH) {
        char tok[LAG_TOKEN_MAX + 1];
        static uint8_t lo[LAG_TOKEN_MAX + 2];
        if (!lag_launch_token(f->data, f->len, tok, sizeof tok) && app_list && lag_applist_has(app_list, tok)) {
            size_t k = lag_launch_write(tok, lo, sizeof lo);
            if (k) send_frame(wfd, LAG_LAUNCH, lo, (uint32_t)k);
        }
        return;
    }
    if (n) send_frame(wfd, f->kind, out, (uint32_t)n);
}

/* frames from lord-server: the hello, then windows, icons and the app list for the host. 0 fine, -1 the socket broke */
static int from_server(int port_fd) {
    ssize_t r = read(wfd, win_in + win_n, WIN_BUF - win_n);
    if (r <= 0) { if (r < 0 && (errno == EAGAIN || errno == EINTR)) return 0; return -1; }
    win_n += (size_t)r;
    for (;;) {
        AgFrame f;
        int used = ag_frame_next(win_in, win_n, &f);
        if (used < 0 || (!used && win_n == WIN_BUF)) return -1;
        if (!used) break;
        size_t n = 0;
        if (f.kind == LAG_WINDOWS && !lag_winlist_read(f.data, f.len, win_list)) n = lag_windows_write(win_list, win_tmp, WIN_BUF);
        else if (f.kind == LAG_WINICON) {
            uint32_t id; uint16_t w, h; const uint8_t *px;
            if (!lag_winicon_parse(f.data, f.len, &id, &w, &h, &px)) n = lag_icon_write(id, w, h, px, win_tmp, WIN_BUF);
        } else if (f.kind == LAG_APPLIST && !lag_applist_read(f.data, f.len, app_list)) n = lag_applist_write(app_list, win_tmp, WIN_BUF);
        if (n && port_fd >= 0 && send_frame(port_fd, f.kind, win_tmp, (uint32_t)n)) return -2;
        memmove(win_in, win_in + used, win_n - (size_t)used);
        win_n -= (size_t)used;
    }
    return 0;
}

/* the port: the virtio-serial character device, or (tests) a Unix socket */
static int open_port(const char *port) {
    struct stat st;
    if (!stat(port, &st) && S_ISSOCK(st.st_mode)) return unix_connect(port);
    return open(port, O_RDWR | O_CLOEXEC | O_NOCTTY);
}

int main(int argc, char **argv) {
    const char *port = PORT;
    char wpath[256] = "";
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (rt && rt[0] == '/') snprintf(wpath, sizeof wpath, "%s/lordos-guestwin.sock", rt);
    int once = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) port = argv[++i];
        else if (!strcmp(argv[i], "--windows") && i + 1 < argc) snprintf(wpath, sizeof wpath, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--once")) once = 1;
        else if (!strcmp(argv[i], "--version")) { printf("lordos-guest %s\n", VERSION); return 0; }
        else { fprintf(stderr, "usage: lordos-guest [--port PATH] [--windows PATH] [--once]\n"); return 2; }
    }
    signal(SIGPIPE, SIG_IGN);
    win_in = malloc(WIN_BUF); win_tmp = malloc(WIN_BUF); app_list = calloc(1, sizeof *app_list); win_list = calloc(1, sizeof *win_list);
    if (!win_in || !win_tmp || !app_list || !win_list) return 1;
    long long win_try = 0;
    static uint8_t buf[AG_HEAD + AG_FRAME_MAX], clip[CLIP_MAX], html[AG_TEXT_MAX], png[AG_PNG_MAX], out[AG_FRAME_MAX];
    static char types[4096], last_types[4096];
    size_t have = 0;
    uint64_t last = 0;                  /* the clipboard's main form last sent or set, so it doesn't go back and forth */
    int fd = -1, host_clipx = 0;
    long long next_look = 0, png_look = 0;
    for (;;) {
        if (fd < 0) {
            fd = open_port(port);
            if (fd < 0) { if (once) { perror(port); return 1; } sleep(3); continue; }
            say_hello(fd);
            have = 0;
            host_clipx = 0;
        }
        long long now = now_ms();
        /* lord-server's endpoint: tried every 3 s while it isn't there; the hello says "windows" while it is */
        if (wfd < 0 && wpath[0] && now >= win_try) {
            win_try = now + 3000;
            wfd = unix_connect(wpath);
            if (wfd >= 0) { win_n = 0; say_hello(fd); }
        }
        if (now >= next_look) {
            next_look = now + 1000;
            int sent = 0;
            if (host_clipx && clip_types(types, sizeof types) > 0) {
                int types_changed = strcmp(types, last_types) != 0;
                snprintf(last_types, sizeof last_types, "%s", types);
                AgClip c;
                memset(&c, 0, sizeof c);
                if (has_type(types, "image/png")) {
                    /* a picture is read again when the offer changed, or every 5 s (reading it is the costly part) */
                    if (types_changed || now >= png_look) {
                        png_look = now + 5000;
                        long n = clip_read_type("image/png", png, sizeof png);
                        if (n > 0 && (size_t)n < sizeof png && !ag_png_check(png, (size_t)n, NULL, NULL)) { c.png = png; c.png_n = (uint32_t)n; }
                    }
                    sent = 1;               /* a picture's text, if any, isn't the clipboard */
                } else if (has_type(types, "text/html")) {
                    long n = clip_read_type("text/html", html, sizeof html);
                    if (n > 0 && (size_t)n < sizeof html && ag_utf8_ok(html, (size_t)n)) { c.html = html; c.html_n = (uint32_t)n; }
                    /* its plain text, only when the clipboard offers one (xclip would hand the HTML itself as text) */
                    long t = has_type(types, "UTF8_STRING") || has_type(types, "text/plain") || has_type(types, "text/plain;charset=utf-8")
                             ? clip_read(clip, sizeof clip) : -1;
                    if (t > 0 && (size_t)t < sizeof clip && ag_utf8_ok(clip, (size_t)t)) { c.text = clip; c.text_n = (uint32_t)t; }
                    sent = c.html != NULL;
                }
                if (c.png || c.html) {
                    uint64_t h = c.png ? hash(c.png, c.png_n) : hash(c.html, c.html_n);
                    if (h != last) {
                        last = h;
                        ag_clipx_write(&c, out);
                        if (send_frame(fd, AGK_CLIPX, out, (uint32_t)ag_clipx_size(&c))) { close(fd); fd = -1; continue; }
                    }
                }
            }
            if (!sent) {
                long n = clip_read(clip, sizeof clip);
                if (n > 0 && ag_utf8_ok(clip, (size_t)n)) {
                    uint64_t h = hash(clip, (size_t)n);
                    if (h != last) { last = h; if (send_frame(fd, AGK_CLIP, clip, (uint32_t)n)) { close(fd); fd = -1; continue; } }
                }
            }
            if (once) return 0;
        }
        struct pollfd pp[2] = { { fd, POLLIN, 0 }, { wfd, POLLIN, 0 } };
        int r = poll(pp, wfd >= 0 ? 2 : 1, (int)(next_look - now_ms() > 0 ? next_look - now_ms() : 0));
        if (r < 0 && errno != EINTR) { close(fd); fd = -1; continue; }
        if (r <= 0) continue;
        if (wfd >= 0 && (pp[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            int w = from_server(fd);
            if (w == -2) { close(fd); fd = -1; continue; }
            if (w < 0) win_close(fd);
        }
        struct pollfd p = pp[0];
        if (!p.revents) continue;
        if (p.revents & (POLLHUP | POLLERR) && !(p.revents & POLLIN)) { close(fd); fd = -1; sleep(1); continue; }
        ssize_t k = read(fd, buf + have, sizeof buf - have);
        if (k <= 0) { if (k < 0 && (errno == EAGAIN || errno == EINTR)) continue; close(fd); fd = -1; sleep(1); continue; }
        have += (size_t)k;
        for (;;) {
            AgFrame f;
            int used = ag_frame_next(buf, have, &f);
            if (used < 0 || (!used && have == sizeof buf)) { have = 0; break; }
            if (!used) break;
            if (f.kind == AGK_HELLO) {
                host_clipx = f.len < 200 && memmem(f.data, f.len, " clipx", 6) != NULL;
                /* a new lord-vm (after a resume) asks once: the hello again */
                if (f.len < 200 && memmem(f.data, f.len, " ask", 4)) say_hello(fd);
            }
            if (f.kind == LAG_APPS || f.kind == LAG_WINACT || f.kind == LAG_LAUNCH) to_server(&f);
            if (f.kind == AGK_CLIP && f.len && ag_utf8_ok(f.data, f.len)) {
                uint64_t h = hash(f.data, f.len);
                if (h != last && !clip_write(f.data, f.len)) last = h;
            }
            AgClip c;
            if (f.kind == AGK_CLIPX && !ag_clipx_parse(f.data, f.len, &c)) {
                /* one form goes on the clipboard: the picture, else the rich text, else the text */
                if (c.png && !ag_png_check(c.png, c.png_n, NULL, NULL)) { uint64_t h = hash(c.png, c.png_n); if (h != last && !clip_write_type("image/png", c.png, c.png_n)) last = h; }
                else if (c.html && ag_utf8_ok(c.html, c.html_n)) { uint64_t h = hash(c.html, c.html_n); if (h != last && !clip_write_type("text/html", c.html, c.html_n)) last = h; }
                else if (c.text && ag_utf8_ok(c.text, c.text_n)) { uint64_t h = hash(c.text, c.text_n); if (h != last && !clip_write(c.text, c.text_n)) last = h; }
                snprintf(last_types, sizeof last_types, "%s", "");
            }
            memmove(buf, buf + used, have - (size_t)used);
            have -= (size_t)used;
        }
    }
}
