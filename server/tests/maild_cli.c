/* maild_cli.c — drives axia-maild the way axia-server does (mailc.c), from a terminal, for
 * manual tests against local servers (docs/MAIL.md, "Testing"). Not part of `make test`.
 *
 *   build/maild-cli '{"op":"status"}' '{"op":"list","acct":"ana","folder":"INBOX"}' --wait 5
 * Each request waits for its answer; events are printed as they come. The accounts come from
 * ~/.config/axia/mail/accounts.json; a password can be given with {"op":"auth",…}. */
#include "mail/mailc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

static struct timespec t_start;
static double since_start(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (t.tv_sec - t_start.tv_sec) + (t.tv_nsec - t_start.tv_nsec) / 1e9; }

static int wait_for(int id, double seconds) {
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int got = 0;
    for (;;) {
        char *s = mail_poll();
        if (s) {
            printf("[%.2f s] %s\n", since_start(), s);
            char key[32];
            snprintf(key, sizeof key, "{\"id\":%d,", id);
            if (id > 0 && strstr(s, key)) got = 1;
            free(s);
            fflush(stdout);
        }
        if (got) return 0;
        clock_gettime(CLOCK_MONOTONIC, &t);
        if ((t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) / 1e9 > seconds) return id > 0 ? -1 : 0;
        usleep(20000);
    }
}

int main(int argc, char **argv) {
    clock_gettime(CLOCK_MONOTONIC, &t_start);
    if (mail_start()) { fprintf(stderr, "axia-maild did not start (is it next to this binary?)\n"); return 1; }
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--wait") && i + 1 < argc) { wait_for(0, atof(argv[++i])); continue; }
        if (!strcmp(argv[i], "--file") && i + 2 < argc) {
            /* --file <path> <json>: the request with one file attached */
            int fd = open(argv[i + 1], 0);
            int fds[1] = { fd };
            int id = mail_request(argv[i + 2], strlen(argv[i + 2]), fds, fd >= 0 ? 1 : 0);
            i += 2;
            if (wait_for(id, 60)) fprintf(stderr, "request %d: no answer\n", id);
            continue;
        }
        struct timespec a, b;
        clock_gettime(CLOCK_MONOTONIC, &a);
        int id = mail_request(argv[i], strlen(argv[i]), NULL, 0);
        if (id < 0) { fprintf(stderr, "request failed\n"); return 1; }
        if (wait_for(id, 120)) fprintf(stderr, "request %d: no answer\n", id);
        clock_gettime(CLOCK_MONOTONIC, &b);
        fprintf(stderr, "request %d: %.1f ms\n", id, (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6);
    }
    return 0;
}
