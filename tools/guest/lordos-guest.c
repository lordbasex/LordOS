/* lordos-guest — Machines' clipboard helper inside a Linux guest (docs/VIRTUALIZATION.md 6.8, 6.17, 7.1.1). Opt-in: it
 * runs only where the machine's guest agent is on and someone installed it (Configure → Guest agent → Install, through
 * qemu-guest-agent, or by hand). One small static program, no runtime.
 *
 * It holds the guest's end of the virtio-serial port org.lordos.agent (/dev/virtio-ports/org.lordos.agent; a udev rule
 * gives the signed-in user access) and speaks the frames of server/src/vm/vmagent.h: it says hello, sends the
 * clipboard's text when it changes, and puts the text the host sends on the clipboard. lord-vm decides which way the
 * text may go (machine.json "clipboard"); this side only does what arrives. It reads and writes the clipboard with the
 * guest's own tools: wl-paste/wl-copy (Wayland) or xclip (X11), started without a shell. It never runs anything the
 * host names, and the host can't ask it for anything else.
 *
 *   lordos-guest [--port PATH] [--once]      (--once: one look at the clipboard and the port, for tests) */
#include "../../server/src/vm/vmagent.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define VERSION "1"
#define PORT "/dev/virtio-ports/org.lordos.agent"
#define CLIP_MAX AG_FRAME_MAX

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

static long clip_read(uint8_t *out, size_t cap) {
    if (wayland()) { char *a[] = { "wl-paste", "-n", "-t", "text/plain;charset=utf-8", NULL }; long n = run(a, NULL, 0, out, cap); if (n >= 0) return n; }
    char *x[] = { "xclip", "-o", "-selection", "clipboard", "-t", "UTF8_STRING", NULL };
    return run(x, NULL, 0, out, cap);
}

static int clip_write(const uint8_t *p, size_t n) {
    if (wayland()) { char *a[] = { "wl-copy", "--type", "text/plain;charset=utf-8", NULL }; if (run(a, p, n, NULL, 0) >= 0) return 0; }
    char *x[] = { "xclip", "-i", "-selection", "clipboard", NULL };
    return run(x, p, n, NULL, 0) >= 0 ? 0 : -1;
}

static int send_frame(int fd, uint16_t kind, const uint8_t *p, uint32_t n) {
    uint8_t h[AG_HEAD];
    ag_frame_head(h, kind, 0, n);
    struct iovec io[2] = { { h, AG_HEAD }, { (void *)p, n } };
    return writev(fd, io, n ? 2 : 1) == (ssize_t)(AG_HEAD + n) ? 0 : -1;
}

int main(int argc, char **argv) {
    const char *port = PORT;
    int once = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) port = argv[++i];
        else if (!strcmp(argv[i], "--once")) once = 1;
        else if (!strcmp(argv[i], "--version")) { printf("lordos-guest %s\n", VERSION); return 0; }
        else { fprintf(stderr, "usage: lordos-guest [--port PATH] [--once]\n"); return 2; }
    }
    signal(SIGPIPE, SIG_IGN);
    static uint8_t buf[AG_HEAD + AG_FRAME_MAX], clip[CLIP_MAX];
    size_t have = 0;
    uint64_t last = 0;                  /* the clipboard text last sent or set, so it doesn't go back and forth */
    int fd = -1;
    long long next_look = 0;
    for (;;) {
        if (fd < 0) {
            fd = open(port, O_RDWR | O_CLOEXEC | O_NOCTTY);
            if (fd < 0) { if (once) { perror(port); return 1; } sleep(3); continue; }
            static const char hello[] = "lordos-guest " VERSION " clipboard";
            send_frame(fd, AGK_HELLO, (const uint8_t *)hello, sizeof hello - 1);
            have = 0;
        }
        long long now = now_ms();
        if (now >= next_look) {
            next_look = now + 1000;
            long n = clip_read(clip, sizeof clip);
            if (n > 0 && ag_utf8_ok(clip, (size_t)n)) {
                uint64_t h = hash(clip, (size_t)n);
                if (h != last) { last = h; if (send_frame(fd, AGK_CLIP, clip, (uint32_t)n)) { close(fd); fd = -1; continue; } }
            }
            if (once) return 0;
        }
        struct pollfd p = { fd, POLLIN, 0 };
        int r = poll(&p, 1, (int)(next_look - now_ms() > 0 ? next_look - now_ms() : 0));
        if (r < 0 && errno != EINTR) { close(fd); fd = -1; continue; }
        if (r <= 0) continue;
        if (p.revents & (POLLHUP | POLLERR) && !(p.revents & POLLIN)) { close(fd); fd = -1; sleep(1); continue; }
        ssize_t k = read(fd, buf + have, sizeof buf - have);
        if (k <= 0) { if (k < 0 && (errno == EAGAIN || errno == EINTR)) continue; close(fd); fd = -1; sleep(1); continue; }
        have += (size_t)k;
        for (;;) {
            AgFrame f;
            int used = ag_frame_next(buf, have, &f);
            if (used < 0 || (!used && have == sizeof buf)) { have = 0; break; }
            if (!used) break;
            if (f.kind == AGK_CLIP && f.len && ag_utf8_ok(f.data, f.len)) {
                uint64_t h = hash(f.data, f.len);
                if (h != last && !clip_write(f.data, f.len)) last = h;
            }
            memmove(buf, buf + used, have - (size_t)used);
            have -= (size_t)used;
        }
    }
}
