/* netconn.h — sockets for LordOS's own components, created in their own scopes (0.8.5).
 *
 * lord-server opens connections for several of its parts: Mail and the calendar sync (for
 * lord-maild, which has no sockets of its own), printers and scanners, and the blocklist
 * downloads. The kernel ties a socket to the cgroup of the process that created it, so each of
 * those parts gets a small connector process of its own: lord-server re-executed as
 * `lord-server --net-connector <component>` (component from a fixed list, fw_component_ok), which
 * asks lord-sessiond to place it in lordos-net-<component>.scope and then only creates sockets on
 * request. lord-netperm sees that scope as the firewall app "lordos:<component>": its rules, its
 * traffic and its row in Monitor → Network are its own, enforced by the kernel.
 *
 * The connector only does what lord-server could do itself (same user): socket(), and connect()
 * to the address lord-server already chose (non-blocking: lord-server waits for the result). It
 * takes no paths or names, runs nothing, and its request is a fixed-size struct validated on its
 * side too. In a LordOS session a connector that can't be placed in its scope exits, and the
 * request fails (fail closed: otherwise the traffic would count, and be decided, as lord-server's
 * own). Outside a LordOS session (no LORDOS_SESSIOND: development, the tests) there are no scopes,
 * and netconn_socket() makes the socket in lord-server itself, as before. */
#ifndef LORD_SEC_NETCONN_H
#define LORD_SEC_NETCONN_H

#include <sys/socket.h>

typedef enum { NC_MAIL = 0, NC_PRINT = 1, NC_LISTS = 2, NC_LAN = 3, NC_COUNT } NcComponent;

/* The connector's name ("mail", "print", "lists", "lan"), the same as its firewall id after "lordos:". */
const char *netconn_name(NcComponent c);

/* A new non-blocking, close-on-exec socket of family fam (AF_INET / AF_INET6) and type
 * (SOCK_STREAM / SOCK_DGRAM), created by comp's connector. With to, the connector also called
 * connect() (EINPROGRESS for TCP): the caller polls for POLLOUT and reads SO_ERROR as usual; a
 * connection the firewall denies fails with EPERM. Returns the fd, or -1 with errno set.
 * Thread-safe. */
int netconn_socket(NcComponent comp, int fam, int type, const struct sockaddr *to, socklen_t tolen);

/* `lord-server --net-connector <component>`: the connector process (main.c). Never returns
 * normally. */
int netconn_main(int argc, char **argv);

/* ---- the request (exposed for tests/netconn_test.c) ---- */
#define NC_MAGIC 0x4c4e4331u                /* "LNC1" */
typedef struct {
    unsigned magic;
    unsigned char fam;                      /* 4 or 6 */
    unsigned char type;                     /* 1 stream, 2 datagram */
    unsigned char connect;                  /* 1: connect to addr:port */
    unsigned char pad;
    unsigned short port;                    /* host order */
    unsigned short pad2;
    unsigned char addr[16];
    unsigned scope_id;                      /* IPv6 link-local */
} NcReq;

/* 0 if a connector may act on it (checked again on the connector's side). */
int netconn_req_ok(const NcReq *r);
/* Builds the request from a sockaddr; 0 ok. */
int netconn_req_make(NcReq *r, int fam, int type, const struct sockaddr *to, socklen_t tolen);
/* The connector's side: does one request (socket, connect) and returns the fd or -errno. */
int netconn_serve_one(const NcReq *r);

#endif
