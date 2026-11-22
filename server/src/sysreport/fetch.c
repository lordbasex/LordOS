/* fetch.c — `lordos-fetch` (see fetch.h). The crown is pixel art drawn two pixels per character
 * cell with half blocks (▀ ▄), in the brand colors (docs/LORDOS-BRAND.md); without color it is plain
 * ASCII. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "fetch.h"
#include "../hw/jbuf.h"
#include "../meminfo.h"
#include "../sec/json.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <limits.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

/* ---- the crown ---- */

/* 31 × 23 pixels: H gem highlight, Y gem, A gem shade, W white, L lavender shading, V band, B band
 * edge, '.' nothing. */
static const char *CROWN[] = {
    "...............H...............",
    "..............HHH..............",
    ".............HHYYY.............",
    "..............AAA..............",
    "...............A...............",
    "...H...........W...........H...",
    "..HHH.........WWL.........HHH..",
    ".HHYYY.......WWWLL.......HHYYY.",
    "..AAA........WWWLL........AAA..",
    "...A........WWWWLLL........A...",
    "...W.......WWWWWLLLW.......L...",
    "...WWW.....WWWWWLLLW.....LLL...",
    "...WWWW...WWWWWWLLLWW...LLLL...",
    "...WWWWW..WWWWWWLLLWW..LLLLL...",
    "...WWWWWW.WWWWWWWWWWW.WLLLLL...",
    "...WWWWWWWWWWWWWWWWWWWWLLLLL...",
    "...WWWWWWWWWWWWWWWWWWWWLLLLL...",
    "..BVVVVVVVVVVVVVVVVVVVVVVVVVB..",
    "..BVVVVVVVVVVVVVVVVVVVVVVVVVB..",
    "..WWWWWWWWWWWWWWWWWWWWWWLLLLL..",
    "..WWWWWWWWWWWWWWWWWWWWWWLLLLL..",
    "...WWWWWWWWWWWWWWWWWWWWWLLLL...",
    "...............................",
};
#define CROWN_H ((int)(sizeof CROWN / sizeof CROWN[0]))
#define CROWN_W 31

static const char *PLAIN[] = {
    "               <>              ",
    "    <>        /  \\        <>   ",
    "    /\\       /    \\       /\\   ",
    "    | \\     /      \\     / |   ",
    "    |  \\   /        \\   /  |   ",
    "    |   \\_/          \\_/   |   ",
    "    |                      |   ",
    "    |======================|   ",
    "    |______________________|   ",
};

