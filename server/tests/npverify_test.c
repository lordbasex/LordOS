/* npverify_test.c — axia-netperm's BPF programs against the real kernel verifier, and the BTF
 * reader (src/netperm/bpfsys.c) it uses to find the LSM hook.
 *
 *   build/npverify-test            (in `make test`; as root it also loads every program)
 *
 * Without root: only the BTF reader, on blobs built here (good, truncated, unknown kind). As root
 * (sudo, in the test VM): creates the same maps as axia-netperm (npload.h), loads every program
 * of build/gen/np_bpf.h with the kind and attach point the service uses, and fails with the
 * verifier's log if the kernel refuses one; then finds bpf_lsm_socket_create in the running
 * kernel's BTF. Nothing is attached: no cgroup, process or device is touched. */
#include "netperm/bpfsys.h"
#include "netperm/npload.h"
#include "np_bpf.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ---- a small BTF blob: INT "int", FUNC_PROTO, FUNC "my_hook", STRUCT with one member ---- */
static size_t put32(unsigned char *b, size_t o, uint32_t v) { memcpy(b + o, &v, 4); return o + 4; }

static size_t make_btf(unsigned char *b, int bad_kind, int truncate) {
    static const char strs[] = "\0int\0my_hook\0s\0m";      /* offsets: int=1 my_hook=5 s=13 m=15 */
    unsigned char t[256];
    size_t o = 0;
    o = put32(t, o, 1); o = put32(t, o, 1u << 24); o = put32(t, o, 4); o = put32(t, o, 32);          /* [1] INT int */
    o = put32(t, o, 0); o = put32(t, o, 13u << 24 | 1); o = put32(t, o, 1); o = put32(t, o, 0); o = put32(t, o, 1);   /* [2] FUNC_PROTO (int) */
    o = put32(t, o, 13); o = put32(t, o, 4u << 24 | 1); o = put32(t, o, 4);                           /* [3] STRUCT s { int m; } */
    o = put32(t, o, 15); o = put32(t, o, 1); o = put32(t, o, 0);
    if (bad_kind) { o = put32(t, o, 0); o = put32(t, o, 30u << 24); o = put32(t, o, 0); }            /* an unknown kind */
    o = put32(t, o, 5); o = put32(t, o, 12u << 24); o = put32(t, o, 2);                               /* FUNC my_hook */
    size_t n = 0;
    b[n++] = 0x9f; b[n++] = 0xeb; b[n++] = 1; b[n++] = 0;
    n = put32(b, n, 24); n = put32(b, n, 0); n = put32(b, n, (uint32_t)o); n = put32(b, n, (uint32_t)o); n = put32(b, n, sizeof strs);
    memcpy(b + n, t, o); n += o;
    memcpy(b + n, strs, sizeof strs); n += sizeof strs;
    return truncate ? n - 20 : n;
}

static uint32_t btf_lookup(const unsigned char *b, size_t n, const char *name) {
    char path[] = "/tmp/npverify-btf-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return 0;
    if (write(fd, b, n) != (ssize_t)n) { close(fd); unlink(path); return 0; }
    close(fd);
    uint32_t id = bpfs_btf_func_id(path, name);
    unlink(path);
    return id;
}

static void btf_reader(void) {
    unsigned char b[512];
    size_t n = make_btf(b, 0, 0);
    CHECK(btf_lookup(b, n, "my_hook") == 4);                 /* ids start at 1: the FUNC is the 4th type */
    CHECK(btf_lookup(b, n, "int") == 0);                     /* an INT, not a function */
    CHECK(btf_lookup(b, n, "my_hoo") == 0);
    CHECK(btf_lookup(b, n, "my_hook_x") == 0);
    n = make_btf(b, 1, 0);
    CHECK(btf_lookup(b, n, "my_hook") == 0);                 /* unknown kind before it: gives up */
    n = make_btf(b, 0, 1);
    CHECK(btf_lookup(b, n, "my_hook") == 0);                 /* sections past the end of the file */
    n = make_btf(b, 0, 0);
    b[1] = 0x12;
    CHECK(btf_lookup(b, n, "my_hook") == 0);                 /* bad magic */
    CHECK(bpfs_btf_func_id("/nonexistent/btf", "my_hook") == 0);
}

/* ---- the kernel verifier ---- */
static int maps[16];
static const char *map_names[16];
static int nmaps;
static int map_fd(const char *name) {
    for (int i = 0; i < nmaps; i++) if (!strcmp(map_names[i], name)) return maps[i];
    return -1;
}

static void kernel_verifier(void) {
#define NP_MKMAP(n, t, k, v, m, f) map_names[nmaps] = #n; maps[nmaps] = bpfs_map_create(t, k, v, m, f, #n); CHECK(maps[nmaps] >= 0); nmaps++;
    NP_MAPS(NP_MKMAP)
#undef NP_MKMAP
    static char log[1 << 20];
    CHECK(NP_NPROGS == NP_NSPECS);                           /* np.bpf.c and the table agree */
    for (int w = 0; w < NP_NSPECS; w++) {
        const NpProgSpec *p = &np_prog_specs[w];
        const NpProgDesc *d = NULL;
        for (int i = 0; i < NP_NPROGS; i++) if (!strcmp(np_progs[i].section, p->sec)) d = &np_progs[i];
        CHECK(d != NULL);
        if (!d) continue;
        uint32_t btf = 0;
        if (p->btf) {
            btf = bpfs_btf_func_id(NULL, p->btf);
            CHECK(btf != 0);
            if (!btf) { fprintf(stderr, "  %s: %s is not in the kernel's BTF\n", p->sec, p->btf); continue; }
        }
        int fd = bpfs_prog_load_btf(p->type, p->attach, btf, np_prog_license(p), d, map_fd, log, sizeof log);
        if (fd < 0) {
            size_t ll = strlen(log);
            fprintf(stderr, "  %s refused (%s):\n%s\n", p->sec, strerror(errno), ll > 4000 ? log + ll - 4000 : log);
        } else printf("npverify-test: %-28s verified (%u instructions)\n", p->sec, d->ninsns);
        CHECK(fd >= 0);
        if (fd >= 0) close(fd);
    }
}

int main(void) {
    btf_reader();
    if (geteuid() == 0) kernel_verifier();
    else printf("npverify-test: kernel verifier skipped: needs root (sudo build/npverify-test)\n");
    printf("npverify-test: %d/%d passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
