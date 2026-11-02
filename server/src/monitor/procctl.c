/* procctl.c — protected processes, a PID-reuse-safe signal and the Get Info details of one
 * process (see procctl.h and docs/MONITOR.md, "Process actions"). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "procctl.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif
#ifndef SYS_pidfd_send_signal
#define SYS_pidfd_send_signal 424
#endif

#define PF_KTHREAD_FLAG 0x00200000u     /* include/linux/sched.h */
#define MAX_FD_SHOW 300                 /* open files listed (the total is always counted) */
#define MAX_ARGS 128
#define MAX_ARG_LEN 2048

/* ---- /proc/<pid>/stat ---- */

int proc_stat_parse(const char *txt, ProcStat *s) {
    memset(s, 0, sizeof *s);
    const char *lp = strchr(txt, '('), *rp = strrchr(txt, ')');
    if (!lp || !rp || rp < lp || rp[1] != ' ' || !rp[2]) return -1;
    s->pid = atoi(txt);
    int cl = (int)(rp - lp - 1);
    snprintf(s->comm, sizeof s->comm, "%.*s", cl < 63 ? cl : 63, lp + 1);
    s->state = rp[2];
    /* fields 4.. (ppid) as numbers: v[0] = field 4 */
    long long v[22] = { 0 };
    char *q = (char *)rp + 3;
    for (int i = 0; i < 22; i++) {
        char *e;
        v[i] = strtoll(q, &e, 10);
        if (e == q) return -1;
        q = e;
    }
    s->ppid = (int)v[0];
    s->pgid = (int)v[1];
    s->sid = (int)v[2];
    s->flags = (unsigned)v[5];
    s->minflt = (unsigned long long)v[6];
    s->majflt = (unsigned long long)v[8];
    s->utime = (unsigned long long)v[10];
    s->stime = (unsigned long long)v[11];
    s->prio = (long)v[14];
    s->nice = (long)v[15];
    s->threads = (long)v[16];
    s->start = (unsigned long long)v[18];
    s->vsize = (unsigned long long)v[19];
    s->rss = (unsigned long long)(v[20] < 0 ? 0 : v[20]);
    return s->pid > 0 ? 0 : -1;
}

int proc_stat_read(int pid, ProcStat *s) {
    char p[64], txt[1024];
    snprintf(p, sizeof p, "/proc/%d/stat", pid);
    if (mon_read_small(p, txt, sizeof txt) <= 0) return -1;
    return proc_stat_parse(txt, s);
}

/* ---- the protected list ---- */

static const char *const system_names[] = {
    "lord-sessiond", "axia-netperm", "lord-netd", "axia-fand",          /* root services */
    "axia-vault", "axia-maild", "axia-doc", "axia-media",               /* the session's helpers */
    "lord-admin",                                                       /* the root helper (comm) */
    "pipewire", "pipewire-pulse", "wireplumber",                        /* sound and screen sharing */
    "systemd", "(sd-pam)",                                              /* the init and user managers */
    NULL,
};

static int in_system(const char *name) {
    if (!name || !name[0]) return 0;
    for (int i = 0; system_names[i]; i++) if (!strcmp(name, system_names[i])) return 1;
    return 0;
}

static const char *base_of(const char *p) { const char *b = strrchr(p, '/'); return b ? b + 1 : p; }

const char *proc_protect_reason(int pid, unsigned flags, const char *comm, const char *exe, const char *argv1, int session_pid) {
    if (pid <= 1) return "init";
    if (pid == 2 || (flags & PF_KTHREAD_FLAG)) return "kernel";
    if (pid == session_pid) return "session";
    char eb[256] = "";
    if (exe && exe[0]) {
        snprintf(eb, sizeof eb, "%s", base_of(exe));
        char *del = strstr(eb, " (deleted)");          /* updated while running */
        if (del) *del = 0;
    }
    if (in_system(comm) || in_system(eb)) return "system";
    /* axia-server: an isolated app (`axia-server --app <id>`) may be quit; any other instance
     * (another session, a terminal holder) is part of LordOS */
    if (!strcmp(eb, "axia-server") || (!eb[0] && comm && !strcmp(comm, "axia-server")))
        return argv1 && !strcmp(argv1, "--app") ? NULL : "system";
    return NULL;
}

/* argv[1] from /proc/<pid>/cmdline into out ("" if there is none). */
static void read_argv1(int pid, char *out, size_t n) {
    char p[64], cmd[512];
    out[0] = 0;
    snprintf(p, sizeof p, "/proc/%d/cmdline", pid);
    long k = mon_read_small(p, cmd, sizeof cmd);
    if (k <= 0) return;
    size_t a0 = strnlen(cmd, (size_t)k);
    if ((long)a0 + 1 < k) snprintf(out, n, "%s", cmd + a0 + 1);
}

