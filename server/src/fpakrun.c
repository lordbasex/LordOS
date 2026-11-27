/* fpakrun.c — runs one command of fpak_cmd_argv's closed list (docs/FLATPAK.md, E3): its own child, in
 * lordos-net-flathub.scope (the firewall component lordos:flathub), /usr/bin/flatpak with a minimal environment, stdin
 * /dev/null and stdout and stderr read back line by line as printable UTF-8. Used by lord.flatpak (ui/js_flatpak.c) and
 * the test tool tests/fpak_cli.c. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "fpakrun.h"
#include "fpak.h"
#include "sec/cgroup.h"
#include "sec/netconn.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define LINE_MAX_BYTES FPAK_LINE_MAX

int fpak_binary_ok(void) {
    struct stat st;
    return !lstat(FPAK_FLATPAK, &st) && S_ISREG(st.st_mode) && st.st_uid == 0 && !(st.st_mode & 022) && (st.st_mode & 0111);
}
const char *fpak_native_arch(void) {
#if defined(__x86_64__)
    return "x86_64";
#else
    return NULL;                                /* phase 1: x86-64 PCs only */
#endif
}

/* printable UTF-8 only (a control character or a broken sequence becomes nothing), at most LINE_MAX_BYTES */
static void clean(const char *in, size_t n, char *out) {
    size_t o = 0;
    for (size_t i = 0; i < n && o < LINE_MAX_BYTES;) {
        unsigned char c = (unsigned char)in[i];
        int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (!len || i + (size_t)len > n) { i++; continue; }
        int ok = 1;
        for (int k = 1; k < len; k++) if (((unsigned char)in[i + k] >> 6) != 2) ok = 0;
        if (!ok || (len == 1 && (c < 0x20 || c == 0x7f) && c != '\t')) { i++; continue; }
        if (o + (size_t)len > LINE_MAX_BYTES) break;
        memcpy(out + o, in + i, (size_t)len);
        o += (size_t)len;
        i += (size_t)len;
    }
    out[o] = 0;
}

int fpak_run(int cmd, const char *ref, void (*on_line)(void *ud, const char *line), void *ud, void (*on_pid)(void *ud, pid_t pid)) {
    const char *arch = fpak_native_arch();
    char *argv[FPAK_ARGV_MAX], buf[2048];
    if (!arch || !fpak_binary_ok() || fpak_cmd_argv(cmd, ref, arch, argv, buf, sizeof buf) < 0) return 127;
    /* the environment it needs and nothing else: no session bus (no portal, no system helper), no display */
    static const char *const keep[] = { "HOME", "USER", "LOGNAME", "LANG", "LC_ALL", "LC_MESSAGES", "XDG_RUNTIME_DIR", NULL };
    char envbuf[8][1024], *envp[10];
    int ne = 0;
    for (int i = 0; keep[i]; i++) {
        const char *v = getenv(keep[i]);
        if (v && strlen(v) < 900) { snprintf(envbuf[ne], sizeof envbuf[ne], "%s=%s", keep[i], v); envp[ne] = envbuf[ne]; ne++; }
    }
    snprintf(envbuf[ne], sizeof envbuf[ne], "PATH=/usr/bin:/bin");
    envp[ne] = envbuf[ne];
    envp[++ne] = NULL;
    int p[2];
    if (pipe2(p, O_CLOEXEC)) return 127;
    pid_t pid = fork();
    if (pid < 0) { close(p[0]); close(p[1]); return 127; }
    if (pid == 0) {
        setsid();                                       /* its own process group: cancel stops all of it */
        int null = open("/dev/null", O_RDONLY);
        if (null >= 0) dup2(null, 0);
        dup2(p[1], 1);
        dup2(p[1], 2);
        syscall(SYS_close_range, 3, ~0u, 0);
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        for (int s = 1; s < 32; s++) signal(s, SIG_DFL);
        const char *home = getenv("HOME");
        if (home && chdir(home)) {}
        /* its own firewall component; in a LordOS session a command that can't be placed there never runs. The scope's
         * memory.max is the one the flathub connector writes too (netconn.h): enough for an OSTree pull of a large
         * runtime, still bounded */
        CgroupLimits lim = { .mem_max_mb = NC_FLATHUB_MEM_MAX_MB };
        if (cgroup_enter("lordos-net-flathub.scope", &lim) && getenv("LORDOS_SESSIOND")) {
            static const char msg[] = "flatpak: could not enter lordos-net-flathub.scope: not run\n";
            if (write(2, msg, sizeof msg - 1) < 0) {}
            _exit(126);
        }
        execve(FPAK_FLATPAK, argv, envp);
        _exit(127);
    }
    close(p[1]);
    if (on_pid) on_pid(ud, pid);
    char acc[4096], line[LINE_MAX_BYTES + 1];
    size_t have = 0;
    for (;;) {
        ssize_t r = read(p[0], acc + have, sizeof acc - have);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        have += (size_t)r;
        size_t start = 0;
        for (size_t i = 0; i < have; i++) {
            if (acc[i] != '\n' && acc[i] != '\r') continue;
            clean(acc + start, i - start, line);
            if (line[0] && on_line) on_line(ud, line);
            start = i + 1;
        }
        if (start == 0 && have == sizeof acc) start = have;     /* a line longer than the buffer: dropped */
        memmove(acc, acc + start, have - start);
        have -= start;
    }
    if (have) {
        clean(acc, have, line);
        if (line[0] && on_line) on_line(ud, line);
    }
    close(p[0]);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    if (on_pid) on_pid(ud, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
}

