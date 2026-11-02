/* npload.h — what lord-netperm loads into the kernel: its maps and which kind of program each
 * section of np.bpf.c is. Shared by netperm.c and tests/npverify_test.c (which loads every
 * program into the running kernel, so the verifier checks exactly what the service loads). */
#ifndef LORD_NP_LOAD_H
#define LORD_NP_LOAD_H

#include <linux/bpf.h>
#include <stdint.h>

#include "netperm/np_shared.h"

#define NP_MAX_SESS 64
#define NP_MAX_SND 1024                /* ALSA nodes in np_snd */

/* name, type, key size, value size, max entries, flags */
#define NP_MAPS(X) \
    X(np_cfg,    BPF_MAP_TYPE_ARRAY,    4,                 sizeof(NpCfg),   1,               0) \
    X(np_apps,   BPF_MAP_TYPE_HASH,     8,                 sizeof(NpApp),   NP_MAX_APPS,     0) \
    X(np_union,  BPF_MAP_TYPE_HASH,     8,                 sizeof(NpApp),   NP_MAX_SESS,     0) \
    X(np_lpm4,   BPF_MAP_TYPE_LPM_TRIE, sizeof(NpLpm4Key), sizeof(NpList),  NP_MAX_LPM,      BPF_F_NO_PREALLOC) \
    X(np_lpm6,   BPF_MAP_TYPE_LPM_TRIE, sizeof(NpLpm6Key), sizeof(NpList),  NP_MAX_LPM,      BPF_F_NO_PREALLOC) \
    X(np_seen,   BPF_MAP_TYPE_LRU_HASH, sizeof(NpSeenKey), 8,               NP_MAX_SEEN,     0) \
    X(np_counts, BPF_MAP_TYPE_HASH,     8,                 sizeof(NpCount), NP_MAX_APPS,     0) \
    X(np_events, BPF_MAP_TYPE_RINGBUF,  0,                 0,               NP_RINGBUF,      0) \
    X(np_snd,    BPF_MAP_TYPE_HASH,     4,                 4,               NP_MAX_SND,      0) \
    X(np_flows,  BPF_MAP_TYPE_LRU_HASH, sizeof(NpFlowKey), sizeof(NpFlow),  NP_MAX_FLOWS,    0) \
    X(np_bound,  BPF_MAP_TYPE_LRU_HASH, sizeof(NpBoundKey), 1,              NP_MAX_BOUND,    0) \
    X(np_blk4,   BPF_MAP_TYPE_LPM_TRIE, sizeof(NpLpm4Key), 1,               NP_MAX_BLK,      BPF_F_NO_PREALLOC) \
    X(np_blk6,   BPF_MAP_TYPE_LPM_TRIE, sizeof(NpLpm6Key), 1,               NP_MAX_BLK,      BPF_F_NO_PREALLOC) \
    X(np_sysc,   BPF_MAP_TYPE_HASH,     8,                 sizeof(NpCount), NP_MAX_SYS,      0)

/* Each program: its section, kind and attach point. btf: the kernel function an LSM program
 * hooks (its id comes from /sys/kernel/btf/vmlinux). opt: 1 = the device, Bluetooth and traffic
 * accounting programs; if the kernel refuses one (no bpf LSM, an old kernel) the network ones
 * still work. The accounting programs (BPF_CGROUP_INET_INGRESS / EGRESS) are attached only to
 * lordos.service, not to user@<uid>.service (np_attach_user). */
typedef struct {
    const char *sec;
    uint32_t type, attach;
    const char *btf;
    int opt;
} NpProgSpec;

static const NpProgSpec np_prog_specs[] = {
    { "cgroup/connect4", BPF_PROG_TYPE_CGROUP_SOCK_ADDR, BPF_CGROUP_INET4_CONNECT, NULL, 0 },
    { "cgroup/connect6", BPF_PROG_TYPE_CGROUP_SOCK_ADDR, BPF_CGROUP_INET6_CONNECT, NULL, 0 },
    { "cgroup/sendmsg4", BPF_PROG_TYPE_CGROUP_SOCK_ADDR, BPF_CGROUP_UDP4_SENDMSG, NULL, 0 },
    { "cgroup/sendmsg6", BPF_PROG_TYPE_CGROUP_SOCK_ADDR, BPF_CGROUP_UDP6_SENDMSG, NULL, 0 },
    { "cgroup/bind4", BPF_PROG_TYPE_CGROUP_SOCK_ADDR, BPF_CGROUP_INET4_BIND, NULL, 0 },
    { "cgroup/bind6", BPF_PROG_TYPE_CGROUP_SOCK_ADDR, BPF_CGROUP_INET6_BIND, NULL, 0 },
    { "cgroup/sock_create", BPF_PROG_TYPE_CGROUP_SOCK, BPF_CGROUP_INET_SOCK_CREATE, NULL, 0 },
    { "cgroup/dev", BPF_PROG_TYPE_CGROUP_DEVICE, BPF_CGROUP_DEVICE, NULL, 1 },
    { "cgroup/lsm_socket_create", BPF_PROG_TYPE_LSM, BPF_LSM_CGROUP, "bpf_lsm_socket_create", 1 },
    /* the packet filter: traffic accounting for the Monitor (bytes per app and per remote address)
     * and the cut of connections a rule now denies (it drops only those packets) */
    { "cgroup/skb_ingress", BPF_PROG_TYPE_CGROUP_SKB, BPF_CGROUP_INET_INGRESS, NULL, 1 },
    { "cgroup/skb_egress", BPF_PROG_TYPE_CGROUP_SKB, BPF_CGROUP_INET_EGRESS, NULL, 1 },
};
#define NP_NSPECS (int)(sizeof np_prog_specs / sizeof np_prog_specs[0])

static inline int np_attach_user(uint32_t attach) { return attach != BPF_CGROUP_INET_INGRESS && attach != BPF_CGROUP_INET_EGRESS; }
/* (0.8.5) Only the accounting programs go to the essential system services' cgroups. */
static inline int np_attach_system(uint32_t attach) { return !np_attach_user(attach); }

/* The license the kernel sees: LSM programs must declare a GPL-compatible one (see bpfsys.h). */
static inline const char *np_prog_license(const NpProgSpec *p) { return p->type == BPF_PROG_TYPE_LSM ? "Dual MIT/GPL" : "LordOS"; }

#endif
