/* lordpgpv.c — lord-pgpv: checks the OpenPGP signature of a system the person added to Machines (Machines settings →
 * Systems; approved by the maintainer on 2026-11-22, AGENTS.md "lord-pgpv"; docs/VIRTUALIZATION.md 5.6.1).
 *
 * lord-server (ui/js_vm.c, lord.vm.sigCheck) starts it from the fixed path next to it, with no arguments and an empty
 * environment, and gives it:
 *   fd 0  one line "<40 hex digits> <unix time>\n": the fingerprint the system's definition names, and now
 *   fd 3  the system's public key (sealed memfd, armored or binary, at most 32 KB)
 *   fd 4  the checksum file (sealed memfd, at most 1 MB)
 *   fd 5  its detached signature (sealed memfd, at most 64 KB)
 *   fd 1  where the one answer line goes; fd 2 is /dev/null
 * It applies the helpers' sandbox (no network, no files, no exec, a syscall allowlist, 32 MB) before it reads a byte
 * of any of them, verifies with LordOS's own OpenPGP code (pkg/pgp.c, through vmsig.c) and prints exactly
 * "valid <fingerprint>\n" or "invalid <reason>\n". Nothing else: lord-server reads that line strictly (vmsig_reply)
 * and looks for the image's SHA-256 in the checksum file itself. */
#include "vmsig.h"
#include "../sec/sandbox.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <unistd.h>

/* all of fd, up to max bytes (more is refused: -1) */
static char *slurp(int fd, size_t max, size_t *len) {
    char *b = malloc(max + 1);
    size_t n = 0;
    if (!b) return NULL;
    for (;;) {
        if (n > max) { free(b); return NULL; }
        ssize_t r = read(fd, b + n, max + 1 - n);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) { free(b); return NULL; }
        if (r == 0) break;
        n += (size_t)r;
    }
    if (n > max) { free(b); return NULL; }
    b[n] = 0;
    *len = n;
    return b;
}

static int say(const char *line) {
    size_t n = strlen(line);
    for (size_t o = 0; o < n;) {
        ssize_t w = write(1, line + o, n - o);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return 4;
        o += (size_t)w;
    }
    return 0;
}

int main(int argc, char **argv) {
    (void)argv;
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    prctl(PR_SET_DUMPABLE, 0);
    struct rlimit nocore = { 0, 0 };
    setrlimit(RLIMIT_CORE, &nocore);
    alarm(10);
    if (argc != 1) return 2;
    SandboxHelperCfg cfg = {
        .name = "lordos-pgpv",
        .ro_paths = NULL,
        .extra_syscalls = (int[]){ -1 },
        .allow_ioctl_classes = 0,
        .mem_max_mb = 32,
        .ro_dir_fds = NULL,
        .rw_dir_fds = NULL,
    };
    sandbox_apply_helper(&cfg);

    char req[80];
    size_t rn = 0;
    while (rn < sizeof req - 1) {
        ssize_t r = read(0, req + rn, 1);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0 || req[rn] == '\n') break;
        rn++;
    }
    req[rn] = 0;
    char fpr[41] = "";
    long long now = 0;
    if (rn < 42 || req[40] != ' ' || sscanf(req + 41, "%lld", &now) != 1 || now <= 0) return say("invalid malformed\n") ? 4 : 0;
    memcpy(fpr, req, 40);
    fpr[40] = 0;

    size_t kl = 0, sl = 0, gl = 0;
    char *key = slurp(3, VMSIG_KEY_MAX, &kl), *sums = slurp(4, VMSIG_SUMS_MAX, &sl), *sig = slurp(5, VMSIG_SIG_MAX, &gl);
    char out[96], got[41], why[32];
    if (!key || !sums || !sig) vmsig_answer(0, "tooLarge", out, sizeof out);
    else {
        int ok = vmsig_verify(key, kl, sums, sl, sig, gl, fpr, (int64_t)now, got, why, sizeof why);
        vmsig_answer(ok, ok ? got : why, out, sizeof out);
    }
    free(key); free(sums); free(sig);
    return say(out);
}
