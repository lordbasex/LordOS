/* npdiag.h — axia-netperm's sock_diag client (NETLINK_SOCK_DIAG): lists the machine's TCP and
 * UDP sockets with their cgroup and destroys one (SOCK_DESTROY, the kernel's tcp_abort /
 * udp_abort: the app gets ECONNABORTED and TCP sends a reset). It is how the firewall cuts the
 * connections an app opened before a rule changed (docs/FIREWALL.md, "Existing connections are
 * cut"). Netlink only: no AF_INET/AF_INET6 socket is created. Destroying needs CAP_NET_ADMIN and a
 * kernel with CONFIG_INET_DIAG_DESTROY (UDP also CONFIG_INET_UDP_DIAG); without it the packet
 * filter's drop still applies and npdiag_destroy returns -EOPNOTSUPP. */
#ifndef LORD_NP_DIAG_H
#define LORD_NP_DIAG_H

#include <stdint.h>

typedef struct {
    int fam;                    /* AF_INET / AF_INET6 */
    int proto;                  /* IPPROTO_TCP / IPPROTO_UDP */
    int state;                  /* the kernel's state (TCP_ESTABLISHED = 1, TCP_LISTEN = 10) */
    uint8_t src[16], dst[16];   /* local and remote address, network order (IPv4 in the first 4) */
    uint16_t sport, dport;      /* local and remote port, host order */
    uint32_t ifindex;
    uint32_t cookie[2];         /* the socket's cookie, as sock_diag gives it */
    uint64_t cgid;              /* its cgroup v2 id (INET_DIAG_CGROUP_ID), 0 if not reported */
} NpSock;

/* A NETLINK_SOCK_DIAG socket (close-on-exec, 500 ms receive timeout), or -1. */
int npdiag_open(void);

/* Calls cb for every socket of that family and protocol whose state bit is in states (1 << state).
 * cb returns nonzero to stop. Returns how many sockets it saw, or -errno. */
int npdiag_each(int fd, int fam, int proto, uint32_t states, int (*cb)(void *u, const NpSock *s), void *u);

/* Destroys the socket with that tuple and cookie (a different socket that reused the tuple has
 * another cookie and is left alone). 0, or -errno (-ENOENT: already gone; -EOPNOTSUPP: the kernel
 * can't). */
int npdiag_destroy(int fd, const NpSock *s);

#endif
