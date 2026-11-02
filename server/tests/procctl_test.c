/* procctl_test.c — the Monitor's process actions (src/monitor/procctl.c): stat parsing, the
 * protected list, a signal that refuses a stale start time (the PID-reuse guard), TERM and KILL on
 * real children, and the Get Info JSON (memory, a pipe, a listening TCP socket). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../src/monitor/procctl.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); } } while (0)
#define STREQ(a, b) ((a) && (b) && !strcmp((a), (b)))

static pid_t child(void) {
    pid_t p = fork();
    if (p == 0) { for (;;) pause(); }
    struct timespec ts = { 0, 20 * 1000 * 1000 };
    nanosleep(&ts, NULL);
    return p;
}

int main(void) {
    /* stat with a hostile name: spaces and parentheses inside comm */
    ProcStat s;
    CHECK(!proc_stat_parse("4242 (a) b (c) S 1 4242 4242 0 -1 4194560 10 0 2 0 7 3 0 0 20 0 5 0 123456 1000000 250 18446744073709551615", &s));
    CHECK(s.pid == 4242 && STREQ(s.comm, "a) b (c") && s.state == 'S' && s.ppid == 1 && s.pgid == 4242);
    CHECK(s.minflt == 10 && s.majflt == 2 && s.utime == 7 && s.stime == 3 && s.prio == 20 && s.threads == 5);
    CHECK(s.start == 123456 && s.vsize == 1000000 && s.rss == 250);
    CHECK(proc_stat_parse("garbage", &s) == -1);
    CHECK(proc_stat_parse("12 (x) S 1 2", &s) == -1);

    /* the protected list */
    CHECK(STREQ(proc_protect_reason(1, 0, "systemd", "/usr/lib/systemd/systemd", NULL, 999), "init"));
    CHECK(STREQ(proc_protect_reason(2, 0x00200000, "kthreadd", "", NULL, 999), "kernel"));
    CHECK(STREQ(proc_protect_reason(77, 0x00200000, "kworker/0:1", "", NULL, 999), "kernel"));
    CHECK(STREQ(proc_protect_reason(999, 0, "axia-server", "/usr/lib/lordos/bin/axia-server", NULL, 999), "session"));
    CHECK(STREQ(proc_protect_reason(500, 0, "axia-server", "/usr/lib/lordos/bin/axia-server", "--term-holder", 999), "system"));
    CHECK(proc_protect_reason(501, 0, "axia-server", "/usr/lib/lordos/bin/axia-server", "--app", 999) == NULL);
    const char *sys[] = { "lord-sessiond", "axia-netperm", "lord-netd", "axia-fand", "axia-vault", "axia-maild", "axia-doc", "axia-media",
                          "lord-admin", "pipewire", "pipewire-pulse", "wireplumber" };
    for (size_t i = 0; i < sizeof sys / sizeof sys[0]; i++) CHECK(STREQ(proc_protect_reason(600, 0, sys[i], "", NULL, 999), "system"));
    CHECK(STREQ(proc_protect_reason(601, 0, "x", "/usr/bin/pipewire (deleted)", NULL, 999), "system"));
    CHECK(proc_protect_reason(602, 0, "firefox-esr", "/usr/lib/firefox-esr/firefox-esr", NULL, 999) == NULL);
    CHECK(proc_protect_reason(603, 0, "lord-serverx", "", NULL, 999) == NULL);

    /* signals: a stale start time is refused and the process survives */
    pid_t a = child();
    ProcStat cs;
    CHECK(!proc_stat_read(a, &cs));
    CHECK(STREQ(proc_signal(a, cs.start + 1, SIGTERM, getpid()), "proc.gone"));
    CHECK(kill(a, 0) == 0);
    CHECK(STREQ(proc_signal(a, cs.start, SIGHUP, getpid()), "proc.badArgument"));
    CHECK(proc_signal(a, cs.start, SIGTERM, getpid()) == NULL);
    int st = 0;
    CHECK(waitpid(a, &st, 0) == a && WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM);
    /* the same pid again after it is gone (the reuse case: no process, or another one) */
    const char *again = proc_signal(a, cs.start, SIGKILL, getpid());
    CHECK(STREQ(again, "proc.gone"));
    pid_t b = child();
    CHECK(!proc_stat_read(b, &cs));
    CHECK(proc_signal(b, cs.start, SIGKILL, getpid()) == NULL);
    CHECK(waitpid(b, &st, 0) == b && WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL);
    /* the session itself and PID 1 */
    ProcStat me;
    CHECK(!proc_stat_read(getpid(), &me));
    CHECK(STREQ(proc_signal(getpid(), me.start, SIGTERM, getpid()), "proc.protectedSession"));
    CHECK(STREQ(proc_signal(1, 0, SIGTERM, getpid()), "proc.protectedInit"));
    CHECK(STREQ(proc_signal(0, 0, SIGTERM, getpid()), "proc.badArgument"));
    CHECK(STREQ(proc_signal(PROC_PID_MAX + 1, 0, SIGTERM, getpid()), "proc.badArgument"));

    /* Get Info: a pipe and a listening TCP socket show up among the open files */
    int pp[2];
    CHECK(!pipe(pp));
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    CHECK(ls >= 0 && !bind(ls, (struct sockaddr *)&sa, sizeof sa) && !listen(ls, 1));
    Buf j = { 0 };
    proc_detail_json(&j, getpid(), me.start, 1, 1);
    CHECK(j.p && strstr(j.p, "\"gone\":false"));
    CHECK(j.p && strstr(j.p, "\"k\":\"pipe\""));
    CHECK(j.p && strstr(j.p, "\"proto\":\"TCP\"") && strstr(j.p, "\"local\":\"127.0.0.1:") && strstr(j.p, "\"state\":\"LISTEN\""));
    CHECK(j.p && strstr(j.p, "\"exe\":\"/") && strstr(j.p, "\"cmdline\":[\""));
    CHECK(j.p && strstr(j.p, "\"private\":") && !strstr(j.p, "\"private\":null"));
    mon_buf_free(&j);
    proc_detail_json(&j, getpid(), me.start, 0, 1);              /* not the owner: no private parts */
    CHECK(j.p && strstr(j.p, "\"fds\":null") && strstr(j.p, "\"exe\":null") && strstr(j.p, "\"private\":null"));
    mon_buf_free(&j);
    proc_detail_json(&j, getpid(), me.start + 7, 1, 1);          /* another start time: gone */
    CHECK(j.p && strstr(j.p, "\"gone\":true") && !strstr(j.p, "\"fds\""));
    mon_buf_free(&j);

    printf("procctl-test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
