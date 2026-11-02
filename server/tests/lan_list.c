/* lan_list.c — axia-lan's sign-in against one test server, a tool, not part of make test:
 *   LAN_PW=... build/lan-list 127.0.0.1 4445 user [domain]
 * The password comes from the environment so it never is on a command line. */
#include "../src/doc/wire.h"
#include "../src/lan/lanproto.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 4) return 2;
    int sv[2];
    socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv);
    if (!fork()) { dup2(sv[1], 3); execl("build/axia-lan", "axia-lan", (char *)NULL); _exit(9); }
    close(sv[1]);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)atoi(argv[2])) };
    inet_pton(AF_INET, argv[1], &a.sin_addr);
    if (connect(fd, (struct sockaddr *)&a, sizeof a)) { perror("connect"); return 1; }
    JBuf b;
    jb_init(&b);
    jb_raw(&b, "{\"addr\":"); jb_str(&b, argv[1]);
    jb_raw(&b, ",\"user\":"); jb_str(&b, argv[3]);
    jb_raw(&b, ",\"domain\":"); jb_str(&b, argc > 4 ? argv[4] : "");
    jb_raw(&b, ",\"password\":"); jb_str(&b, getenv("LAN_PW") ? getenv("LAN_PW") : "");
    jb_raw(&b, "}");
    doc_send(sv[0], LQ_SMB_AUTH, 1, b.p, b.n, &fd, 1);
    DocMsg m;
    if (doc_recv(sv[0], &m)) { puts("no reply"); return 1; }
    printf("op %u: %s\n", m.h.op, m.json ? m.json : "");
    doc_send(sv[0], LQ_END, 0, "{}", 2, NULL, 0);
    return m.h.op == LR_SMB ? 0 : 1;
}