static void read_exe(int pid, char *out, size_t n) {
    char p[64];
    snprintf(p, sizeof p, "/proc/%d/exe", pid);
    ssize_t k = readlink(p, out, n - 1);
    out[k > 0 ? k : 0] = 0;
}

const char *proc_protected(int pid, int session_pid) {
    ProcStat s;
    if (pid <= 1) return "init";
    if (proc_stat_read(pid, &s)) return NULL;
    char exe[PATH_MAX], a1[64];
    read_exe(pid, exe, sizeof exe);
    read_argv1(pid, a1, sizeof a1);
    return proc_protect_reason(pid, s.flags, s.comm, exe, a1, session_pid);
}

/* ---- signal ---- */

const char *proc_signal(int pid, unsigned long long start, int sig, int session_pid) {
    if (pid <= 0 || pid > PROC_PID_MAX || (sig != SIGTERM && sig != SIGKILL)) return "proc.badArgument";
    if (pid == 1) return "proc.protectedInit";
    /* 1. pin the process: from here on the pidfd names this process, never a later one */
    int pfd = (int)syscall(SYS_pidfd_open, pid, 0);
    if (pfd < 0 && errno == ESRCH) return "proc.gone";
    /* 2. is it still the one the user chose? (a PID reused before step 1 has another start time) */
    ProcStat s;
    if (proc_stat_read(pid, &s) || s.start != start) { if (pfd >= 0) close(pfd); return "proc.gone"; }
    const char *why = proc_protected(pid, session_pid);
    if (why) {
        if (pfd >= 0) close(pfd);
        if (!strcmp(why, "init")) return "proc.protectedInit";
        if (!strcmp(why, "kernel")) return "proc.protectedKernel";
        if (!strcmp(why, "session")) return "proc.protectedSession";
        return "proc.protectedSystem";
    }
    /* 3. signal the pinned process; if it died meanwhile this is ESRCH, never another process */
    int r;
    if (pfd >= 0) {
        r = (int)syscall(SYS_pidfd_send_signal, pfd, sig, NULL, 0);
        int e = errno;
        close(pfd);
        errno = e;
    } else {
        /* no pidfd (kernel before 5.3): kill(2) right after the check; the window is small but real */
        r = kill(pid, sig);
    }
    if (!r) return NULL;
    if (errno == ESRCH) return "proc.gone";
    if (errno == EPERM) return "proc.denied";
    return "proc.failed";
}

/* ---- Get Info ---- */

static unsigned long long btime_s(void) {
    static unsigned long long bt;
    if (bt) return bt;
    static char txt[65536];
    if (mon_read_small("/proc/stat", txt, sizeof txt) <= 0) return 0;
    const char *p = strstr(txt, "\nbtime ");
    if (p) bt = strtoull(p + 7, NULL, 10);
    return bt;
}

/* "Key:   1234 kB" lines: value of key (kB) or -1. */
static long long kv(const char *txt, const char *key) {
    size_t kl = strlen(key);
    for (const char *p = txt; p && *p; p = strchr(p, '\n'), p = p ? p + 1 : NULL) {
        if (!strncmp(p, key, kl) && p[kl] == ':') return strtoll(p + kl + 1, NULL, 10);
    }
    return -1;
}

static void num_or_null(Buf *b, const char *key, long long v, long long mul) {
    if (v < 0) mon_buf_printf(b, ",\"%s\":null", key);
    else mon_buf_printf(b, ",\"%s\":%lld", key, v * mul);
}

typedef struct { int fd; unsigned long ino; char kind; } FdSock;     /* kind: 0 unknown, 't' tcp, 'u' udp, 'x' unix */

static const char *tcp_state(unsigned st) {
    static const char *const n[] = { "", "ESTABLISHED", "SYN_SENT", "SYN_RECV", "FIN_WAIT1", "FIN_WAIT2", "TIME_WAIT",
                                     "CLOSE", "CLOSE_WAIT", "LAST_ACK", "LISTEN", "CLOSING" };
    return st < sizeof n / sizeof n[0] ? n[st] : "";
}

