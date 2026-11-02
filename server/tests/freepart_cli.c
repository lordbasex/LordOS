/* freepart_cli.c — the real partitioning step of "install in free space" (src/install/freepart.c) on
 * a loop device, for manual testing (root; not part of `make test`, never installed; tests/freepart_loop.sh).
 *
 *   build/freepart-cli probe  <loopN> [--uefi]
 *   build/freepart-cli create <loopN> [--uefi] [--start S --end E] [--fail N] [--undo]
 *
 *   --uefi     act as if booted through UEFI with efibootmgr present (a test machine may not be)
 *   --fail N   1: fail after sgdisk wrote; 2: fail instead of mkfs; 3: make sgdisk itself refuse
 *   --undo     after success, undo as a failed copy or grub step would
 *
 * Anything but a loop device is refused. */
#include "install/freepart.h"
#include "admin.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

void admin_say(const char *kind, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("%s ", kind);
    vprintf(fmt, ap);
    printf("\n");
    fflush(stdout);
    va_end(ap);
}

static int run(const char *const argv[], char *out, size_t cap) {
    int p[2];
    if (pipe(p)) return -1;
    pid_t pid = fork();
    if (pid == 0) {
        dup2(p[1], 1);
        close(p[0]);
        char *const env[] = { "PATH=/usr/sbin:/usr/bin:/sbin:/bin", "LANG=C.UTF-8", NULL };
        execve(argv[0], (char *const *)argv, env);
        _exit(127);
    }
    close(p[1]);
    size_t n = 0;
    char buf[512];
    for (ssize_t r; (r = read(p[0], buf, sizeof buf)) > 0;) {
        if (out && n + (size_t)r < cap) { memcpy(out + n, buf, (size_t)r); n += (size_t)r; }
        else if (!out) fwrite(buf, 1, (size_t)r, stdout);
    }
    if (out) out[n] = 0;
    close(p[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128;
}
int admin_run(const char *const argv[]) { return run(argv, NULL, 0); }
int admin_run_capture(const char *const argv[], char *out, size_t cap) { return run(argv, out, cap); }

static FpProbe p;
static FpUndo u;

int main(int argc, char **argv) {
    if (argc < 3 || strncmp(argv[2], "loop", 4) || !argv[2][4]) { fprintf(stderr, "usage: freepart-cli probe|create loopN ...\n"); return 2; }
    const char *disk = argv[2];
    unsigned long long start = 0, end = 0;
    int undo = 0, have = 0;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--uefi")) fp_test_uefi = 1;
        else if (!strcmp(argv[i], "--undo")) undo = 1;
        else if (!strcmp(argv[i], "--start") && i + 1 < argc) { start = strtoull(argv[++i], NULL, 10); have = 1; }
        else if (!strcmp(argv[i], "--end") && i + 1 < argc) { end = strtoull(argv[++i], NULL, 10); have = 1; }
        else if (!strcmp(argv[i], "--fail") && i + 1 < argc) fp_test_fail = atoi(argv[++i]);
        else return 2;
    }
    fp_probe(disk, &p);
    printf("probe ptable=%s esps=%d free=%llu..%llu bytes=%llu num=%u why=%s\n", fsp_mbr_name(p.facts.mbr), p.esps,
           (unsigned long long)p.region.start, (unsigned long long)p.region.end, (unsigned long long)p.region.bytes, p.choice.num, p.why ? p.why : "-");
    if (!strcmp(argv[1], "probe")) return 0;
    if (strcmp(argv[1], "create")) return 2;
    if (!have) { start = p.choice.region.start; end = p.choice.region.end; }
    const char *why = fp_create(disk, start, end, &u);
    printf("create %s root=%s esp=%s\n", why ? why : "ok", u.root, u.esp);
    if (why) return 1;
    if (undo) { int r = fp_undo(&u); printf("undo %s\n", r ? "FAILED" : "ok"); return r ? 3 : 0; }
    fp_commit(&u);
    return 0;
}
