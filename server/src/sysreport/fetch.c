/* fetch.c — `axia-fetch` (see fetch.h). The mark is pixel art drawn two pixels per character
 * cell with half blocks (▀ ▄), in the violet of the app icon (tools/gen-brand.py); without color it is plain
 * ASCII. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "fetch.h"
#include "../hw/jbuf.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <spawn.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

/* Installed RPM packages under <root>: one line per package from `rpm -qa`, run without a shell.
 * The RPM database is SQLite, so the count comes from rpm itself. -1 if rpm is missing or slow. */
static int rpm_packages(const char *root) {
    int fd[2];
    if (pipe2(fd, O_CLOEXEC)) return -1;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fd[1], 1);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    char *argv[] = { "rpm", "--root", (char *)(*root ? root : "/"), "-qa", "--qf", "\n", NULL };
    pid_t pid;
    int r = posix_spawnp(&pid, "rpm", &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fd[1]);
    if (r) { close(fd[0]); return -1; }
    int n = 0, timed_out = 0;
    char buf[4096];
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        int left = 5000 - (int)((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000);
        struct pollfd p = { fd[0], POLLIN, 0 };
        if (left <= 0 || poll(&p, 1, left) <= 0) { timed_out = 1; break; }
        ssize_t k = read(fd[0], buf, sizeof buf);
        if (k <= 0) break;
        for (ssize_t i = 0; i < k; i++) n += buf[i] == '\n';
    }
    close(fd[0]);
    if (timed_out) kill(pid, SIGKILL);
    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    return !timed_out && WIFEXITED(st) && WEXITSTATUS(st) == 0 ? n : -1;
}

/* ---- the mark ---- */

/* 31 × 22 pixels: the Axia mark, an "A" whose crossbar runs past both legs, in the icon's violet from
 * light (P) at the apex to deep (B) at the feet; '.' nothing. */
static const char *MARK[] = {
    "..............PPP..............",
    ".............PPPPP.............",
    ".............PPPPP.............",
    "............PPPPPPP............",
    "............PPP.PPP............",
    "............PPP.PPP............",
    "...........PPP...PPP...........",
    "...........VVV...VVV...........",
    "..........VVV.....VVV..........",
    "..........VVV.....VVV..........",
    ".........VVV.......VVV.........",
    ".........VVV.......VVV.........",
    "........VVV.........VVV........",
    "..VVVVVVVVVVVVVVVVVVVVVVVVVVV..",
    "..VVVVVVVVVVVVVVVVVVVVVVVVVVV..",
    ".......BBB...........BBB.......",
    "......BBB.............BBB......",
    "......BBB.............BBB......",
    ".....BBBB.............BBBB.....",
    ".....BBB...............BBB.....",
    "....BBBB...............BBBB....",
    "....BBB.................BBB....",
};
#define MARK_H ((int)(sizeof MARK / sizeof MARK[0]))
#define MARK_W 31

static const char *PLAIN[] = {
    "               /\\              ",
    "              /  \\             ",
    "             /    \\            ",
    "            /      \\           ",
    "           /        \\          ",
    "    ======/==========\\======   ",
    "         /            \\        ",
    "        /              \\       ",
    "       /                \\      ",
};

typedef struct { char key; unsigned char r, g, b, idx; } Pal;
static const Pal PAL[] = {
    { 'P', 0xb4, 0x6c, 0xff, 135 }, { 'V', 0x7c, 0x5c, 0xff, 99 }, { 'B', 0x4f, 0x46, 0xe5, 62 },
};

static const Pal *pal(char k) {
    for (size_t i = 0; i < sizeof PAL / sizeof PAL[0]; i++) if (PAL[i].key == k) return &PAL[i];
    return NULL;
}

static void fg(HBuf *b, const Pal *p, FetchColor c) {
    if (c == FETCH_TRUECOLOR) hb_printf(b, "\033[38;2;%d;%d;%dm", p->r, p->g, p->b);
    else hb_printf(b, "\033[38;5;%dm", p->idx);
}
static void bg(HBuf *b, const Pal *p, FetchColor c) {
    if (c == FETCH_TRUECOLOR) hb_printf(b, "\033[48;2;%d;%d;%dm", p->r, p->g, p->b);
    else hb_printf(b, "\033[48;5;%dm", p->idx);
}