/* "0100007F:0035" (v4) or 32 hex digits + ":port" (v6) → "127.0.0.1:53" / "[::1]:53"; zero → "". */
static void fmt_addr(const char *hex, int v6, char *out, size_t n) {
    out[0] = 0;
    const char *colon = strchr(hex, ':');
    if (!colon) return;
    unsigned port = (unsigned)strtoul(colon + 1, NULL, 16);
    char ip[INET6_ADDRSTRLEN] = "";
    int zero = 1;
    if (!v6) {
        struct in_addr a;
        a.s_addr = (uint32_t)strtoul(hex, NULL, 16);
        zero = a.s_addr == 0;
        inet_ntop(AF_INET, &a, ip, sizeof ip);
        if (zero && !port) return;
        snprintf(out, n, "%s:%u", ip, port);
    } else {
        struct in6_addr a;
        if (colon - hex != 32) return;
        for (int i = 0; i < 4; i++) {
            char w[9];
            memcpy(w, hex + i * 8, 8);
            w[8] = 0;
            uint32_t v = (uint32_t)strtoul(w, NULL, 16);
            memcpy(&a.s6_addr[i * 4], &v, 4);
            if (v) zero = 0;
        }
        inet_ntop(AF_INET6, &a, ip, sizeof ip);
        if (zero && !port) return;
        snprintf(out, n, "[%s]:%u", ip, port);
    }
}

typedef struct { unsigned long ino; char proto[8], local[64], remote[64], state[16], path[256]; int found; } SockInfo;

/* Fills the sockets of fds from /proc/<pid>/net/{tcp,tcp6,udp,udp6,unix} (that process's network
 * namespace). */
static void sockets_lookup(int pid, SockInfo *si, int n) {
    if (!n) return;
    static const struct { const char *file, *proto; int v6, tcp; } tabs[] = {
        { "tcp", "TCP", 0, 1 }, { "tcp6", "TCP", 1, 1 }, { "udp", "UDP", 0, 0 }, { "udp6", "UDP", 1, 0 },
    };
    char path[64], line[512];
    for (size_t t = 0; t < sizeof tabs / sizeof tabs[0]; t++) {
        snprintf(path, sizeof path, "/proc/%d/net/%s", pid, tabs[t].file);
        FILE *f = fopen(path, "re");
        if (!f) continue;
        while (fgets(line, sizeof line, f)) {
            char la[80], ra[80];
            unsigned st;
            unsigned long ino;
            /* sl local rem st tx:rx tr:tm retr uid timeout inode */
            if (sscanf(line, " %*s %79s %79s %x %*s %*s %*s %*s %*s %lu", la, ra, &st, &ino) != 4 || !ino) continue;
            for (int i = 0; i < n; i++) {
                if (si[i].found || si[i].ino != ino) continue;
                si[i].found = 1;
                snprintf(si[i].proto, sizeof si[i].proto, "%s", tabs[t].proto);
                fmt_addr(la, tabs[t].v6, si[i].local, sizeof si[i].local);
                fmt_addr(ra, tabs[t].v6, si[i].remote, sizeof si[i].remote);
                snprintf(si[i].state, sizeof si[i].state, "%s", tabs[t].tcp ? tcp_state(st) : st == 1 ? "ESTABLISHED" : "");
            }
        }
        fclose(f);
    }
    snprintf(path, sizeof path, "/proc/%d/net/unix", pid);
    FILE *f = fopen(path, "re");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        unsigned long ino;
        int off = 0;
        /* Num RefCount Protocol Flags Type St Inode Path */
        if (sscanf(line, "%*s %*s %*s %*s %*s %*s %lu%n", &ino, &off) != 1 || !ino) continue;
        for (int i = 0; i < n; i++) {
            if (si[i].found || si[i].ino != ino) continue;
            si[i].found = 1;
            snprintf(si[i].proto, sizeof si[i].proto, "UNIX");
            const char *p = line + off;
            while (*p == ' ') p++;
            snprintf(si[i].path, sizeof si[i].path, "%s", p);
            si[i].path[strcspn(si[i].path, "\n")] = 0;
        }
    }
    fclose(f);
}

static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

