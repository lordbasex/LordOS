/* fetchspawn.c — lord-fetch for lord-server's workers (see fetchspawn.h). */
#include "fetchspawn.h"
#include "fetchproto.h"
#include "../doc/wire.h"
#include "../sec/netconn.h"

#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int fetch_helper_start(const char *bin, FetchHelper *h) {
    struct stat st, me;
    if (!bin || stat(bin, &st) || !S_ISREG(st.st_mode) || stat("/proc/self/exe", &me) ||
        (st.st_uid != 0 && st.st_uid != me.st_uid) || (st.st_mode & 022)) return -1;   /* root's or ours, not writable by others */
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) return -1;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, sv[1], 3);
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    posix_spawnattr_setsigmask(&at, &none);
    posix_spawnattr_setsigdefault(&at, &all);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    char *argv[] = { "lord-fetch", NULL };
    char *envp[] = { "LANG=C.UTF-8", getenv("LORDOS_SESSIOND") ? "LORDOS_SESSIOND=1" : NULL, NULL };
    int e = posix_spawn(&h->pid, bin, &fa, &at, argv, envp);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    close(sv[1]);
    if (e) { close(sv[0]); h->fd = -1; return -1; }
    h->fd = sv[0];
    return 0;
}

int fetch_connect(int comp, const char *host) {
    struct addrinfo hints = { 0 }, *res = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_ADDRCONFIG;
    if (getaddrinfo(host, "443", &hints, &res)) return -2;
    int fd = -1;
    for (struct addrinfo *a = res; a && fd < 0; a = a->ai_next) {
        fd = netconn_socket((NcComponent)comp, a->ai_family, SOCK_STREAM, a->ai_addr, a->ai_addrlen);
        if (fd < 0) continue;
        struct pollfd p = { fd, POLLOUT, 0 };
        int err = 0;
        socklen_t el = sizeof err;
        if (poll(&p, 1, 15000) != 1 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) || err) { close(fd); fd = -1; }
    }
    freeaddrinfo(res);
    if (fd >= 0) fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
    return fd;
}

void fetch_helper_stop(FetchHelper *h) {
    if (h->fd < 0) return;
    doc_send(h->fd, FQ_END, 0, "{}", 2, NULL, 0);
    close(h->fd);
    h->fd = -1;
    for (int i = 0; i < 50; i++) {
        if (waitpid(h->pid, NULL, WNOHANG) == h->pid) return;
        struct timespec ts = { 0, 20 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    kill(h->pid, SIGKILL);
    waitpid(h->pid, NULL, 0);
}
