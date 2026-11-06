/* install_run_cli.c — install-disks and install-run (src/install/install.c) as a command, for testing
 * the whole install on a throwaway virtual machine booted from the live ISO (root; not part of
 * `make test`, never installed or shipped). install.c itself refuses outside the live system.
 *
 *   build/install-run-cli disks
 *   build/install-run-cli run plan.json      the same JSON the installer sends (docs/ISO.md)
 *
 * What install.c reports goes to standard output, one "KIND text" line each. */
#include "install/install.c"

#include <sys/wait.h>

void admin_say(const char *kind, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("%s ", kind);
    vprintf(fmt, ap);
    printf("\n");
    fflush(stdout);
    va_end(ap);
}

static int run_prog(const char *const argv[], char *out, size_t cap) {
    int p[2];
    if (pipe(p)) return -1;
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        dup2(p[1], 1);
        dup2(p[1], 2);
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
int admin_run(const char *const argv[]) { return run_prog(argv, NULL, 0); }
int admin_run_capture(const char *const argv[], char *out, size_t cap) { return run_prog(argv, out, cap); }

int main(int argc, char **argv) {
    static AdminReq req;
    if (argc == 2 && !strcmp(argv[1], "disks")) { install_act_disks(&req, "", 0); return 0; }
    if (argc != 3 || strcmp(argv[1], "run")) { fprintf(stderr, "usage: install-run-cli disks | run plan.json\n"); return 2; }
    static char plan[ADMIN_ARG_MAX];
    FILE *f = fopen(argv[2], "r");
    if (!f) { perror(argv[2]); return 2; }
    size_t n = fread(plan, 1, sizeof plan - 1, f);
    fclose(f);
    plan[n] = 0;
    install_act_run(&req, plan, n);
    return 0;
}
