/* vm_net_cli.c — runs Machines' vm-net-bridge and vm-net-unbridge (src/vm/vmnet_admin.c) like the admin helper does, for
 * a manual check on a computer with a wired card (not part of `make test`: it needs root and makes a real macvtap):
 *
 *   sudo build/vm-net-cli vm-net-bridge '<machine id> <card> <mac>'
 *   sudo build/vm-net-cli vm-net-unbridge '<machine id>'
 *   sudo build/vm-net-cli sweep
 *
 * The caller is SUDO_USER (as the helper's session user). admin_say prints to stdout; admin_run runs udevadm settle. */
#include "admin.h"
#include "vm/vm_admin.h"

#include <pwd.h>
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

int admin_run(const char *const argv[]) {
    pid_t p = fork();
    if (!p) { char *env[] = { NULL }; execve(argv[0], (char *const *)argv, env); _exit(127); }
    int st = 0;
    if (p < 0 || waitpid(p, &st, 0) < 0) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: vm-net-cli vm-net-bridge|vm-net-unbridge ARG | sweep\n"); return 2; }
    struct passwd *pw = getpwnam(getenv("SUDO_USER") ? getenv("SUDO_USER") : "root");
    if (!pw) return 2;
    AdminReq r = { .id = 1, .user = pw->pw_name, .home = pw->pw_dir, .uid = pw->pw_uid, .gid = pw->pw_gid, .admin = 1 };
    if (!strcmp(argv[1], "sweep")) { vm_net_sweep(r.uid); return 0; }
    if (argc != 3) return 2;
    if (!strcmp(argv[1], "vm-net-bridge")) vm_act_net_bridge(&r, argv[2], strlen(argv[2]));
    else if (!strcmp(argv[1], "vm-net-unbridge")) vm_act_net_unbridge(&r, argv[2], strlen(argv[2]));
    else return 2;
    return 0;
}
