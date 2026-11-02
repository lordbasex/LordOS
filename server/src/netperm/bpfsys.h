/* bpfsys.h — the bare minimum of the bpf() syscall for axia-netperm: maps, loading programs,
 * attaching them to a cgroup with a bpf_link and reading a ring buffer (BPF_MAP_TYPE_RINGBUF).
 * No libbpf or bcc.
 *
 * The programs come precompiled inside the binary (build/gen/np_bpf.h, which bpf2c builds from
 * clang's .o): a program is never read from a file at run time. */
#ifndef LORD_NP_BPFSYS_H
#define LORD_NP_BPFSYS_H

#include <stddef.h>
#include <stdint.h>

/* What build/bpf2c generates (see np_bpf.h). */
typedef struct {
    uint32_t insn;              /* ld_imm64 instruction that carries the map fd */
    const char *map;            /* map name (np_apps…) */
} NpReloc;

typedef struct {
    const char *section;        /* "cgroup/connect4"… */
    const char *name;           /* function name */
    const uint64_t *insns;
    uint32_t ninsns;
    const NpReloc *relocs;
    uint32_t nrelocs;
} NpProgDesc;

int  bpfs_map_create(uint32_t type, uint32_t key, uint32_t val, uint32_t max, uint32_t flags, const char *name);
int  bpfs_map_update(int fd, const void *key, const void *val, uint64_t flags);
int  bpfs_map_lookup(int fd, const void *key, void *val);
int  bpfs_map_delete(int fd, const void *key);
/* Reads up to max entries of a hash map (keys of ksz bytes into keys, values of vsz into vals),
 * in one BPF_MAP_LOOKUP_BATCH call when the kernel has it (5.6+), otherwise key by key. Returns how
 * many it read, or -1. A map that changes meanwhile may give an entry twice or miss a new one. */
int  bpfs_map_dump(int fd, void *keys, uint32_t ksz, void *vals, uint32_t vsz, uint32_t max);

/* Loads a program with its relocations resolved (map_fd(name) gives the fd, -1 if it doesn't
 * exist). log (optional) receives the verifier log if it fails. Returns the fd or -1. */
int  bpfs_prog_load(uint32_t type, uint32_t attach, const NpProgDesc *d, int (*map_fd)(const char *), char *log, size_t loglen);
/* The same with the attach target's BTF id (LSM programs: the kernel function of the hook) and
 * the license string. The kernel only loads LSM programs with a GPL-compatible license: the
 * programs are ours (MIT), and MIT code may also be distributed under the GPL, so those go as
 * "Dual MIT/GPL"; the others as "LordOS". */
int  bpfs_prog_load_btf(uint32_t type, uint32_t attach, uint32_t btf_id, const char *license, const NpProgDesc *d,
                        int (*map_fd)(const char *), char *log, size_t loglen);
/* Id of a function (BTF_KIND_FUNC) in the kernel's own type information (/sys/kernel/btf/vmlinux,
 * or the file given, for tests), 0 if it isn't there or the file is malformed. Only reads; checks
 * every offset. */
uint32_t bpfs_btf_func_id(const char *path, const char *name);
int  bpfs_link_create(int prog_fd, int target_fd, uint32_t attach);

/* Ring buffer: mmap of the consumer page (rw) and of the producer + data (ro). */
typedef struct {
    int fd;
    size_t size, page;
    void *cons, *prod;
} BpfRing;

int  bpfs_ring_open(BpfRing *r, int map_fd, size_t size);
/* Calls cb for each ready record; returns how many it read. */
int  bpfs_ring_drain(BpfRing *r, void (*cb)(void *u, const void *data, uint32_t len), void *u);
void bpfs_ring_close(BpfRing *r);

/* cgroup v2 id of a folder (the one bpf_get_current_cgroup_id gives), or 0. */
uint64_t bpfs_cgroup_id(int dirfd, const char *name);

#endif
