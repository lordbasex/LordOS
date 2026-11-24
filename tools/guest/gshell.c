/* gshell.c — lordos-guest's agent shell and its local socket for links and notifications (docs/VIRTUALIZATION.md 6.12,
 * 6.18; vmagent.h AGK_SHELL_*, AGK_LINK, AGK_NOTIFY). See gshell.h. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "gshell.h"
#include "gsend.h"
#include "../../server/src/vm/vmagent.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static int send_frame(int fd, uint16_t kind, const uint8_t *p, uint32_t n) {
    uint8_t h[AG_HEAD];
    ag_frame_head(h, kind, 0, n);
    struct iovec io[2] = { { h, AG_HEAD }, { (void *)p, n } };
    return writev(fd, io, n ? 2 : 1) == (ssize_t)(AG_HEAD + n) ? 0 : -1;
}

/* ---- the shell ---- */

static int sh_master = -1;
static pid_t sh_pid;
static uint32_t sh_id, sh_unacked;     /* the shell's id; what was sent to the host and not acknowledged yet */
static uint8_t sh_in[AG_SHELL_WINDOW]; /* what the host typed that the pty hasn't taken yet */
static size_t sh_in_n;

/* the signed-in person's own login shell: their /etc/passwd entry (read here: no NSS in a static program), only an
 * absolute path to an executable file, and listed in /etc/shells when that file exists; else /bin/sh */
int gshell_pick(uid_t uid, const char *passwd, const char *shells, char *out, size_t cap) {
    snprintf(out, cap, "/bin/sh");
    FILE *f = fopen(passwd ? passwd : "/etc/passwd", "re");
    if (!f) return 0;
    char line[1024], want[24], sh[512] = "";
    snprintf(want, sizeof want, "%u", (unsigned)uid);
    while (fgets(line, sizeof line, f)) {
        char *fld[7];
        int k = 0;
        char *p = line;
        line[strcspn(line, "\n")] = 0;
        while (k < 7) { fld[k++] = p; char *c = strchr(p, ':'); if (!c) break; *c = 0; p = c + 1; }
        if (k == 7 && !strcmp(fld[2], want)) { snprintf(sh, sizeof sh, "%s", fld[6]); break; }
    }
    fclose(f);
    struct stat st;
    if (sh[0] != '/' || strstr(sh, "/../") || stat(sh, &st) || !S_ISREG(st.st_mode) || access(sh, X_OK)) return 0;
    FILE *sf = fopen(shells ? shells : "/etc/shells", "re");
    if (sf) {
        int listed = 0;
        while (fgets(line, sizeof line, sf)) { line[strcspn(line, "\r\n")] = 0; if (!strcmp(line, sh)) { listed = 1; break; } }
        fclose(sf);
        if (!listed) return 0;
    }
    snprintf(out, cap, "%s", sh);
    return 1;
}

int gshell_fd(void) { return sh_master; }
int gshell_wants_out(void) { return sh_master >= 0 && sh_in_n > 0; }
int gshell_can_read(void) { return sh_master >= 0 && sh_unacked < AG_SHELL_WINDOW; }

static void sh_close_frame(int port, uint32_t id, int status) {
    uint8_t b[8];
    ag_shell_pair_write(b, id, (uint32_t)status);
    if (port >= 0) send_frame(port, AGK_SHELL_CLOSE, b, 8);
}

/* ends the shell (the host asked, or the port went): its process group gets SIGHUP, as a closed terminal does */
void gshell_end(void) {
    if (sh_master < 0) return;
    if (sh_pid > 0) { kill(-sh_pid, SIGHUP); kill(sh_pid, SIGHUP); }
    close(sh_master);
    sh_master = -1;
    if (sh_pid > 0) {
        for (int i = 0; i < 50 && waitpid(sh_pid, NULL, WNOHANG) == 0; i++) { struct timespec t = { 0, 10000000 }; nanosleep(&t, NULL); }
        if (waitpid(sh_pid, NULL, WNOHANG) == 0) { kill(-sh_pid, SIGKILL); waitpid(sh_pid, NULL, 0); }
    }
    sh_pid = 0;
    sh_id = 0;
    sh_unacked = 0;
    sh_in_n = 0;
}

static void sh_resize(int cols, int rows) {
    struct winsize ws = { (unsigned short)rows, (unsigned short)cols, 0, 0 };
    if (sh_master >= 0) ioctl(sh_master, TIOCSWINSZ, &ws);
}

