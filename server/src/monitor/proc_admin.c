/* proc_admin.c — process actions of the root helper, for the Monitor's Processes tab when the
 * process belongs to another user or to root (the user's own processes are signalled by
 * axia-server itself, without root). Administrators only.
 *
 *   proc-signal <pid> <start> TERM|KILL   stop one process; <start> is its start time (field 22
 *                                         of /proc/<pid>/stat), so a reused PID is never hit
 *   proc-info <pid> <start>               the Get Info JSON with the parts only root can read
 *                                         (memory breakdown, executable, open files and sockets)
 *
 * The arguments are numbers and one of two words, checked here; the protected list
 * (procctl.c) applies too. It runs no program: one syscall (pidfd_send_signal) and reads of
 * /proc. proc-info sends its JSON as LOG lines of at most 900 bytes (cut on UTF-8 boundaries)
 * that the session joins; OK carries the number of lines. */
#include "proc_admin.h"
#include "procctl.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* "<pid> <start>[ <word>]" with nothing else: decimal numbers without signs or leading zeros. */
static int parse_args(const char *arg, size_t len, int *pid, unsigned long long *start, char *word, size_t wn) {
    char a[96];
    while (len && (arg[len - 1] == '\n' || arg[len - 1] == ' ')) len--;
    if (!len || len >= sizeof a || memchr(arg, 0, len) || memchr(arg, '\n', len)) return -1;
    memcpy(a, arg, len);
    a[len] = 0;
    char *f[3] = { 0 }, *save = NULL;
    int n = 0;
    for (char *t = strtok_r(a, " ", &save); t; t = strtok_r(NULL, " ", &save)) { if (n == 3) return -1; f[n++] = t; }
    if (n != (word ? 3 : 2)) return -1;
    for (int i = 0; i < 2; i++) {
        size_t l = strlen(f[i]);
        if (!l || l > 20 || (l > 1 && f[i][0] == '0')) return -1;
        for (size_t k = 0; k < l; k++) if (f[i][k] < '0' || f[i][k] > '9') return -1;
    }
    long long p = strtoll(f[0], NULL, 10);
    if (p < 1 || p > PROC_PID_MAX) return -1;
    *pid = (int)p;
    *start = strtoull(f[1], NULL, 10);
    if (word) snprintf(word, wn, "%s", f[2]);
    return 0;
}

void proc_act_signal(const AdminReq *r, const char *arg, size_t len) {
    (void)r;
    int pid;
    unsigned long long start;
    char w[8];
    if (parse_args(arg, len, &pid, &start, w, sizeof w) || (strcmp(w, "TERM") && strcmp(w, "KILL"))) {
        admin_err("admin.invalidArgument", NULL);
        return;
    }
    /* the helper's parent is the session's axia-server */
    const char *e = proc_signal(pid, start, !strcmp(w, "KILL") ? SIGKILL : SIGTERM, (int)getppid());
    if (e) admin_err(e, NULL);
    else admin_say("OK", "%d", pid);   /* i18n-ok */
}

#define CHUNK 900

void proc_act_info(const AdminReq *r, const char *arg, size_t len) {
    (void)r;
    int pid;
    unsigned long long start;
    if (parse_args(arg, len, &pid, &start, NULL, 0)) { admin_err("admin.invalidArgument", NULL); return; }
    Buf b = { 0 };
    proc_detail_json(&b, pid, start, 1, (int)getppid());
    if (b.bad || !b.p) { mon_buf_free(&b); admin_err("proc.failed", NULL); return; }
    int lines = 0;
    for (size_t i = 0; i < b.n && lines < 256;) {
        size_t n = b.n - i < CHUNK ? b.n - i : CHUNK;
        while (n < b.n - i && n > 1 && ((unsigned char)b.p[i + n] & 0xc0) == 0x80) n--;   /* not inside a UTF-8 sequence */
        admin_say("LOG", "%.*s", (int)n, b.p + i);   /* i18n-ok */
        i += n;
        lines++;
    }
    mon_buf_free(&b);
    admin_say("OK", "%d", lines);   /* i18n-ok */
}