static void fds_json(Buf *b, int pid) {
    char dir[64];
    snprintf(dir, sizeof dir, "/proc/%d/fd", pid);
    DIR *d = opendir(dir);
    if (!d) { mon_buf_printf(b, ",\"fds\":null,\"fdTotal\":null"); return; }
    int *fds = malloc(sizeof(int) * MAX_FD_SHOW), nf = 0, total = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!isdigit((unsigned char)e->d_name[0])) continue;
        total++;
        if (fds && nf < MAX_FD_SHOW) fds[nf++] = atoi(e->d_name);
    }
    closedir(d);
    if (!fds) { mon_buf_printf(b, ",\"fds\":null,\"fdTotal\":%d", total); return; }
    qsort(fds, (size_t)nf, sizeof *fds, cmp_int);
    static char targets[MAX_FD_SHOW][PATH_MAX];
    SockInfo *si = calloc((size_t)(nf ? nf : 1), sizeof *si);
    int *sidx = calloc((size_t)(nf ? nf : 1), sizeof *sidx), ns = 0;
    for (int i = 0; i < nf; i++) {
        char p[96];
        snprintf(p, sizeof p, "/proc/%d/fd/%d", pid, fds[i]);
        ssize_t k = readlink(p, targets[i], PATH_MAX - 1);
        targets[i][k > 0 ? k : 0] = 0;
        sidx[i] = -1;
        if (si && sidx && !strncmp(targets[i], "socket:[", 8)) { si[ns].ino = strtoul(targets[i] + 8, NULL, 10); sidx[i] = ns++; }
    }
    if (si) sockets_lookup(pid, si, ns);
    mon_buf_printf(b, ",\"fdTotal\":%d,\"fds\":[", total);
    for (int i = 0; i < nf; i++) {
        const char *t = targets[i];
        mon_buf_printf(b, "%s{\"fd\":%d", i ? "," : "", fds[i]);
        if (sidx && sidx[i] >= 0 && si && si[sidx[i]].found) {
            SockInfo *s = &si[sidx[i]];
            mon_buf_printf(b, ",\"k\":\"socket\",\"proto\":\"%s\"", s->proto);
            if (s->path[0]) { mon_buf_printf(b, ",\"p\":"); mon_buf_str(b, s->path); }
            if (s->local[0]) { mon_buf_printf(b, ",\"local\":"); mon_buf_str(b, s->local); }
            if (s->remote[0]) { mon_buf_printf(b, ",\"remote\":"); mon_buf_str(b, s->remote); }
            if (s->state[0]) mon_buf_printf(b, ",\"state\":\"%s\"", s->state);
        } else {
            const char *k = !strncmp(t, "socket:", 7) ? "socket" : !strncmp(t, "pipe:", 5) ? "pipe" : !strncmp(t, "anon_inode:", 11) ? "anon"
                          : !strncmp(t, "/dev/", 5) ? "dev" : t[0] == '/' ? "file" : "other";
            mon_buf_printf(b, ",\"k\":\"%s\",\"p\":", k);
            mon_buf_str(b, t);
        }
        mon_buf_printf(b, "}");
    }
    mon_buf_printf(b, "]");
    free(si);
    free(sidx);
    free(fds);
}

static void cmdline_json(Buf *b, int pid) {
    char p[64];
    static char cmd[32768];
    snprintf(p, sizeof p, "/proc/%d/cmdline", pid);
    long k = mon_read_small(p, cmd, sizeof cmd);
    mon_buf_printf(b, ",\"cmdline\":[");
    int n = 0, cut = k >= (long)sizeof cmd - 1;
    for (long i = 0; k > 0 && i < k && n < MAX_ARGS;) {
        size_t l = strnlen(cmd + i, (size_t)(k - i));
        char save = 0;
        size_t keep = l;
        if (keep > MAX_ARG_LEN) { keep = MAX_ARG_LEN; while (keep && ((unsigned char)cmd[i + keep] & 0xc0) == 0x80) keep--; cut = 1; }
        if (keep < l) { save = cmd[i + keep]; cmd[i + keep] = 0; }
        mon_buf_printf(b, "%s", n ? "," : "");
        mon_buf_str(b, cmd + i);
        if (keep < l) cmd[i + keep] = save;
        n++;
        i += (long)l + 1;
        if (n == MAX_ARGS && i < k) cut = 1;
    }
    mon_buf_printf(b, "],\"cmdCut\":%s", cut ? "true" : "false");
}

