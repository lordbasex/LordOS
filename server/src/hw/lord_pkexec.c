/* lord_pkexec.c — /usr/bin/pkexec on LordOS (a link; the real one is /usr/bin/pkexec.distrib, by
 * dpkg-divert). It has no privilege and sees no secret: it asks the session's agent to answer for the
 * process that called it (hw/pkagent.c says why) and becomes the real pkexec, same pid, arguments and
 * environment. If anything fails it still does, and pkexec reports what it always did. From a terminal
 * it asks nothing: pkexec's own text prompt works there. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#define REAL "/usr/bin/pkexec.distrib"

static void ask_agent(void) {
    int tty = open("/dev/tty", O_RDONLY | O_CLOEXEC | O_NOCTTY);
    if (tty >= 0) { close(tty); return; }
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    const char *rt = getenv("XDG_RUNTIME_DIR");
    int n = rt && *rt ? snprintf(a.sun_path, sizeof a.sun_path, "%s/lordos-pkagent.sock", rt)
                      : snprintf(a.sun_path, sizeof a.sun_path, "/run/user/%u/lordos-pkagent.sock", (unsigned)getuid());
    if (n < 0 || (size_t)n >= sizeof a.sun_path) return;
    int s = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (s < 0) return;
    struct timeval tv = { 3, 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    if (!connect(s, (struct sockaddr *)&a, sizeof a)) {
        /* pkexec's subject is its parent, or itself when that parent was init */
        pid_t ppid = getppid();
        char m[24], r[8];
        int l = snprintf(m, sizeof m, "%d", ppid == 1 ? getpid() : ppid);
        if (send(s, m, (size_t)l, MSG_NOSIGNAL) == l) (void)!recv(s, r, sizeof r, 0);
    }
    close(s);
}

int main(int argc, char **argv) {
    (void)argc;
    ask_agent();
    execv(REAL, argv);
    perror("pkexec");
    return 127;
}
