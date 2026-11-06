/* manualpart_cli.c — the real steps of manual partitioning (src/install/manualpart.c) on a loop
 * device, for manual testing (root; not part of `make test`, never installed; tests/manualpart_loop.sh).
 *
 *   build/manualpart-cli probe <loopN> [--uefi]
 *   build/manualpart-cli apply <loopN> [--uefi] --tag T [--delete L] [--create L] [--root N] [--home N]
 *                              [--home-format] [--swap N] [--esp N] [--esp-format] [--swap-file MiB]
 *                              [--fail N] [--undo]
 *
 *   --uefi     act as if booted through UEFI with efibootmgr present (a test machine may not be)
 *   --fail N   1: fail after sgdisk wrote; 2: fail before the first format
 *   --undo     after success, undo as a failed copy or grub step would
 *
 * Anything but a loop device is refused. */
#include "install/manualpart.h"
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
    fflush(stdout);
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

static MppProbe p;
static MppResult res;
static MpPlan pl;

int main(int argc, char **argv) {
    if (argc < 3 || strncmp(argv[2], "loop", 4) || !argv[2][4]) { fprintf(stderr, "usage: manualpart-cli probe|apply loopN ...\n"); return 2; }
    const char *disk = argv[2], *tag = "", *del = "", *create = "";
    int undo = 0;
    memset(&pl, 0, sizeof pl);
    for (int i = 3; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--uefi")) fp_test_uefi = 1;
        else if (!strcmp(a, "--undo")) undo = 1;
        else if (!strcmp(a, "--home-format")) pl.home_format = 1;
        else if (!strcmp(a, "--esp-format")) pl.esp_format = 1;
        else if (!v) return 2;
        else if (!strcmp(a, "--tag")) { tag = v; i++; }
        else if (!strcmp(a, "--delete")) { del = v; i++; }
        else if (!strcmp(a, "--create")) { create = v; i++; }
        else if (!strcmp(a, "--root")) { pl.part[MP_ROOT] = (unsigned)atoi(v); i++; }
        else if (!strcmp(a, "--home")) { pl.part[MP_HOME] = (unsigned)atoi(v); i++; }
        else if (!strcmp(a, "--swap")) { pl.part[MP_SWAP] = (unsigned)atoi(v); i++; }
        else if (!strcmp(a, "--esp")) { pl.part[MP_ESP] = (unsigned)atoi(v); i++; }
        else if (!strcmp(a, "--swap-file")) { pl.swap_file = atol(v); i++; }
        else if (!strcmp(a, "--fail")) { fp_test_fail = atoi(v); i++; }
        else return 2;
    }
    mpp_probe(disk, &p);
    printf("probe tag=%s why=%s lsec=%u first=%llu last=%llu\n", p.tag[0] ? p.tag : "-", p.why ? p.why : "-", p.fp.table.lsec,
           (unsigned long long)p.fp.table.first, (unsigned long long)p.fp.table.last);
    for (int i = 0; i < p.fp.table.n; i++) {
        const FspPart *q = &p.fp.table.p[i];
        printf("part %u %llu %llu esp=%d fs=%s\n", q->num, (unsigned long long)q->start, (unsigned long long)q->end, q->esp,
               q->num <= FSP_MAX_PARTS && p.facts.fstype[q->num][0] ? p.facts.fstype[q->num] : "-");
        if (q->esp) {
            char path[48], names[8][33];
            fsp_part_path(disk, q->num, path, sizeof path);
            int n = mpp_esp_loaders(path, names, 8);
            for (int k = 0; k < n; k++) printf("loader %u %s\n", q->num, names[k]);
        }
    }
    if (!strcmp(argv[1], "probe")) return 0;
    if (strcmp(argv[1], "apply")) return 2;
    if (mp_parse_delete(del, &pl) || mp_parse_create(create, &pl)) { printf("apply install.badPlan\n"); return 1; }
    const char *why = mpp_apply(disk, tag, &pl, &res);
    printf("apply %s root=%s home=%s swap=%s esp=%s homefs=%s espnum=%u espformatted=%d undo=%s\n", why ? why : "ok",
           res.dev[MP_ROOT], res.dev[MP_HOME], res.dev[MP_SWAP], res.dev[MP_ESP], res.home_fs, res.esp_num, res.esp_formatted,
           res.undo.active ? "active" : "none");
    if (why) return 1;
    if (undo) { int r = fp_undo(&res.undo); printf("undo %s\n", r ? "FAILED" : "ok"); return r ? 3 : 0; }
    fp_commit(&res.undo);
    return 0;
}