static void sh_open(int port, uint32_t id, int cols, int rows) {
    gshell_end();
    /* never root's shell: the helper runs as the person signed in; a helper started as root opens none */
    if (!getuid() || !geteuid()) { sh_close_frame(port, id, AG_SHELL_NOSTART); fprintf(stderr, "lordos-guest: no shell as root\n"); return; }
    char shell[512];
    gshell_pick(getuid(), NULL, NULL, shell, sizeof shell);
    int m = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    char pts[128];
    if (m < 0 || grantpt(m) || unlockpt(m) || ptsname_r(m, pts, sizeof pts)) { if (m >= 0) close(m); sh_close_frame(port, id, AG_SHELL_NOSTART); return; }
    struct winsize ws = { (unsigned short)rows, (unsigned short)cols, 0, 0 };
    pid_t pid = fork();
    if (pid < 0) { close(m); sh_close_frame(port, id, AG_SHELL_NOSTART); return; }
    if (!pid) {
        setsid();
        int sl = open(pts, O_RDWR);
        if (sl < 0) _exit(127);
        ioctl(sl, TIOCSCTTY, 0);
        ioctl(sl, TIOCSWINSZ, &ws);
        dup2(sl, 0); dup2(sl, 1); dup2(sl, 2);
        for (int f = 3; f < 1024; f++) close(f);
        signal(SIGPIPE, SIG_DFL);
        signal(SIGTERM, SIG_DFL); signal(SIGINT, SIG_DFL); signal(SIGHUP, SIG_DFL);
        const char *home = getenv("HOME");
        if (!home || home[0] != '/' || chdir(home)) { if (chdir("/")) {} }
        setenv("TERM", "xterm-256color", 1);
        setenv("SHELL", shell, 1);
        setenv("LORDOS_AGENT_SHELL", "1", 1);
        const char *base = strrchr(shell, '/');
        char argv0[520];
        snprintf(argv0, sizeof argv0, "-%s", base ? base + 1 : shell);      /* a login shell, as a terminal starts it */
        char *argv[] = { argv0, NULL };
        execv(shell, argv);
        _exit(127);
    }
    fcntl(m, F_SETFL, fcntl(m, F_GETFL) | O_NONBLOCK);
    sh_master = m;
    sh_pid = pid;
    sh_id = id;
    sh_unacked = 0;
    sh_in_n = 0;
    fprintf(stderr, "lordos-guest: the host opened a shell (%s, %dx%d)\n", shell, cols, rows);
}

/* what the host typed, into the pty as far as it takes it now; acknowledged as it goes */
static void sh_feed(int port) {
    if (sh_master < 0 || !sh_in_n) return;
    ssize_t w = write(sh_master, sh_in, sh_in_n);
    if (w <= 0) return;
    memmove(sh_in, sh_in + w, sh_in_n - (size_t)w);
    sh_in_n -= (size_t)w;
    uint8_t b[8];
    ag_shell_pair_write(b, sh_id, (uint32_t)w);
    send_frame(port, AGK_SHELL_ACK, b, 8);
}

int gshell_frame(int port, uint16_t kind, const uint8_t *p, uint32_t n) {
    uint32_t id;
    if (kind == AGK_SHELL_OPEN) { int c, r; if (!ag_shell_size_parse(p, n, &id, &c, &r)) sh_open(port, id, c, r); return 1; }
    if (kind == AGK_SHELL_SIZE) { int c, r; if (!ag_shell_size_parse(p, n, &id, &c, &r) && id == sh_id) sh_resize(c, r); return 1; }
    if (kind == AGK_SHELL_CLOSE) { int st; if (!ag_shell_close_parse(p, n, &id, &st) && id == sh_id) gshell_end(); return 1; }
    if (kind == AGK_SHELL_ACK) {
        uint32_t by;
        if (!ag_shell_ack_parse(p, n, &id, &by) && id == sh_id) sh_unacked = by > sh_unacked ? 0 : sh_unacked - by;
        return 1;
    }
    if (kind == AGK_SHELL_DATA) {
        const uint8_t *d;
        uint32_t dn;
        if (ag_shell_data_parse(p, n, &id, &d, &dn) || id != sh_id || sh_master < 0) return 1;
        if (sh_in_n + dn > sizeof sh_in) return 1;          /* the host broke its window: dropped */
        memcpy(sh_in + sh_in_n, d, dn);
        sh_in_n += dn;
        sh_feed(port);
        return 1;
    }
    return 0;
}

