/* ssh_root_e2e.c — root over SSH end to end (src/remote/ssh_root.c with the real password check,
 * src/users/users_admin.c): runs the same admin actions Settings → Sharing uses, as root, on a test
 * machine with openssh-server. Not part of make test (it changes sshd's configuration):
 *
 *   sudo build/ssh-root-e2e <admin user> <their password> <action> [args…]
 *     actions: status | mode <off|key|password> | add <pubkey file> | remove <SHA256:…> */
#include "../src/remote/ssh_root.h"

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
    va_end(ap);
}
static int run(const char *const argv[], const char *input) {
    int p[2];
    if (input && pipe(p)) return -1;
    pid_t pid = fork();
    if (pid == 0) {
        if (input) { dup2(p[0], 0); close(p[0]); close(p[1]); }
        char *const env[] = { "PATH=/usr/sbin:/usr/bin:/sbin:/bin", NULL };
        execve(argv[0], (char *const *)argv, env);
        _exit(127);
    }
    if (input) { close(p[0]); if (write(p[1], input, strlen(input)) < 0) {} close(p[1]); }
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}
int admin_run(const char *const argv[]) { return run(argv, NULL); }
int admin_run_input(const char *const argv[], const char *input) { return run(argv, input); }

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s user password action [args]\n", argv[0]); return 2; }
    AdminReq r = { .id = 1, .user = argv[1], .home = "/", .uid = 0, .gid = 0, .admin = 1 };
    char arg[8192];
    const char *act = argv[3];
    if (!strcmp(act, "status")) { ssh_act_root_status(&r, "", 0); return 0; }
    if (!strcmp(act, "mode") && argc == 5) { int n = snprintf(arg, sizeof arg, "%s\n%s", argv[4], argv[2]); ssh_act_root(&r, arg, (size_t)n); return 0; }
    if (!strcmp(act, "add") && argc == 5) {
        FILE *f = fopen(argv[4], "r");
        char line[4096] = "";
        if (!f || !fgets(line, sizeof line, f)) return 2;
        fclose(f);
        line[strcspn(line, "\n")] = 0;
        int n = snprintf(arg, sizeof arg, "add\n%s\n%s", argv[2], line);
        ssh_act_root_key(&r, arg, (size_t)n);
        return 0;
    }
    if (!strcmp(act, "remove") && argc == 5) { int n = snprintf(arg, sizeof arg, "remove\n%s\n%s", argv[2], argv[4]); ssh_act_root_key(&r, arg, (size_t)n); return 0; }
    return 2;
}
