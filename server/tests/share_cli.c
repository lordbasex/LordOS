/* share_cli.c — the share-* actions alone, as root on a test machine: real mounts through the real
 * code, without a session. A tool, not part of make test.
 *   share-cli mount <user> <uid> <gid>      the request on standard input (lan/lanaddr.h)
 *   share-cli unmount <user> <folder>                                                       */
#include "../src/lan/share_admin.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void admin_say(const char *kind, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("%s ", kind);
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

int main(int argc, char **argv) {
    AdminReq r = { 0 };
    if (argc >= 5 && !strcmp(argv[1], "mount")) {
        char buf[1024];
        size_t n = fread(buf, 1, sizeof buf - 1, stdin);
        r.user = argv[2];
        r.uid = (uid_t)atoi(argv[3]);
        r.gid = (gid_t)atoi(argv[4]);
        share_act_mount(&r, buf, n);
        explicit_bzero(buf, sizeof buf);
    } else if (argc == 4 && !strcmp(argv[1], "unmount")) {
        r.user = argv[2];
        share_act_unmount(&r, argv[3], strlen(argv[3]));
    } else return 2;
    return 0;
}
