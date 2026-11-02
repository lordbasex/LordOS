/* facts.h — what System Information's Checks tab reads about the running session (docs/BENCHMARK.md,
 * "Checks: configured versus effective"), without root: lord-server's own ids from /proc/self/status,
 * its cgroup and who owns it, the app scopes beside it, and whether anything listens on a TCP port
 * (/proc/net/tcp and tcp6, which list every socket of the network namespace). `root` prefixes every
 * path ("" for this machine), as in sysreport.h. */
#ifndef LORD_SYSREPORT_FACTS_H
#define LORD_SYSREPORT_FACTS_H

typedef struct {
    int uid, euid;                      /* -1 unknown */
    char cgroup[256];                   /* the unified hierarchy path, "" unknown */
    int leaf_root, parent_root;         /* the cgroup and its parent are root-owned (-1 unknown) */
    int app_scopes;                     /* app-*.scope folders beside the leaf */
} SrFacts;

void sr_facts(SrFacts *f, const char *root);

/* 1 if a socket listens on the TCP port (IPv4 or IPv6), 0 if none, -1 if /proc/net can't be read. */
int sr_tcp_listening(const char *root, unsigned port);

#endif