/* One text row of the mark: pixel rows 2y and 2y+1. */
static void mark_row(HBuf *b, int y, FetchColor c) {
    if (c == FETCH_PLAIN) { hb_printf(b, "%-*s", MARK_W, y < (int)(sizeof PLAIN / sizeof PLAIN[0]) ? PLAIN[y] : ""); return; }
    for (int x = 0; x < MARK_W; x++) {
        const Pal *t = 2 * y < MARK_H ? pal(MARK[2 * y][x]) : NULL;
        const Pal *u = 2 * y + 1 < MARK_H ? pal(MARK[2 * y + 1][x]) : NULL;
        if (!t && !u) { hb_printf(b, " "); continue; }
        if (t && u && t == u) { fg(b, t, c); hb_printf(b, "█"); }
        else if (t && u) { fg(b, t, c); bg(b, u, c); hb_printf(b, "▀"); }
        else if (t) { fg(b, t, c); hb_printf(b, "▀"); }
        else { fg(b, u, c); hb_printf(b, "▄"); }
        hb_printf(b, "\033[0m");
    }
}

/* ---- live facts ---- */

static char *slurp(const char *path, size_t max) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    char *p = malloc(max + 1);
    size_t got = 0;
    while (p && got < max) {
        ssize_t n = read(fd, p + got, max - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);
    if (p) p[got] = 0;
    return p;
}

static unsigned long long meminfo_kb(const char *txt, const char *key) {
    const char *p = txt ? strstr(txt, key) : NULL;
    return p ? strtoull(p + strlen(key) + 1, NULL, 10) : 0;
}

/* printable, no spaces: a user or host name for the screen */
static void clean(char *s) {
    for (; *s; s++) if (*s <= ' ' || *s >= 0x7f) *s = '?';
}

void fetch_live(FetchLive *l, const char *root, int share) {
    memset(l, 0, sizeof *l);
    char p[1024];
    if (!share) {
        struct passwd *pw = getpwuid(getuid());
        if (pw && pw->pw_name) snprintf(l->user, sizeof l->user, "%.63s", pw->pw_name);
        snprintf(p, sizeof p, "%s/proc/sys/kernel/hostname", root);
        char *h = slurp(p, 64);
        if (h) { h[strcspn(h, "\n")] = 0; snprintf(l->host, sizeof l->host, "%.63s", h); free(h); }
        clean(l->user);
        clean(l->host);
    }
    snprintf(p, sizeof p, "%s/proc/uptime", root);
    char *t = slurp(p, 128);
    l->uptime_s = t ? atol(t) : -1;
    free(t);
    snprintf(p, sizeof p, "%s/proc/meminfo", root);
    t = slurp(p, 16384);
    unsigned long long total = meminfo_kb(t, "MemTotal:"), avail = meminfo_kb(t, "MemAvailable:");
    l->mem_used_kb = total > avail ? total - avail : 0;
    free(t);
    struct statvfs vs;
    snprintf(p, sizeof p, "%s/", root);
    if (!statvfs(p, &vs)) {
        l->disk_total = (unsigned long long)vs.f_blocks * vs.f_frsize;
        l->disk_used = l->disk_total - (unsigned long long)vs.f_bfree * vs.f_frsize;
    }
    l->packages = rpm_packages(root);
    const char *sh = getenv("SHELL");
    if (sh && *sh) {
        const char *s = strrchr(sh, '/');
        snprintf(l->shell, sizeof l->shell, "%.31s", s ? s + 1 : sh);
        clean(l->shell);
    }
    snprintf(l->session, sizeof l->session, "%s", getenv("WAYLAND_DISPLAY") ? "Wayland" : getenv("DISPLAY") ? "X11" : "");
}

