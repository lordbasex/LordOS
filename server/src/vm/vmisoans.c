/* vmisoans.c — the small side of the ISO probe (vmiso.h) that lord-server keeps: the probe's fixed answer, written by
 * lord-vm --iso-probe and decoded here, and the architecture it names. The parser itself (vmiso.c) runs only in lord-vm,
 * out of lord-server, under seccomp strict mode: lord-server never links it. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "vmiso.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

const char *vmiso_arch(const VmIso *v) {
    unsigned m = v->efi_pe ? v->efi_pe : v->efi_named;
    int x86 = (m & (VMISO_X64 | VMISO_IA32)) != 0, arm = (m & VMISO_AA64) != 0, rv = (m & VMISO_RISCV64) != 0;
    if (x86 + arm + rv > 1) return "";
    if (arm) return "arm64";
    if (x86) return "amd64";
    if (rv) return "riscv64";
    return v->bios ? "amd64" : "";
}

/* ---- the answer of lord-vm's probe (vmiso.h) ---- */

static void put32(uint8_t *p, uint32_t x) { p[0] = (uint8_t)x; p[1] = (uint8_t)(x >> 8); p[2] = (uint8_t)(x >> 16); p[3] = (uint8_t)(x >> 24); }

void vmiso_answer_encode(int rc, const VmIso *v, uint8_t out[VMISO_ANSWER]) {
    memset(out, 0, VMISO_ANSWER);
    memcpy(out, VMISO_MAGIC, 8);
    uint32_t f = rc ? 0 : (VMISO_A_OK | (v->iso9660 ? VMISO_A_ISO : 0) | (v->udf ? VMISO_A_UDF : 0) | (v->bios ? VMISO_A_BIOS : 0) |
                           (v->efi ? VMISO_A_EFI : 0) | (v->windows ? VMISO_A_WINDOWS : 0));
    put32(out + 8, f);
    put32(out + 12, rc ? 0 : v->efi_named & VMISO_ARCH_ALL);
    put32(out + 16, rc ? 0 : v->efi_pe & VMISO_ARCH_ALL);
    put32(out + 20, v->reads);
    if (!rc) for (int i = 0; i < 32 && v->label[i]; i++) out[24 + i] = (uint8_t)v->label[i];
}

static int decode(const uint8_t *in, size_t n, VmIso *v);
int vmiso_answer_decode(const uint8_t *in, size_t n, VmIso *v) {
    int r = decode(in, n, v);
    if (r < 0) memset(v, 0, sizeof *v);          /* refused: nothing of it is kept */
    return r;
}
static int decode(const uint8_t *in, size_t n, VmIso *v) {
    memset(v, 0, sizeof *v);
    if (!in || n != VMISO_ANSWER || memcmp(in, VMISO_MAGIC, 8)) return -1;
    uint32_t f = le32(in + 8), named = le32(in + 12), pe = le32(in + 16);
    /* nothing beyond what the format holds: unknown bits, a label not printable ASCII up to its first zero, or bytes after
     * it, and the answer is refused whole */
    if ((f & ~(uint32_t)VMISO_A_ALL) || (named & ~(uint32_t)VMISO_ARCH_ALL) || (pe & ~(uint32_t)VMISO_ARCH_ALL)) return -1;
    int end = 0;
    for (int i = 0; i < 40; i++) {
        uint8_t c = in[24 + i];
        if (i >= 32 && c) return -1;
        if (!c) { end = 1; continue; }
        if (end || c < 0x20 || c > 0x7e) return -1;
        v->label[i] = (char)c;
    }
    if (!(f & VMISO_A_OK)) { if (f || named || pe || v->label[0]) return -1; return 1; }
    v->iso9660 = !!(f & VMISO_A_ISO);
    v->udf = !!(f & VMISO_A_UDF);
    v->bios = !!(f & VMISO_A_BIOS);
    v->efi = !!(f & VMISO_A_EFI);
    v->windows = !!(f & VMISO_A_WINDOWS);
    v->efi_named = named;
    v->efi_pe = pe;
    v->reads = le32(in + 20);
    return 0;
}

int vmiso_run(const char *lordvm, int fd, unsigned max_reads, int timeout_ms, VmIso *v) {
    memset(v, 0, sizeof *v);
    if (!lordvm || lordvm[0] != '/' || fd < 0 || max_reads > VMISO_READS) return -1;
    int p[2];
    if (pipe2(p, O_CLOEXEC)) return -1;
    char mx[16];
    snprintf(mx, sizeof mx, "%u", max_reads);
    pid_t pid = fork();
    if (pid == 0) {
        /* fd 0 the ISO, fd 1 the answer; nothing else crosses */
        int a = fcntl(fd, F_DUPFD_CLOEXEC, 10), b = fcntl(p[1], F_DUPFD_CLOEXEC, 10);
        if (a < 0 || b < 0 || dup2(a, 0) != 0 || dup2(b, 1) != 1) _exit(126);
        int nul = open("/dev/null", O_WRONLY);
        if (nul >= 0 && nul != 2) dup2(nul, 2);
        syscall(SYS_close_range, 3, ~0u, 0);
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        char *args[] = { "lord-vm", "--iso-probe", mx, NULL };
        char *env[] = { NULL };
        execve(lordvm, args, env);
        _exit(127);
    }
    close(p[1]);
    if (pid < 0) { close(p[0]); return -1; }
    uint8_t buf[VMISO_ANSWER + 1];
    size_t got = 0;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &t1);
        int left = timeout_ms - (int)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000);
        struct pollfd pf = { p[0], POLLIN, 0 };
        if (left <= 0) break;
        int pr = poll(&pf, 1, left);
        if (pr < 0 && errno == EINTR) continue;
        if (pr <= 0) break;
        ssize_t r = read(p[0], buf + got, sizeof buf - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        got += (size_t)r;
        if (got == sizeof buf) break;           /* more than an answer: refused below */
    }
    close(p[0]);
    kill(pid, SIGKILL);                         /* done, or out of time: it never outlives the call */
    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) ;
    return vmiso_answer_decode(buf, got, v);
}