void gshell_pump(int port, short ev) {
    if (sh_master < 0) return;
    if (ev & POLLOUT) sh_feed(port);
    if (!(ev & (POLLIN | POLLHUP | POLLERR))) return;
    if (sh_unacked < AG_SHELL_WINDOW) {
        static uint8_t buf[4 + AG_SHELL_CHUNK];
        size_t room = AG_SHELL_WINDOW - sh_unacked;
        if (room > AG_SHELL_CHUNK) room = AG_SHELL_CHUNK;
        ssize_t r = read(sh_master, buf + 4, room);
        if (r > 0) {
            { uint8_t h8[8]; ag_shell_pair_write(h8, sh_id, 0); memcpy(buf, h8, 4); }      /* the id only: the data follows */
            if (!send_frame(port, AGK_SHELL_DATA, buf, (uint32_t)(4 + r))) sh_unacked += (uint32_t)r;
            return;
        }
        if (r < 0 && (errno == EAGAIN || errno == EINTR)) return;
    } else if (!(ev & (POLLHUP | POLLERR))) return;
    /* EIO or EOF: every program on the terminal is gone; the shell's own status goes to the host */
    int st = 0, status = AG_SHELL_NOSTART;
    if (sh_pid > 0 && waitpid(sh_pid, &st, 0) == sh_pid) status = WIFEXITED(st) ? WEXITSTATUS(st) : 256 + WTERMSIG(st);
    if (status == 127) status = AG_SHELL_NOSTART;
    sh_pid = 0;
    uint32_t id = sh_id;
    gshell_end();
    sh_close_frame(port, id, status);
}

/* ---- the local socket: links and notifications from this person's programs ---- */

static int ls_fd = -1;
static char ls_path[256];
static struct { int fd; long long until; size_t n; uint8_t b[AG_HEAD + 4096 + AG_NOTE_BYTES + AG_LINK_MAX]; } cl[4];

static long long mono(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000; }

int gshell_sock_path(char *out, size_t cap) {
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (!rt || rt[0] != '/') return -1;
    return snprintf(out, cap, "%s/lordos-guest.sock", rt) < (int)cap ? 0 : -1;
}

/* $XDG_RUNTIME_DIR/lordos-guest.sock, 0600, this person's only (peers checked by SO_PEERCRED). A file of that name that
 * isn't our own socket is never replaced */
int gshell_listen(void) {
    if (ls_fd >= 0) return ls_fd;
    if (gshell_sock_path(ls_path, sizeof ls_path)) return -1;
    struct stat st;
    if (!lstat(ls_path, &st)) {
        if (!S_ISSOCK(st.st_mode) || st.st_uid != getuid()) return -1;
        unlink(ls_path);
    }
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    if (strlen(ls_path) >= sizeof a.sun_path) return -1;
    memcpy(a.sun_path, ls_path, strlen(ls_path) + 1);
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (s < 0) return -1;
    mode_t old = umask(0177);
    int r = bind(s, (struct sockaddr *)&a, sizeof a);
    umask(old);
    if (r || chmod(ls_path, 0600) || listen(s, 4)) { close(s); return -1; }
    for (int i = 0; i < 4; i++) cl[i].fd = -1;
    ls_fd = s;
    return s;
}

int gshell_listen_fd(void) { return ls_fd; }
int gshell_client_fd(int i) { return i >= 0 && i < 4 ? cl[i].fd : -1; }