FetchColor fetch_color_mode(const char *opt, int is_tty) {
    const char *ct = getenv("COLORTERM");
    FetchColor full = ct && (strstr(ct, "truecolor") || strstr(ct, "24bit")) ? FETCH_TRUECOLOR : FETCH_256;
    if (opt && !strcmp(opt, "never")) return FETCH_PLAIN;
    if (opt && !strcmp(opt, "always")) return full;
    const char *term = getenv("TERM");
    if (!is_tty || getenv("NO_COLOR") || (term && !strcmp(term, "dumb"))) return FETCH_PLAIN;
    return full;
}

/* ---- the summary ---- */

#define LINES_MAX 24
typedef struct { char label[16]; char value[256]; } Line;

static void bytes_text(unsigned long long v, char *out, size_t n) {
    if (v >= 1000ULL * 1000 * 1000 * 1000) snprintf(out, n, "%.1f TB", v / 1e12);
    else if (v >= 10ULL * 1000 * 1000 * 1000) snprintf(out, n, "%.0f GB", v / 1e9);
    else snprintf(out, n, "%.1f GB", v / 1e9);
}

static int add(Line *ls, int n, const char *label, const char *value) {
    if (n >= LINES_MAX || !value || !value[0]) return n;
    snprintf(ls[n].label, sizeof ls[n].label, "%s", label);
    snprintf(ls[n].value, sizeof ls[n].value, "%s", value);
    return n + 1;
}

