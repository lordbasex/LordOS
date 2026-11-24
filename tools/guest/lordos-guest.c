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
 * The host's clipboard on demand (6.8, "clipreq" in both hellos): where the guest has an X display (X11, or Xwayland,
 * which GNOME, KDE and wlroots desktops bridge to the Wayland clipboard), this side owns the X CLIPBOARD itself
 * (x11clip.c: the X11 protocol, no Xlib, no tool) for the host's offer (AGK_CLIPOFFER: its forms, no data) and asks
 * the host for a form (AGK_CLIPREQ) only when a program pastes it; it waits AG_CLIP_WAIT_MS (AG_ASK_WAIT_MS when the
 * person is asked) and gives the program nothing when the answer isn't the data. While the offer is ours the guest's
 * clipboard isn't read (that would ask the host). Without an X display the host pushes, as before.
 *
 * Which tool: looked for in PATH at the start, every 30 s and whenever one couldn't be started (installing xclip later
 * starts working without a restart). On GNOME (XDG_CURRENT_DESKTOP) with DISPLAY set, xclip through Xwayland, which
 * Mutter bridges to the Wayland clipboard, and never wl-paste (Mutter has no data-control protocol: wl-clipboard needs
 * a focus-stealing popup there); elsewhere wl-clipboard under Wayland, then xclip. With neither installed the hello says
 * "noclip" instead of "clipboard clipx", so Machines says the guest needs xclip, and the journal says so once. Under
 * systemd the display's variables can arrive after the service started: they're read from the user's manager
 * (systemctl --user show-environment) while they're missing.
 *
 * Apps mode (7.2) on a guest that runs LordOS: the guest's own lord-server lists its windows on a local socket of the
 * signed-in person ($XDG_RUNTIME_DIR/lordos-guestwin.sock, server/src/ui/js_guestwin.c). While that socket answers,
 * the hello says "windows" too, and frames are relayed both ways, each checked with vmagent.c's parsers and written
 * again: the windows, their icons and the launcher's app list toward the host; apps mode on or off, an action on a
 * listed window and the launch of a listed app toward lord-server (which checks them again). Nothing here runs an app.
 *
 * The guest's shutdown (docs/VIRTUALIZATION.md "Graphics 3D"): systemd-shutdown runs every program in
 * /usr/lib/systemd/system-shutdown/ with poweroff, halt, reboot or kexec once all services have stopped and the file
 * systems are read-only. The hook there (packaging/systemd/lordos-guest-shutdown, or the one Install Agent writes)
 * runs `lordos-guest --stopping <verb>`, as root: it opens the port (free again: this helper's own run has ended),
 * sends AGK_STOPPING (vmagent.h) and waits up to AG_STOP_WAIT_MS for lord-vm's echo, then the guest goes on. No port
 * (a computer that isn't a Machines guest, or its agent is off): it ends at once. The hello says "stopping" while such
 * a hook is installed, so Machines knows a shutdown or restart would have been said.
 *
 * The machine's performance profile (docs/VIRTUALIZATION.md 7.1.4, tools 7, "perf" in the hello; perf.h): on AGK_PERF
 * this side, as the person and with its own D-Bus client (no program started), sets power-profiles-daemon's profile to
 * performance on the system bus and turns GNOME's animations off on the session bus, keeps what was there before and
 * puts it back on AGK_PERF 0, when the port goes, when it ends (SIGTERM, SIGINT, SIGHUP) and at the next start after a
 * crash. It answers AGK_PERFSTATE. Built without it (-DLG_NO_PERF), the hello doesn't say "perf".
 *
 * The agent's shell, and links and notifications for the host (tools 8, "shell links notify" in the hello; gshell.h):
 * the host opens a shell here as the person (their own login shell in a pty, never root, nothing named by the host);
 * the person's programs hand this helper a link or a notification over $XDG_RUNTIME_DIR/lordos-guest.sock, which goes
 * on only when the host's hello says the machine takes them.
 *
 * Tools 9 ("send" in the hello; gsend.h, gnotes.h): "Send to LordOS" (`lordos-guest --send-to-host FILE...`, the file
 * manager's entry lordos-send-to-host.desktop) sends files to the host's quarantine folder for this machine, one at a time,
 * with flow control and resume; and while the host takes notifications, the person's own desktop notifications go on too.
 *
 *   lordos-guest [--port PATH] [--windows PATH] [--once] [--system-bus PATH] [--session-bus PATH]
 *   lordos-guest [--port PATH] --stopping poweroff|halt|reboot|kexec
 *   lordos-guest --open-on-host URL | --notify-host TITLE [BODY] | --send-to-host FILE...
 *     --port: the agent's port (a character device; a Unix socket path for tests); --windows: lord-server's socket
 *     (default $XDG_RUNTIME_DIR/lordos-guestwin.sock); --once: one look at the clipboard and the port, for tests;
 *     --system-bus, --session-bus: other buses' sockets for the performance profile, for tests;
 *     --stopping: say the guest powers off or restarts now (above) and end */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../../server/src/vm/vmagent.h"
#include "x11clip.h"
#include "gshell.h"
#include "gsend.h"
#include "gnotes.h"
#ifndef LG_NO_PERF
#include "perf.h"
#endif

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

#define VERSION "9"
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
    if (WIFEXITED(st) && WEXITSTATUS(st) == 127) return -2;       /* not there (execvp failed): look for the tools again */
    if (!WIFEXITED(st) || WEXITSTATUS(st)) return -1;
    return got;
}

/* ---- the clipboard's tools: which ones this guest has, in the order they're tried ---- */
enum { T_WL = 1, T_XCLIP = 2 };
static int tools[2], tool_n;            /* tool_n 0: none (the hello says "noclip") */
static long long tool_look_at;          /* when to look for them again (0: at once) */
static char tool_said[160];             /* what the journal was told last */

/* an executable file of that name in PATH (as execvp would find it); no shell */
static int in_path(const char *name) {
    const char *path = getenv("PATH");
    if (!path || !*path) path = "/usr/local/bin:/usr/bin:/bin";
    for (const char *p = path; *p;) {
        const char *e = strchr(p, ':');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        char f[512];
        struct stat st;
        size_t nl = strlen(name);
        if (l && l + nl + 2 <= sizeof f) {
            memcpy(f, p, l); f[l] = '/'; memcpy(f + l + 1, name, nl + 1);
            if (!stat(f, &st) && S_ISREG(st.st_mode) && !access(f, X_OK)) return 1;
        }
        p += l + (e ? 1 : 0);
        if (!e) break;
    }
    return 0;
}

static const char *env_set(const char *k) { const char *v = getenv(k); return v && *v ? v : NULL; }

/* the display's variables from the user's systemd manager, while this process doesn't have them (a service started
 * before the session exported them): at the start and with each look for the tools, 10 times at most. Only these four,
 * only plain values, never one this process already has */
static void session_env(void) {
    static const char *const keys[] = { "WAYLAND_DISPLAY", "DISPLAY", "XAUTHORITY", "XDG_CURRENT_DESKTOP" };
    static int tries;                   /* a session that never has some of them (X11 alone): asked 10 times at most */
    int missing = !env_set("WAYLAND_DISPLAY") || !env_set("DISPLAY") || !env_set("XDG_CURRENT_DESKTOP");
    if (!missing || tries >= 10 || !in_path("systemctl")) return;
    tries++;
    static char out[65536];
    char *a[] = { "systemctl", "--user", "show-environment", NULL };
    long n = run(a, NULL, 0, (uint8_t *)out, sizeof out - 1);
    if (n <= 0) return;
    out[n] = 0;
    for (char *line = out, *nl; line && *line; line = nl) {
        nl = strchr(line, '\n');
        if (nl) *nl++ = 0;
        char *eq = strchr(line, '=');
        if (!eq || eq[1] == '$' || strpbrk(eq + 1, "'\"\\")) continue;
        *eq = 0;
        for (size_t i = 0; i < sizeof keys / sizeof *keys; i++)
            if (!strcmp(line, keys[i]) && !env_set(keys[i]) && eq[1]) setenv(keys[i], eq + 1, 1);
    }
}

/* looks for the tools; 1 when whether there is one changed (the hello has to be said again) */
static int pick_tools(void) {
    int had = tool_n > 0;
    session_env();
    int x = in_path("xclip"), w = in_path("wl-paste") && in_path("wl-copy");
    const char *desk = env_set("XDG_CURRENT_DESKTOP");
    int gnome = desk && strstr(desk, "GNOME"), wl = env_set("WAYLAND_DISPLAY") != NULL, disp = env_set("DISPLAY") != NULL;
    tool_n = 0;
    if (gnome && disp && x) tools[tool_n++] = T_XCLIP;      /* Mutter: Xwayland's CLIPBOARD, never wl-paste */
    else {
        if (w && wl) tools[tool_n++] = T_WL;
        if (x) tools[tool_n++] = T_XCLIP;
        if (w && !wl) tools[tool_n++] = T_WL;               /* WAYLAND_DISPLAY may come later (session_env) */
    }
    char said[sizeof tool_said];
    if (!tool_n) snprintf(said, sizeof said, "no clipboard tool: install xclip or wl-clipboard (the clipboard is off until one is there)");
    else snprintf(said, sizeof said, "clipboard through %s%s%s%s", tools[0] == T_XCLIP ? "xclip" : "wl-clipboard",
                  tools[0] == T_XCLIP ? (gnome && disp ? " (Xwayland, GNOME)" : " (X11)") : " (Wayland)",
                  tool_n > 1 ? ", then " : "", tool_n > 1 ? (tools[1] == T_XCLIP ? "xclip" : "wl-clipboard") : "");
    if (strcmp(said, tool_said)) { fprintf(stderr, "lordos-guest: %s\n", said); snprintf(tool_said, sizeof tool_said, "%s", said); }
    return had != (tool_n > 0);
}

/* a tool's answer: one that couldn't be started has the tools looked for again at the next turn */
static long tool_ran(long r) { if (r == -2) { tool_look_at = 0; return -1; } return r; }

/* one form of the clipboard: "text" (UTF-8), "text/html", "image/png" */
static long clip_read_type(const char *type, uint8_t *out, size_t cap) {
    int text = !strcmp(type, "text");
    long n = -1;
    for (int i = 0; i < tool_n && n < 0; i++) {
        char *a[] = { "wl-paste", "-n", "-t", text ? "text/plain;charset=utf-8" : (char *)type, NULL };
        char *x[] = { "xclip", "-o", "-selection", "clipboard", "-t", text ? "UTF8_STRING" : (char *)type, NULL };
        n = tool_ran(run(tools[i] == T_WL ? a : x, NULL, 0, out, cap));
    }
    return n;
}
static long clip_read(uint8_t *out, size_t cap) { return clip_read_type("text", out, cap); }

/* the types the clipboard offers, one per line (wl-paste -l, or xclip's TARGETS) */
static long clip_types(char *out, size_t cap) {
    long n = -1;
    for (int i = 0; i < tool_n && n < 0; i++) {
        char *a[] = { "wl-paste", "-l", NULL };
        char *x[] = { "xclip", "-o", "-selection", "clipboard", "-t", "TARGETS", NULL };
        n = tool_ran(run(tools[i] == T_WL ? a : x, NULL, 0, (uint8_t *)out, cap - 1));
    }
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
    for (int i = 0; i < tool_n; i++) {
        char *a[] = { "wl-copy", "--type", text ? "text/plain;charset=utf-8" : (char *)type, NULL };
        char *x[] = { "xclip", "-i", "-selection", "clipboard", "-t", text ? "UTF8_STRING" : (char *)type, NULL };
        if (tool_ran(run(tools[i] == T_WL ? a : x, p, n, NULL, 0)) >= 0) return 0;
    }
    return -1;
}
static int clip_write(const uint8_t *p, size_t n) { return clip_write_type("text", p, n); }

/* the whole frame, however the port takes it: a virtio-serial port writes at most 32 KiB per call (virtio_console), so
 * a frame is written in a loop and never left half sent */
static int send_frame(int fd, uint16_t kind, const uint8_t *p, uint32_t n) {
    uint8_t h[AG_HEAD];
    ag_frame_head(h, kind, 0, n);
    size_t total = AG_HEAD + (size_t)n, done = 0;
    while (done < total) {
        struct iovec io[2];
        int k = 0;
        if (done < AG_HEAD) { io[k++] = (struct iovec){ h + done, AG_HEAD - done }; if (n) io[k++] = (struct iovec){ (void *)p, n }; }
        else io[k++] = (struct iovec){ (void *)(p + (done - AG_HEAD)), total - done };
        ssize_t w = writev(fd, io, k);
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && errno == EAGAIN) { struct pollfd q = { fd, POLLOUT, 0 }; if (poll(&q, 1, 2000) <= 0) return -1; continue; }
        if (w <= 0) return -1;
        done += (size_t)w;
    }
    return 0;
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

/* the shutdown hooks that run `lordos-guest --stopping` (the package's, Install Agent's): one there and executable */
static const char *const STOP_HOOKS[] = { "/usr/lib/systemd/system-shutdown/lordos-guest",
                                          "/usr/lib/systemd/system-shutdown/lordos-guest-local", NULL };
static int stop_hook(void) {
    for (int i = 0; STOP_HOOKS[i]; i++) if (!access(STOP_HOOKS[i], X_OK)) return 1;
    return 0;
}

static void say_hello(int fd) {
    char h[160];
#ifndef LG_NO_PERF
    static const char perf_word[] = " perf";
#else
    static const char perf_word[] = "";
#endif
    /* "noclip": the helper is there but the guest has no clipboard tool (Machines says what to install) */
    snprintf(h, sizeof h, "lordos-guest " VERSION "%s%s%s%s%s shell%s", tool_n ? " clipboard clipx" : " noclip", tool_n && xc_fd() >= 0 ? " clipreq" : "",
             wfd >= 0 ? " windows" : "", stop_hook() ? " stopping" : "", perf_word, gshell_listen_fd() >= 0 ? " links notify send" : "");
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

/* --stopping: AGK_STOPPING, then lord-vm's echo for at most AG_STOP_WAIT_MS. Always 0 once the verb is one (the
 * guest's shutdown never waits on this beyond that), 2 when it isn't */
static int say_stopping(const char *port, const char *verb) {
    int mode = ag_stop_verb(verb);
    if (!mode) { fprintf(stderr, "lordos-guest: --stopping takes poweroff, halt, reboot or kexec\n"); return 2; }
    int fd = open_port(port);
    if (fd < 0) return 0;                       /* no agent port: not a Machines guest, or its agent is off */
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);     /* never stuck in a write: the shutdown goes on */
    uint8_t b = (uint8_t)mode, in[AG_HEAD + 64];
    size_t have = 0;
    if (send_frame(fd, AGK_STOPPING, &b, 1)) { close(fd); return 0; }
    long long end = now_ms() + AG_STOP_WAIT_MS;
    for (;;) {
        long long left = end - now_ms();
        struct pollfd p = { fd, POLLIN, 0 };
        if (left <= 0 || poll(&p, 1, (int)left) <= 0) break;
        ssize_t r = read(fd, in + have, sizeof in - have);
        if (r <= 0) { if (r < 0 && errno == EINTR) continue; break; }
        have += (size_t)r;
        int done = 0;
        for (;;) {
            AgFrame f;
            int used = ag_frame_next(in, have, &f);
            if (used < 0 || (!used && have == sizeof in)) { have = 0; break; }   /* a big frame of something else: dropped */
            if (!used) break;
            int m;
            if (f.kind == AGK_STOPPING && !ag_stopping_parse(f.data, f.len, &m) && m == mode) done = 1;
            memmove(in, in + used, have - (size_t)used);
            have -= (size_t)used;
        }
        if (done) break;
    }
    close(fd);
    return 0;
}

#ifndef LG_NO_PERF
/* the performance profile: applied or put back, and the outcome said to the host (fd < 0: only done) */
static void perf_do(int fd, int on) {
    unsigned done, failed;
    uint8_t b[AG_PERFSTATE_BYTES];
    perf_set(on, &done, &failed);
    if (fd >= 0 && !ag_perfstate_write(b, on, done, failed)) send_frame(fd, AGK_PERFSTATE, b, sizeof b);
}
#endif

/* SIGTERM (the session's end), SIGINT, SIGHUP: the profile is put back before the helper ends */
static volatile sig_atomic_t ending;
static void on_end(int sig) { (void)sig; ending = 1; }

int main(int argc, char **argv) {
    const char *port = PORT;
    char wpath[256] = "";
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (rt && rt[0] == '/') snprintf(wpath, sizeof wpath, "%s/lordos-guestwin.sock", rt);
    int once = 0;
    const char *stopping = NULL, *sys_bus = NULL, *ses_bus = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) port = argv[++i];
        else if (!strcmp(argv[i], "--windows") && i + 1 < argc) snprintf(wpath, sizeof wpath, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--once")) once = 1;
        else if (!strcmp(argv[i], "--stopping") && i + 1 < argc) stopping = argv[++i];
        else if (!strcmp(argv[i], "--system-bus") && i + 1 < argc) sys_bus = argv[++i];
        else if (!strcmp(argv[i], "--session-bus") && i + 1 < argc) ses_bus = argv[++i];
        else if (!strcmp(argv[i], "--version")) { printf("lordos-guest %s\n", VERSION); return 0; }
        else if (!strcmp(argv[i], "--open-on-host") && i + 1 < argc && i + 2 == argc) return gshell_cli(argv[i], argv[i + 1], NULL);
        else if (!strcmp(argv[i], "--notify-host") && i + 1 < argc && i + 3 >= argc) return gshell_cli(argv[i], argv[i + 1], i + 2 < argc ? argv[i + 2] : NULL);
        else if (!strcmp(argv[i], "--send-to-host") && i + 1 < argc) return gsend_cli(argc - i - 1, argv + i + 1);
        else { fprintf(stderr, "usage: lordos-guest [--port PATH] [--windows PATH] [--once] [--system-bus PATH] [--session-bus PATH] | [--port PATH] --stopping VERB | --open-on-host URL | --notify-host TITLE [BODY] | --send-to-host FILE...\n"); return 2; }
    }
    signal(SIGPIPE, SIG_IGN);
    if (stopping) return say_stopping(port, stopping);
    /* without SA_RESTART: a wait in poll or sleep ends at the signal */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_end;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
#ifndef LG_NO_PERF
    perf_buses(sys_bus, ses_bus);
#else
    (void)sys_bus; (void)ses_bus;
#endif
    win_in = malloc(WIN_BUF); win_tmp = malloc(WIN_BUF); app_list = calloc(1, sizeof *app_list); win_list = calloc(1, sizeof *win_list);
    if (!win_in || !win_tmp || !app_list || !win_list) return 1;
    long long win_try = 0;
    static uint8_t buf[AG_HEAD + AG_FRAME_MAX], clip[CLIP_MAX], html[AG_TEXT_MAX], png[AG_PNG_MAX], out[AG_FRAME_MAX];
    static char types[4096], last_types[4096];
    size_t have = 0;
    uint64_t last = 0;                  /* the clipboard's main form last sent or set, so it doesn't go back and forth */
    int fd = -1, host_clipx = 0, host_clipreq = 0;
    uint32_t offer_gen = 0;                 /* the host's offer we hold on the X CLIPBOARD, and how long a paste waits */
    long long offer_wait = AG_CLIP_WAIT_MS;
    long long asked_at[4] = { 0 };          /* a request to the host per form, waiting since then (0 none) */
    long long next_look = 0, png_look = 0;
    pick_tools();
#ifndef LG_NO_PERF
    perf_recover();                     /* an earlier run that ended badly left the profile on */
#endif
    xc_open();
    if (!once) gshell_listen();
    unsigned host_caps = 0;                 /* the host's hello: "links", "notify" while the machine takes them */
    tool_look_at = now_ms() + 30000;
    for (;;) {
        if (ending) {
            gshell_end();
#ifndef LG_NO_PERF
            if (perf_active()) perf_do(fd, 0);
#endif
            return 0;
        }
#ifndef LG_NO_PERF
        /* the port went (the host's lord-vm is gone): the profile lasts only while the host holds it */
        if (fd < 0 && perf_active()) perf_do(-1, 0);
#endif
        if (fd < 0 && gshell_fd() >= 0) gshell_end();             /* the host is gone: so is its shell */
        if (fd < 0) { gsend_port_lost(); gnotes_want(0, NULL); }
        if (fd < 0) {
            fd = open_port(port);
            if (fd < 0) { if (once) { perror(port); return 1; } sleep(3); continue; }
            say_hello(fd);
            have = 0;
            host_clipx = host_clipreq = 0;
            host_caps = 0;
        }
        gshell_expire();
        gnotes_want((host_caps & AG_HELLO_NOTIFY) != 0, ses_bus);
        int send_wait = gsend_pump(fd);
        long long now = now_ms();
        /* lord-server's endpoint: tried every 3 s while it isn't there; the hello says "windows" while it is */
        if (wfd < 0 && wpath[0] && now >= win_try) {
            win_try = now + 3000;
            wfd = unix_connect(wpath);
            if (wfd >= 0) { win_n = 0; say_hello(fd); }
        }
        /* the tools again every 30 s, or at once after one couldn't be started; the hello again when that changed */
        if (now >= tool_look_at) {
            tool_look_at = now + 30000;
            int had_x = xc_fd() >= 0;
            if (xc_fd() < 0) xc_open();
            if (pick_tools() || had_x != (xc_fd() >= 0)) say_hello(fd);
        }
        /* a program pastes the host's offer: that form asked of the host, once; no answer in time is a refusal */
        for (int f; xc_fd() >= 0 && (f = xc_wanted());) {
            uint8_t rq[5];
            ag_clipreq_write(rq, offer_gen, (uint8_t)f);
            if (!offer_gen || send_frame(fd, AGK_CLIPREQ, rq, 5)) xc_supply(f, NULL, 0);
            else asked_at[f] = now;
        }
        for (int f = AGF_TEXT; f <= AGF_PNG; f++)
            if (asked_at[f] && now - asked_at[f] > offer_wait) { asked_at[f] = 0; xc_supply(f, NULL, 0); fprintf(stderr, "lordos-guest: no answer from the host for a paste\n"); }
        xc_expire(now, offer_wait + 1000);
        if (now >= next_look && !tool_n) { next_look = now + 1000; if (once) return 0; }
        /* while the host's offer is ours, the guest's clipboard is the host's: not read (that would ask for it) */
        if (now >= next_look && xc_owned()) { next_look = now + 1000; if (once) return 0; }
        if (now >= next_look) {
            next_look = now + 1000;
            int sent = 0;
            long tl = clip_types(types, sizeof types);
            /* a password manager's secret (KeePassXC, KDE: x-kde-passwordManagerHint) never leaves the guest, so it can't
             * reach the host's clipboard history or outlive the manager's own clearing */
            if (tl > 0 && has_type(types, "x-kde-passwordManagerHint")) sent = 1;
            else if (host_clipx && tl > 0) {
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
        int xfd0 = xc_fd();
        struct pollfd pp[10] = { { fd, POLLIN, 0 }, { wfd, POLLIN, 0 }, { xfd0, POLLIN, 0 },
                                { gshell_fd(), (short)((gshell_can_read() ? POLLIN : 0) | (gshell_wants_out() ? POLLOUT : 0)), 0 },
                                { gshell_listen_fd(), POLLIN, 0 } };
        for (int i = 0; i < 4; i++) pp[5 + i] = (struct pollfd){ gshell_client_fd(i), POLLIN, 0 };
        pp[9] = (struct pollfd){ gnotes_fd(), POLLIN, 0 };
        long long until = next_look - now_ms();
        if (until > 250) until = 250;          /* the requests' time limits */
        if (send_wait >= 0 && send_wait < until) until = send_wait;      /* a file on its way */
        int r = poll(pp, 10, (int)(until > 0 ? until : 0));
        if (r < 0 && errno != EINTR) { close(fd); fd = -1; continue; }
        if (r <= 0) continue;
        if (pp[3].fd >= 0 && pp[3].revents) gshell_pump(fd, pp[3].revents);
        if (pp[4].fd >= 0 && (pp[4].revents & POLLIN)) gshell_accept();
        if (pp[9].fd >= 0 && pp[9].revents) gnotes_read(fd);
        for (int i = 0; i < 4; i++) if (pp[5 + i].fd >= 0 && pp[5 + i].revents) gshell_client(i, fd, host_caps);
        if (xfd0 >= 0 && (pp[2].revents & (POLLIN | POLLHUP | POLLERR)) && xc_read() < 0) say_hello(fd);
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
                host_clipreq = f.len < 200 && (ag_hello_caps(f.data, f.len) & AG_HELLO_CLIPREQ);
                host_caps = f.len < 200 ? ag_hello_caps(f.data, f.len) : 0;
                gsend_host(fd, (host_caps & AG_HELLO_SEND) != 0);
                /* a new lord-vm (after a resume) asks once: the hello again */
                if (f.len < 200 && memmem(f.data, f.len, " ask", 4)) say_hello(fd);
            }
            if (f.kind == LAG_APPS || f.kind == LAG_WINACT || f.kind == LAG_LAUNCH) to_server(&f);
            gshell_frame(fd, f.kind, f.data, f.len);
            gsend_frame(fd, f.kind, f.data, f.len);
#ifndef LG_NO_PERF
            int perf_on;
            /* the desktop's name (XDG_CURRENT_DESKTOP) may still be on its way from the user's manager */
            if (f.kind == AGK_PERF && !ag_perf_parse(f.data, f.len, &perf_on)) { session_env(); perf_do(fd, perf_on); }
#endif
            uint32_t g;
            uint8_t forms, flags, form, status;
            const uint8_t *d;
            uint32_t dn;
            if (f.kind == AGK_CLIPOFFER && host_clipreq && !ag_offer_parse(f.data, f.len, &g, &forms, &flags) && !xc_own(forms)) {
                offer_gen = g;
                offer_wait = flags & AGO_ASK ? AG_ASK_WAIT_MS : AG_CLIP_WAIT_MS;
                memset(asked_at, 0, sizeof asked_at);
                last = 0;
                snprintf(last_types, sizeof last_types, "%s", "");
            }
            if (f.kind == AGK_CLIPDATA && !ag_clipdata_parse(f.data, f.len, &g, &form, &status, &d, &dn) && g == offer_gen && asked_at[form]) {
                asked_at[form] = 0;
                xc_supply(form, status == AGD_OK ? d : NULL, status == AGD_OK ? dn : 0);
            }
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