void gshell_accept(void) {
    int c = accept4(ls_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (c < 0) return;
    struct ucred cr;
    socklen_t l = sizeof cr;
    if (getsockopt(c, SOL_SOCKET, SO_PEERCRED, &cr, &l) || cr.uid != getuid()) { close(c); return; }
    for (int i = 0; i < 4; i++) if (cl[i].fd < 0) { cl[i].fd = c; cl[i].n = 0; cl[i].until = mono() + 2000; return; }
    close(c);                                           /* busy: four at a time */
}

static void cl_done(int i, uint8_t answer) {
    if (cl[i].fd < 0) return;
    if (write(cl[i].fd, &answer, 1) < 0) {}
    close(cl[i].fd);
    cl[i].fd = -1;
}

/* one request a connection: one AGK_LINK or AGK_NOTIFY frame, parsed as the host will, then sent on to the host while it
 * said it takes them ("links", "notify" in its hello). The answer is one GSA_* byte */
void gshell_client(int i, int port, unsigned host_caps) {
    if (cl[i].fd < 0) return;
    ssize_t r = read(cl[i].fd, cl[i].b + cl[i].n, sizeof cl[i].b - cl[i].n);
    if (r <= 0) { if (r < 0 && (errno == EAGAIN || errno == EINTR)) return; cl_done(i, GSA_MALFORMED); return; }
    cl[i].n += (size_t)r;
    AgFrame f;
    int used = ag_frame_next(cl[i].b, cl[i].n, &f);
    if (used < 0 || (!used && cl[i].n == sizeof cl[i].b)) { cl_done(i, GSA_MALFORMED); return; }
    if (!used) return;
    if (f.kind == AGK_SEND_OFFER) {
        /* a file to send (tools 9): its absolute path, sent on only while the host takes files */
        char path[4096 + 1];
        if (!f.len || f.len > 4096 || memchr(f.data, 0, f.len)) { cl_done(i, GSA_MALFORMED); return; }
        memcpy(path, f.data, f.len);
        path[f.len] = 0;
        if (port < 0) { cl_done(i, GSA_NOHOST); return; }
        if (!(host_caps & AG_HELLO_SEND)) { cl_done(i, GSA_REFUSED); return; }
        int r = gsend_queue(path);
        cl_done(i, r == 0 ? GSA_SENT : r == 1 ? GSA_MALFORMED : GSA_FULL);
        return;
    }
    int link = f.kind == AGK_LINK;
    if ((!link && f.kind != AGK_NOTIFY) || (link ? !ag_link_ok(f.data, f.len) : ag_note_parse(f.data, f.len, NULL, NULL, NULL, NULL))) { cl_done(i, GSA_MALFORMED); return; }
    if (port < 0) { cl_done(i, GSA_NOHOST); return; }
    if (!(host_caps & (link ? AG_HELLO_LINKS : AG_HELLO_NOTIFY))) { cl_done(i, GSA_REFUSED); return; }
    cl_done(i, send_frame(port, f.kind, f.data, f.len) ? GSA_NOHOST : GSA_SENT);
}

void gshell_expire(void) {
    long long now = mono();
    for (int i = 0; i < 4; i++) if (cl[i].fd >= 0 && now > cl[i].until) cl_done(i, GSA_MALFORMED);
}

/* ---- the command line: lordos-guest --open-on-host URL, --notify-host TITLE [BODY] ---- */

/* text for a notification: at most max bytes, cut on a character boundary, control characters as spaces */
static size_t note_text(const char *in, char *out, size_t max) {
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && n < max; p++) {
        unsigned char c = *p;
        if (c < 0x20 || c == 0x7f) c = ' ';
        out[n++] = (char)c;
    }
    while (n && ((unsigned char)out[n - 1] & 0xc0) == 0x80) n--;       /* a cut character goes whole */
    if (n && ((unsigned char)out[n - 1] & 0xc0) == 0xc0) n--;
    out[n] = 0;
    return n;
}

int gshell_cli(const char *what, const char *a, const char *b) {
    uint8_t payload[AG_NOTE_BYTES + AG_LINK_MAX];
    size_t n = 0;
    uint16_t kind;
    if (!strcmp(what, "--open-on-host")) {
        char enc[AG_LINK_MAX * 3 + 1];
        long l = ag_link_encode((const uint8_t *)a, strlen(a), enc, sizeof enc);
        if (l <= 0 || !ag_link_ok((const uint8_t *)enc, (size_t)l)) { fprintf(stderr, "lordos-guest: only http and https links can open on LordOS\n"); return 2; }
        memcpy(payload, enc, (size_t)l);
        n = (size_t)l;
        kind = AGK_LINK;
    } else {
        char t[AG_NOTE_TITLE_MAX + 1], bo[AG_NOTE_BODY_MAX + 1];
        size_t tn = note_text(a, t, AG_NOTE_TITLE_MAX), bn = note_text(b ? b : "", bo, AG_NOTE_BODY_MAX);
        n = ag_note_write((const uint8_t *)t, tn, (const uint8_t *)bo, bn, payload, sizeof payload);
        if (!n) { fprintf(stderr, "lordos-guest: a notification needs a title of plain text\n"); return 2; }
        kind = AGK_NOTIFY;
    }
    char path[256];
    if (gshell_sock_path(path, sizeof path)) { fprintf(stderr, "lordos-guest: no XDG_RUNTIME_DIR\n"); return 1; }
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    if (strlen(path) >= sizeof sa.sun_path) return 1;
    memcpy(sa.sun_path, path, strlen(path) + 1);
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0 || connect(s, (struct sockaddr *)&sa, sizeof sa)) { fprintf(stderr, "lordos-guest: the LordOS tools aren't running in this session\n"); if (s >= 0) close(s); return 1; }
    if (send_frame(s, kind, payload, (uint32_t)n)) { close(s); return 1; }
    uint8_t ans = GSA_NOHOST;
    struct pollfd p = { s, POLLIN, 0 };
    if (poll(&p, 1, 3000) <= 0 || read(s, &ans, 1) != 1) ans = GSA_NOHOST;
    close(s);
    switch (ans) {
    case GSA_SENT: return 0;
    case GSA_REFUSED: fprintf(stderr, "lordos-guest: this machine doesn't take %s (Machines: Configure, Sharing)\n", kind == AGK_LINK ? "links" : "notifications"); return 3;
    case GSA_MALFORMED: fprintf(stderr, "lordos-guest: refused as malformed\n"); return 2;
    default: fprintf(stderr, "lordos-guest: not connected to LordOS\n"); return 1;
    }
}