char *fetch_render(const SysReport *r, const FetchLive *l, FetchColor c) {
    Line ls[LINES_MAX];
    int n = 0;
    char v[256], a[96], b2[96];

    snprintf(v, sizeof v, "Axia DE %s%s%s%s", r->sw.lordos, r->sw.os[0] ? " (" : "", r->sw.os, r->sw.os[0] ? ")" : "");
    n = add(ls, n, "OS", v);
    const SrMachine *m = &r->machine;
    snprintf(v, sizeof v, "%s%s%s", m->vendor, m->vendor[0] && m->product[0] ? " " : "", m->product);
    if (!v[0]) snprintf(v, sizeof v, "%s", m->model);
    if (m->virt[0]) { size_t o = strlen(v); snprintf(v + o, sizeof v - o, "%svirtual machine (%s)", o ? " · " : "", m->virt); }
    n = add(ls, n, "Computer", v);
    n = add(ls, n, "Kernel", r->sw.kernel);
    if (l->uptime_s >= 0) {
        long d = l->uptime_s / 86400, h = l->uptime_s % 86400 / 3600, mi = l->uptime_s % 3600 / 60;
        if (d) snprintf(v, sizeof v, "%ld d %ld h %ld min", d, h, mi);
        else if (h) snprintf(v, sizeof v, "%ld h %ld min", h, mi);
        else snprintf(v, sizeof v, "%ld min", mi);
        n = add(ls, n, "Uptime", v);
    }
    if (l->packages > 0) { snprintf(v, sizeof v, "%d (rpm)", l->packages); n = add(ls, n, "Packages", v); }
    snprintf(v, sizeof v, "%s%s%s", l->shell, l->session[0] ? " · " : "", l->session[0] ? l->session : "");
    n = add(ls, n, "Shell", v);
    for (int i = 0; i < r->ndisplay; i++) {
        const SrDisplay *x = &r->display[i];
        if (x->pref_w) snprintf(a, sizeof a, "%d × %d", x->pref_w, x->pref_h); else snprintf(a, sizeof a, "%s", x->mode);
        snprintf(v, sizeof v, "%s%s%s", x->name[0] ? x->name : x->connector, a[0] ? ", " : "", a);
        if (x->diag_tenths) { size_t o = strlen(v); snprintf(v + o, sizeof v - o, ", %d\"", (x->diag_tenths + 5) / 10); }
        n = add(ls, n, "Display", v);
    }
    const SrCpu *cpu = &r->cpu;
    snprintf(v, sizeof v, "%s (%d)", cpu->model[0] ? cpu->model : cpu->arch, cpu->threads);
    if (cpu->max_mhz) { size_t o = strlen(v); snprintf(v + o, sizeof v - o, " @ %.2f GHz", cpu->max_mhz / 1000.0); }
    n = add(ls, n, "CPU", v);
    for (int i = 0; i < r->ngpu; i++) {
        const SrGpu *g = &r->gpu[i];
        if (!g->connectors && !g->render_node) continue;
        snprintf(a, sizeof a, "%s", g->device[0] ? g->device : g->vendor[0] ? g->vendor : g->card);
        snprintf(v, sizeof v, "%s%s%s%s", a, g->driver[0] ? " (" : "", g->driver, g->driver[0] ? ")" : "");
        n = add(ls, n, "GPU", v);
    }
    if (r->mem.total_kb) {
        bytes_text(l->mem_used_kb * 1024ULL, a, sizeof a);
        bytes_text(r->mem.total_kb * 1024ULL, b2, sizeof b2);
        snprintf(v, sizeof v, "%s / %s (%d %%)", a, b2, (int)(l->mem_used_kb * 100 / r->mem.total_kb));
        n = add(ls, n, "Memory", v);
    }
    if (l->disk_total) {
        bytes_text(l->disk_used, a, sizeof a);
        bytes_text(l->disk_total, b2, sizeof b2);
        snprintf(v, sizeof v, "%s / %s (%d %%)", a, b2, (int)(l->disk_used * 100 / l->disk_total));
        n = add(ls, n, "Disk (/)", v);
    }
    /* what keeps apps in their place (docs/PRIVACY.md, docs/FIREWALL.md) */
    v[0] = 0;
    if (r->sw.landlock_abi > 0) snprintf(v, sizeof v, "Landlock %d", r->sw.landlock_abi);
    if (r->sw.netperm == 1) { size_t o = strlen(v); snprintf(v + o, sizeof v - o, "%sapp firewall", o ? " · " : ""); }
    if (r->sw.cgroup2) { size_t o = strlen(v); snprintf(v + o, sizeof v - o, "%scgroup v2", o ? " · " : ""); }
    n = add(ls, n, "Isolation", v);

    HBuf b = { 0 };
    const Pal *violet = pal('V');
    /* the title: user@host (or the product) and a rule under it */
    char title[160];
    if (l->user[0] && l->host[0]) snprintf(title, sizeof title, "%s@%s", l->user, l->host);
    else snprintf(title, sizeof title, "Axia DE");
    /* title, rule, the lines, and with color a blank line and the palette; at least the mark's height */
    int rows = n + (c == FETCH_PLAIN ? 2 : 4);
    if (rows < (MARK_H + 1) / 2) rows = (MARK_H + 1) / 2;
    if (rows < (int)(sizeof PLAIN / sizeof PLAIN[0])) rows = (int)(sizeof PLAIN / sizeof PLAIN[0]);
    for (int y = 0; y < rows; y++) {
        hb_printf(&b, " ");
        mark_row(&b, y, c);
        hb_printf(&b, "   ");
        if (y == 0) {
            if (c != FETCH_PLAIN) { hb_printf(&b, "\033[1m"); fg(&b, violet, c); }
            hb_printf(&b, "%s", title);
            if (c != FETCH_PLAIN) hb_printf(&b, "\033[0m");
        } else if (y == 1) {
            for (size_t i = 0; i < strlen(title); i++) hb_printf(&b, "%s", c == FETCH_PLAIN ? "-" : "─");
        } else if (y - 2 < n) {
            const Line *x = &ls[y - 2];
            if (c != FETCH_PLAIN) { hb_printf(&b, "\033[1m"); fg(&b, violet, c); }
            hb_printf(&b, "%s", x->label);
            if (c != FETCH_PLAIN) hb_printf(&b, "\033[0m");
            hb_printf(&b, "%*s%s", 10 - (int)strlen(x->label), "", x->value);
        } else if (y - 2 == n + 1 && c != FETCH_PLAIN) {
            static const char keys[] = "PVB";
            for (const char *k = keys; *k; k++) { bg(&b, pal(*k), c); hb_printf(&b, "   "); }
            hb_printf(&b, "\033[0m");
        }
        hb_printf(&b, "\n");
    }
    return hb_take(&b);
}