void proc_detail_json(Buf *b, int pid, unsigned long long start, int full, int session_pid) {
    ProcStat s;
    if (pid <= 0 || pid > PROC_PID_MAX || proc_stat_read(pid, &s) || s.start != start) {
        mon_buf_printf(b, "{\"pid\":%d,\"gone\":true}", pid);
        return;
    }
    long hz = sysconf(_SC_CLK_TCK), page = sysconf(_SC_PAGESIZE);
    char path[64];
    static char st[8192], sm[4096];
    snprintf(path, sizeof path, "/proc/%d/status", pid);
    if (mon_read_small(path, st, sizeof st) <= 0) st[0] = 0;
    unsigned ruid = 0, euid = 0;
    const char *u = strstr(st, "\nUid:");
    if (u) sscanf(u + 5, "%u %u", &ruid, &euid);
    mon_buf_printf(b, "{\"pid\":%d,\"gone\":false,\"start\":%llu,\"hz\":%ld,\"name\":", pid, s.start, hz);
    mon_buf_str(b, s.comm);
    mon_buf_printf(b, ",\"state\":\"%c\",\"ppid\":%d,\"pgid\":%d,\"sid\":%d,\"uid\":%u,\"euid\":%u,"
                      "\"utime\":%llu,\"stime\":%llu,\"minflt\":%llu,\"majflt\":%llu,\"prio\":%ld,\"nice\":%ld,\"threads\":%ld,"
                      "\"startMs\":%.0f,\"vsize\":%llu,\"rss\":%llu",
                   isalpha((unsigned char)s.state) ? s.state : '?', s.ppid, s.pgid, s.sid, ruid, euid, s.utime, s.stime,
                   s.minflt, s.majflt, s.prio, s.nice, s.threads,
                   btime_s() ? ((double)btime_s() + (double)s.start / (double)hz) * 1000.0 : 0.0, s.vsize, s.rss * (unsigned long long)page);
    num_or_null(b, "rssAnon", kv(st, "RssAnon"), 1024);
    num_or_null(b, "rssFile", kv(st, "RssFile"), 1024);
    num_or_null(b, "rssShmem", kv(st, "RssShmem"), 1024);
    num_or_null(b, "swap", kv(st, "VmSwap"), 1024);
    num_or_null(b, "vctx", kv(st, "voluntary_ctxt_switches"), 1);
    num_or_null(b, "nvctx", kv(st, "nonvoluntary_ctxt_switches"), 1);
    num_or_null(b, "seccomp", kv(st, "Seccomp"), 1);
    num_or_null(b, "nnp", kv(st, "NoNewPrivs"), 1);
    /* smaps_rollup needs ptrace read access: the owner (or root) only */
    snprintf(path, sizeof path, "/proc/%d/smaps_rollup", pid);
    if (full && mon_read_small(path, sm, sizeof sm) > 0) {
        long long pc = kv(sm, "Private_Clean"), pd = kv(sm, "Private_Dirty"), sc = kv(sm, "Shared_Clean"), sd = kv(sm, "Shared_Dirty");
        num_or_null(b, "private", pc >= 0 && pd >= 0 ? pc + pd : -1, 1024);
        num_or_null(b, "shared", sc >= 0 && sd >= 0 ? sc + sd : -1, 1024);
        num_or_null(b, "pss", kv(sm, "Pss"), 1024);
    } else mon_buf_printf(b, ",\"private\":null,\"shared\":null,\"pss\":null");
    /* cgroup v2 path (the app scope) */
    char cg[1024] = "";
    snprintf(path, sizeof path, "/proc/%d/cgroup", pid);
    if (mon_read_small(path, cg, sizeof cg) > 0) {
        char *z = strstr(cg, "0::");
        if (z) { memmove(cg, z + 3, strlen(z + 3) + 1); cg[strcspn(cg, "\n")] = 0; } else cg[0] = 0;
    }
    mon_buf_printf(b, ",\"cgroup\":");
    mon_buf_str(b, cg);
    char exe[PATH_MAX] = "", cwd[PATH_MAX] = "", a1[64];
    read_exe(pid, exe, sizeof exe);
    read_argv1(pid, a1, sizeof a1);
    const char *why = proc_protect_reason(pid, s.flags, s.comm, exe, a1, session_pid);
    mon_buf_printf(b, ",\"prot\":");
    if (why) mon_buf_str(b, why); else mon_buf_printf(b, "null");
    mon_buf_printf(b, ",\"isolated\":%s", !strcmp(base_of(exe), "axia-server") && !strcmp(a1, "--app") ? "true" : "false");
    mon_buf_printf(b, ",\"kthread\":%s", (s.flags & PF_KTHREAD_FLAG) || pid == 2 ? "true" : "false");
    snprintf(path, sizeof path, "/proc/%d/cwd", pid);
    if (full) { ssize_t k = readlink(path, cwd, sizeof cwd - 1); cwd[k > 0 ? k : 0] = 0; }
    mon_buf_printf(b, ",\"exe\":");
    if (full && exe[0]) mon_buf_str(b, exe); else mon_buf_printf(b, "null");
    mon_buf_printf(b, ",\"cwd\":");
    if (full && cwd[0]) mon_buf_str(b, cwd); else mon_buf_printf(b, "null");
    cmdline_json(b, pid);
    if (full) fds_json(b, pid); else mon_buf_printf(b, ",\"fds\":null,\"fdTotal\":null");
    mon_buf_printf(b, "}");
}