typedef struct { char key; unsigned char r, g, b, idx; } Pal;
static const Pal PAL[] = {
    { 'H', 0xff, 0xf1, 0xa8, 229 }, { 'Y', 0xff, 0xd6, 0x0a, 220 }, { 'A', 0xff, 0xb0, 0x00, 214 },
    { 'W', 0xff, 0xff, 0xff, 231 }, { 'L', 0xbd, 0xb1, 0xf5, 147 }, { 'V', 0x7c, 0x5c, 0xff, 99 },
    { 'B', 0x5b, 0x3f, 0xe0, 62 },
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

/* One text row of the crown: pixel rows 2y and 2y+1. */
static void crown_row(HBuf *b, int y, FetchColor c) {
    if (c == FETCH_PLAIN) { hb_printf(b, "%-*s", CROWN_W, y < (int)(sizeof PLAIN / sizeof PLAIN[0]) ? PLAIN[y] : ""); return; }
    for (int x = 0; x < CROWN_W; x++) {
        const Pal *t = 2 * y < CROWN_H ? pal(CROWN[2 * y][x]) : NULL;
        const Pal *u = 2 * y + 1 < CROWN_H ? pal(CROWN[2 * y + 1][x]) : NULL;
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

/* printable ASCII, spaces allowed: a name from a file for the screen */
static void clean_text(char *s) {
    for (; *s; s++) if (*s < ' ' || *s >= 0x7f) *s = '?';
}

/* ---- parsing helpers ---- */

/* the number after "key:" at the start of a line */
static int line_num(const char *txt, const char *key, unsigned long long *v) {
    size_t kl = strlen(key);
    for (const char *p = txt; p && *p; p = strchr(p, '\n'), p = p ? p + 1 : NULL)
        if (!strncmp(p, key, kl) && p[kl] == ':') { *v = strtoull(p + kl + 1, NULL, 10); return 0; }
    return -1;
}

int fetch_swap(const char *meminfo, unsigned long long *total_kb, unsigned long long *used_kb) {
    unsigned long long t = 0, f = 0;
    if (!meminfo || line_num(meminfo, "SwapTotal", &t)) return -1;
    if (line_num(meminfo, "SwapFree", &f) || f > t) f = t;
    *total_kb = t;
    *used_kb = t - f;
    return 0;
}

static int safe_id(const char *s) {
    if (!s || !*s || strlen(s) > 64) return 0;
    for (; *s; s++) if (!isalnum((unsigned char)*s) && *s != '-' && *s != '_') return 0;
    return 1;
}

int fetch_settings(const char *json, char *theme, size_t tn, char *lang, size_t ln) {
    snprintf(theme, tn, "default");
    lang[0] = 0;
    JDoc d;
    char err[64];
    if (!json || json_parse(json, strlen(json), &d, err, sizeof err)) return -1;
    const char *t = json_str(d.root, "theme"), *g = json_str(d.root, "language");
    if (safe_id(t)) snprintf(theme, tn, "%s", t);
    if (g && strlen(g) < 24) { snprintf(lang, ln, "%s", g); clean(lang); }
    json_free(&d);
    return 0;
}

int fetch_theme_name(const char *json, char *out, size_t n) {
    JDoc d;
    char err[64];
    out[0] = 0;
    if (!json || json_parse(json, strlen(json), &d, err, sizeof err)) return -1;
    const char *s = json_str(d.root, "name");
    if (s && *s) { snprintf(out, n, "%.60s", s); clean_text(out); }
    json_free(&d);
    return out[0] ? 0 : -1;
}

int fetch_dpkg_version(const char *status, const char *pkg, char *out, size_t n) {
    out[0] = 0;
    size_t pl = strlen(pkg);
    for (const char *p = status; p && *p; ) {
        /* one stanza: from here to the next blank line */
        const char *end = strstr(p, "\n\n");
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (!strncmp(p, "Package: ", 9) && !strncmp(p + 9, pkg, pl) && p[9 + pl] == '\n') {
            const char *st = strstr(p, "\nStatus: "), *v = strstr(p, "\nVersion: ");
            if (st && st < p + len && !strncmp(st + 9, "install ok installed", 20) && v && v < p + len) {
                v += 10;
                size_t vl = strcspn(v, "\n");
                const char *colon = memchr(v, ':', vl);
                if (colon) { vl -= (size_t)(colon + 1 - v); v = colon + 1; }
                const char *dash = NULL;
                for (size_t i = 0; i < vl; i++) if (v[i] == '-') dash = v + i;
                if (dash) vl = (size_t)(dash - v);
                if (vl && vl < n && vl < 32) { memcpy(out, v, vl); out[vl] = 0; clean(out); return 0; }
            }
            return -1;
        }
        p = end ? end + 2 : NULL;
    }
    return -1;
}

int fetch_route_iface(const char *route, char *out, size_t n) {
    out[0] = 0;
    long best = -1;
    const char *p = route ? strchr(route, '\n') : NULL;          /* skip the header */
    for (; p && *++p; p = strchr(p, '\n')) {
        char ifn[IF_NAMESIZE + 1], dst[16], gw[16];
        unsigned flags;
        long metric;
        if (sscanf(p, "%16s %15s %15s %x %*d %*d %ld", ifn, dst, gw, &flags, &metric) != 5) continue;
        if (strcmp(dst, "00000000") || !(flags & 1)) continue;   /* the default route, up (RTF_UP) */
        if (best < 0 || metric < best) { best = metric; snprintf(out, n, "%s", ifn); clean(out); }
    }
    return out[0] ? 0 : -1;
}

int fetch_inet6(const char *if_inet6, const char *iface, char *out, size_t n) {
    out[0] = 0;
    for (const char *p = if_inet6; p && *p; p = strchr(p, '\n'), p = p ? p + 1 : NULL) {
        char hex[33], name[IF_NAMESIZE + 1];
        unsigned idx, plen, scope, flags;
        if (sscanf(p, "%32s %x %x %x %x %16s", hex, &idx, &plen, &scope, &flags, name) != 6) continue;
        if (strcmp(name, iface) || scope != 0 || strlen(hex) != 32) continue;   /* global scope only */
        if (flags & 0x40) continue;                                              /* tentative (IFA_F_TENTATIVE) */
        unsigned char a[16];
        for (int i = 0; i < 16; i++) { unsigned b; if (sscanf(hex + 2 * i, "%2x", &b) != 1) return -1; a[i] = (unsigned char)b; }
        char txt[INET6_ADDRSTRLEN];
        if (!inet_ntop(AF_INET6, a, txt, sizeof txt)) continue;
        snprintf(out, n, "%s/%u", txt, plen);
        return 0;
    }
    return -1;
}

int fetch_boot_time(const char *jsonl, const char *boot_id, double *secs, char *shown, size_t sn) {
    *secs = 0;
    shown[0] = 0;
    if (!jsonl || !boot_id || !*boot_id || strspn(boot_id, "0123456789abcdef-") != strlen(boot_id)) return -1;
    char key[80];
    snprintf(key, sizeof key, "{\"bootId\":\"%.40s\",", boot_id);
    const char *line = NULL;
    for (const char *q = jsonl; (q = strstr(q, key)); q++) if (q == jsonl || q[-1] == '\n') line = q;   /* the last one */
    if (!line) return -1;
    const char *eol = strchr(line, '\n');
    const char *sec = strstr(line, "\"seconds\":"), *sh = strstr(line, "\"shown\":\"");
    if (!sec || (eol && sec > eol)) return -1;
    double v = atof(sec + 10);
    if (!(v > 0 && v < 3600)) return -1;
    *secs = v;
    if (sh && (!eol || sh < eol)) {
        sh += 9;
        size_t k = strspn(sh, "abcdefghijklmnopqrstuvwxyz");
        if (k < sn) { memcpy(shown, sh, k); shown[k] = 0; }
    }
    return 0;
}

unsigned long long fetch_status_rss(const char *status) {
    unsigned long long v = 0;
    return status && !line_num(status, "VmRSS", &v) ? v : 0;
}

void fetch_terminal_pick(const char *term_program, const char *term_version, int ssh, const char *tty,
                         const char *const *ancestors, int nanc, char *out, size_t n) {
    out[0] = 0;
    if (term_program && !strcmp(term_program, "LordTerminal")) snprintf(out, n, "LordOS Terminal");
    else if (term_program && *term_program)
        snprintf(out, n, "%.40s%s%.16s", term_program, term_version && *term_version ? " " : "", term_version ? term_version : "");
    else if (ssh) snprintf(out, n, "SSH");
    else if (tty && !strncmp(tty, "/dev/tty", 8) && isdigit((unsigned char)tty[8])) snprintf(out, n, "Linux console");
    else {
        static const char *const skip[] = { "sh", "bash", "zsh", "fish", "dash", "ksh", "mksh", "tcsh", "csh", "nu", "elvish",
                                            "sudo", "su", "doas", "run0", "login", "env", "script", "timeout", "nohup", "strace",
                                            "watch", "time", "xargs", "make", "lordos-fetch", "lordos-sysreport", "fastfetch",
                                            "neofetch", NULL };
        for (int i = 0; i < nanc && !out[0]; i++) {
            const char *a = ancestors[i];
            int s = !a || !*a;
            for (int k = 0; !s && skip[k]; k++) if (!strcmp(a, skip[k])) s = 1;
            if (!s) snprintf(out, n, "%.40s", a);
        }
    }
    clean_text(out);
}

/* ---- live facts from this machine ---- */

static void read_line(const char *path, char *out, size_t n) {
    char *t = slurp(path, n ? n - 1 : 0);
    out[0] = 0;
    if (t) { t[strcspn(t, "\n")] = 0; snprintf(out, n, "%s", t); free(t); }
}

/* The names of this process's ancestors (nearest first) from /proc/<pid>/stat. */
static int ancestors(char names[][32], int max) {
    int k = 0;
    pid_t pid = getppid();
    while (pid > 1 && k < max) {
        char path[64], *t;
        snprintf(path, sizeof path, "/proc/%d/stat", (int)pid);
        if (!(t = slurp(path, 1024))) break;
        char *l = strchr(t, '('), *r = strrchr(t, ')');
        if (!l || !r || r < l) { free(t); break; }
        size_t len = (size_t)(r - l - 1);
        if (len > 31) len = 31;
        memcpy(names[k], l + 1, len);
        names[k][len] = 0;
        k++;
        int ppid = 0;
        if (sscanf(r + 1, " %*c %d", &ppid) != 1) { free(t); break; }
        free(t);
        pid = ppid;
    }
    return k;
}

/* One pass over /proc: this user's session lord-server (its RSS) and whether Xwayland runs for this user. */
static void scan_procs(FetchLive *l, int *xwayland) {
    DIR *d = opendir("/proc");
    if (!d) return;
    uid_t me = getuid();
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!isdigit((unsigned char)e->d_name[0])) continue;
        char path[300], comm[32];
        struct stat st;
        snprintf(path, sizeof path, "/proc/%s", e->d_name);
        if (stat(path, &st) || st.st_uid != me) continue;
        snprintf(path, sizeof path, "/proc/%s/comm", e->d_name);
        read_line(path, comm, sizeof comm);
        if (!strcmp(comm, "Xwayland")) *xwayland = 1;
        if (strcmp(comm, "lord-server")) continue;
        /* the session's, not an isolated app's (lord-server --app <id>) */
        snprintf(path, sizeof path, "/proc/%s/cmdline", e->d_name);
        char *cmd = slurp(path, 4096);
        int app = 0;
        if (cmd) {
            for (char *a = cmd; *a; a += strlen(a) + 1) if (!strcmp(a, "--app")) { app = 1; break; }
            free(cmd);
        }
        if (app) continue;
        snprintf(path, sizeof path, "/proc/%s/status", e->d_name);
        char *s = slurp(path, 8192);
        unsigned long long rss = fetch_status_rss(s);
        free(s);
        if (rss > l->server_rss_kb) l->server_rss_kb = rss;
    }
    closedir(d);
}

/* The LordOS desktop in use (settings.json's theme, named by its theme.json: next to this binary when installed,
 * /usr/lib/lordos/bin/../themes, or in the repository for a development build) and the interface language. */
static void desktop(FetchLive *l, char *lang, size_t ln) {
    char p[PATH_MAX + 64], theme[72];
    struct passwd *pw = getpwuid(getuid());
    char *t = NULL;
    if (pw && pw->pw_dir && (size_t)snprintf(p, sizeof p, "%s/.config/lordos/settings.json", pw->pw_dir) < sizeof p)
        t = slurp(p, 4u << 20);
    fetch_settings(t, theme, sizeof theme, lang, ln);   /* "default" without the file */
    free(t);
    char exe[PATH_MAX];
    ssize_t k = readlink("/proc/self/exe", exe, sizeof exe - 1);
    exe[k > 0 ? k : 0] = 0;
    char *slash = strrchr(exe, '/');
    if (slash) *slash = 0;
    static const char *const up[] = { "/..", "/../..", NULL };
    for (int i = 0; i < 3 && !l->desktop[0]; i++) {
        char q[PATH_MAX + 128];
        if (up[i] && !exe[0]) continue;
        if (up[i]) snprintf(q, sizeof q, "%.*s%s/themes/%s/theme.json", PATH_MAX, exe, up[i], theme);
        else snprintf(q, sizeof q, "/usr/lib/lordos/themes/%s/theme.json", theme);
        t = slurp(q, 256 * 1024);
        fetch_theme_name(t, l->desktop, sizeof l->desktop);
        free(t);
    }
}

static void addresses(FetchLive *l, const char *root) {
    char p[1024], ifn[IF_NAMESIZE + 1], v4[64] = "", v6[80] = "";
    snprintf(p, sizeof p, "%s/proc/net/route", root);
    char *t = slurp(p, 64 * 1024);
    int ok = !fetch_route_iface(t, ifn, sizeof ifn);
    free(t);
    if (!ok) return;
    struct ifaddrs *all;
    if (!root[0] && !getifaddrs(&all)) {
        for (struct ifaddrs *a = all; a && !v4[0]; a = a->ifa_next) {
            if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET || strcmp(a->ifa_name, ifn)) continue;
            char txt[INET_ADDRSTRLEN];
            int bits = 0;
            if (a->ifa_netmask) { unsigned m = ntohl(((struct sockaddr_in *)a->ifa_netmask)->sin_addr.s_addr); while (m & 0x80000000u) { bits++; m <<= 1; } }
            if (inet_ntop(AF_INET, &((struct sockaddr_in *)a->ifa_addr)->sin_addr, txt, sizeof txt)) snprintf(v4, sizeof v4, "%s/%d", txt, bits);
        }
        freeifaddrs(all);
    }
    snprintf(p, sizeof p, "%s/proc/net/if_inet6", root);
    t = slurp(p, 64 * 1024);
    fetch_inet6(t, ifn, v6, sizeof v6);
    free(t);
    if (v4[0] || v6[0])
        snprintf(l->ip, sizeof l->ip, "%s%s%s (%s)", v4, v4[0] && v6[0] ? " · " : "", v6, ifn);
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
    l->mem_used_kb = meminfo_used(meminfo_kb(t, "MemTotal:"), meminfo_kb(t, "MemFree:"), meminfo_kb(t, "Buffers:"),
                                  meminfo_kb(t, "Cached:"), meminfo_kb(t, "SReclaimable:"), meminfo_kb(t, "Shmem:"));
    l->swap_known = !fetch_swap(t, &l->swap_total_kb, &l->swap_used_kb);
    free(t);
    struct statvfs vs;
    snprintf(p, sizeof p, "%s/", root);
    if (!statvfs(p, &vs)) {
        l->disk_total = (unsigned long long)vs.f_blocks * vs.f_frsize;
        l->disk_used = l->disk_total - (unsigned long long)vs.f_bfree * vs.f_frsize;
    }
    /* dpkg's status file: one "Status: install ok installed" per installed package */
    snprintf(p, sizeof p, "%s/var/lib/dpkg/status", root);
    t = slurp(p, 64u << 20);
    l->packages = t ? 0 : -1;
    for (const char *q = t; q && (q = strstr(q, "\nStatus: install ok installed")); q++) l->packages++;
    if (t && !strncmp(t, "Status: install ok installed", 28)) l->packages++;
    char xver[32] = "";
    fetch_dpkg_version(t, "xwayland", xver, sizeof xver);
    free(t);
    /* this boot's startup time, as About shows it: written by lord-sessiond (root, not writable by others) */
    char bid[64];
    snprintf(p, sizeof p, "%s/proc/sys/kernel/random/boot_id", root);
    read_line(p, bid, sizeof bid);
    snprintf(p, sizeof p, "%s/var/lib/lordos/boot-times.jsonl", root);
    int fd = open(p, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat st;
    if (fd >= 0 && !fstat(fd, &st) && S_ISREG(st.st_mode) && (root[0] || st.st_uid == 0) && !(st.st_mode & 022) &&
        st.st_size <= 256 * 1024) {
        char *txt = malloc((size_t)st.st_size + 1);
        ssize_t n = txt ? read(fd, txt, (size_t)st.st_size) : -1;
        if (n >= 0) { txt[n] = 0; fetch_boot_time(txt, bid, &l->boot_s, l->boot_shown, sizeof l->boot_shown); }
        free(txt);
    }
    if (fd >= 0) close(fd);
    const char *sh = getenv("SHELL");
    if (sh && *sh) {
        const char *s = strrchr(sh, '/');
        snprintf(l->shell, sizeof l->shell, "%.31s", s ? s + 1 : sh);
        clean(l->shell);
    }
    snprintf(l->session, sizeof l->session, "%s", getenv("WAYLAND_DISPLAY") ? "Wayland" : getenv("DISPLAY") ? "X11" : "");
    if (!share) addresses(l, root);
    if (root[0]) return;                    /* the rest is this process and this user's session */
    char names[8][32];
    const char *anc[8];
    int na = ancestors(names, 8);
    for (int i = 0; i < na; i++) anc[i] = names[i];
    char tty[64] = "";
    if (ttyname_r(0, tty, sizeof tty)) tty[0] = 0;
    fetch_terminal_pick(getenv("TERM_PROGRAM"), getenv("TERM_PROGRAM_VERSION"), getenv("SSH_CONNECTION") || getenv("SSH_TTY"),
                        tty, anc, na, l->terminal, sizeof l->terminal);
    int xw = 0;
    scan_procs(l, &xw);
    if (xw) snprintf(l->xwayland, sizeof l->xwayland, "%s", xver[0] ? xver : "?");
    char lang[24];
    desktop(l, lang, sizeof lang);
    const char *lc = getenv("LC_ALL");
    if (!lc || !*lc) lc = getenv("LANG");
    /* the interface language: settings.json's, or (null) the one LANG names */
    char lcs[48];
    snprintf(lcs, sizeof lcs, "%.40s", lc && *lc ? lc : "C");
    clean(lcs);
    snprintf(l->locale, sizeof l->locale, "%s · interface %s", lcs, lang[0] ? lang : "from LANG");
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

#define LINES_MAX 40
typedef struct { char label[16]; char value[256]; } Line;

static void bytes_text(unsigned long long v, char *out, size_t n) {
    if (v >= 1000ULL * 1000 * 1000 * 1000) snprintf(out, n, "%.1f TB", v / 1e12);
    else if (v >= 10ULL * 1000 * 1000 * 1000) snprintf(out, n, "%.0f GB", v / 1e9);
    else snprintf(out, n, "%.1f GB", v / 1e9);
}

/* Memory comes in binary sizes: a 16 GB board has 15.8 GiB usable, which in thousands reads "17 GB". */
static void mem_text(unsigned long long v, char *out, size_t n) {
    double g = v / 1073741824.0;
    if (g < 1) snprintf(out, n, "%.0f MB", v / 1048576.0);   /* swap barely in use: "12 MB", not "0.0 GB" */
    else snprintf(out, n, g >= 10 ? "%.0f GB" : "%.1f GB", g);
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

    snprintf(v, sizeof v, "LordOS %s%s%s%s", r->sw.lordos, r->sw.os[0] ? " (" : "", r->sw.os, r->sw.os[0] ? ")" : "");
    n = add(ls, n, "OS", v);
    const SrMachine *m = &r->machine;
    snprintf(v, sizeof v, "%s%s%s", m->vendor, m->vendor[0] && m->product[0] ? " " : "", m->product);
    if (!v[0]) snprintf(v, sizeof v, "%s", m->model);
    if (m->virt[0]) { size_t o = strlen(v); snprintf(v + o, sizeof v - o, "%svirtual machine (%s)", o ? " · " : "", m->virt); }
    n = add(ls, n, "Host", v);
    n = add(ls, n, "Kernel", r->sw.kernel);
    if (l->uptime_s >= 0) {
        long d = l->uptime_s / 86400, h = l->uptime_s % 86400 / 3600, mi = l->uptime_s % 3600 / 60;
        if (d) snprintf(v, sizeof v, "%ld d %ld h %ld min", d, h, mi);
        else if (h) snprintf(v, sizeof v, "%ld h %ld min", h, mi);
        else snprintf(v, sizeof v, "%ld min", mi);
        n = add(ls, n, "Uptime", v);
    }
    if (l->packages > 0) { snprintf(v, sizeof v, "%d (dpkg)", l->packages); n = add(ls, n, "Packages", v); }
    n = add(ls, n, "Shell", l->shell);
    /* one line per kind of monitor: three alike read "3 × S27F350, 1920 × 1080, 27"" */
    char disp[SR_MAX_DISPLAY][160];
    int dcount[SR_MAX_DISPLAY], nd = 0;
    for (int i = 0; i < r->ndisplay && i < SR_MAX_DISPLAY; i++) {
        const SrDisplay *x = &r->display[i];
        if (x->pref_w) snprintf(a, sizeof a, "%d × %d", x->pref_w, x->pref_h); else snprintf(a, sizeof a, "%s", x->mode);
        snprintf(v, sizeof v, "%s%s%s", x->name[0] ? x->name : x->connector, a[0] ? ", " : "", a);
        if (x->diag_tenths) { size_t o = strlen(v); snprintf(v + o, sizeof v - o, ", %d\"", (x->diag_tenths + 5) / 10); }
        int j = 0;
        while (j < nd && strcmp(disp[j], v)) j++;
        if (j < nd) { dcount[j]++; continue; }
        snprintf(disp[nd], sizeof disp[nd], "%.150s", v);
        dcount[nd++] = 1;
    }
    for (int j = 0; j < nd; j++) {
        if (dcount[j] > 1) snprintf(v, sizeof v, "%d × %.150s", dcount[j], disp[j]); else snprintf(v, sizeof v, "%.150s", disp[j]);
        n = add(ls, n, "Display", v);
    }
    n = add(ls, n, "DE", l->desktop);
    /* the compositor: lord-server, how it drives the hardware, and the protocols this session has */
    if (l->server[0]) {
        snprintf(v, sizeof v, "%s (C · DRM/KMS · evdev)%s%s%s%s", l->server, l->session[0] ? " · " : "", l->session,
                 l->xwayland[0] ? " · Xwayland " : "", l->xwayland);
        n = add(ls, n, "Compositor", v);
    }
    n = add(ls, n, "Terminal", l->terminal);
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
        mem_text(l->mem_used_kb * 1024ULL, a, sizeof a);
        mem_text(r->mem.total_kb * 1024ULL, b2, sizeof b2);
        snprintf(v, sizeof v, "%s / %s (%d %%)", a, b2, (int)(l->mem_used_kb * 100 / r->mem.total_kb));
        n = add(ls, n, "Memory", v);
    }
    if (l->swap_known) {
        if (!l->swap_total_kb) snprintf(v, sizeof v, "none");
        else {
            mem_text(l->swap_used_kb * 1024ULL, a, sizeof a);
            mem_text(l->swap_total_kb * 1024ULL, b2, sizeof b2);
            snprintf(v, sizeof v, "%s / %s (%d %%)", a, b2, (int)(l->swap_used_kb * 100 / l->swap_total_kb));
        }
        n = add(ls, n, "Swap", v);
    }
    if (l->disk_total) {
        bytes_text(l->disk_used, a, sizeof a);
        bytes_text(l->disk_total, b2, sizeof b2);
        snprintf(v, sizeof v, "%s / %s (%d %%)", a, b2, (int)(l->disk_used * 100 / l->disk_total));
        n = add(ls, n, "Disk (/)", v);
    }
    n = add(ls, n, "Local IP", l->ip);
    n = add(ls, n, "Locale", l->locale);
    /* LordOS itself, as About shows it */
    n = add(ls, n, "UI engine", l->engine);
    n = add(ls, n, "Build", l->build);
    if (l->boot_s > 0) {
        snprintf(v, sizeof v, "%.3f s to the %s", l->boot_s, !strcmp(l->boot_shown, "session") ? "desktop" : "login screen");
        n = add(ls, n, "Boot time", v);
    }
    if (l->server_rss_kb) {
        snprintf(v, sizeof v, "%.1f MB (lord-server)", l->server_rss_kb / 1024.0);
        n = add(ls, n, "Server RAM", v);
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
    else snprintf(title, sizeof title, "LordOS");
    /* title, rule, the lines, and with color a blank line and the palette; at least the crown's height */
    int rows = n + (c == FETCH_PLAIN ? 2 : 4);
    if (rows < (CROWN_H + 1) / 2) rows = (CROWN_H + 1) / 2;
    if (rows < (int)(sizeof PLAIN / sizeof PLAIN[0])) rows = (int)(sizeof PLAIN / sizeof PLAIN[0]);
    for (int y = 0; y < rows; y++) {
        hb_printf(&b, " ");
        crown_row(&b, y, c);
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
            hb_printf(&b, "%*s%s", 12 - (int)strlen(x->label), "", x->value);
        } else if (y - 2 == n + 1 && c != FETCH_PLAIN) {
            static const char keys[] = "HYAWLVB";
            for (const char *k = keys; *k; k++) { bg(&b, pal(*k), c); hb_printf(&b, "   "); }
            hb_printf(&b, "\033[0m");
        }
        hb_printf(&b, "\n");
    }
    return hb_take(&b);
}
