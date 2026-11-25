/* vm_usb_cli.c — runs Machines' vm-usb-allow (src/vm/vmusb_admin.c) like the admin helper does, for tests/vm_usb_e2e.sh and
 * manual checks (not part of `make test`: it needs root and a real USB device):
 *
 *   sudo build/vm-usb-cli '<machine id> <bus> <address> <vvvv:pppp>' [--not-admin] [-- PROGRAM ARGS...]
 *
 * The caller is SUDO_USER (as the helper's session user; --not-admin: as someone who isn't an administrator). It prints the
 * answer line ("OK vm.usbReady" or "ERR <code>"); with a program, on OK it becomes SUDO_USER (groups, gid, uid) and runs it
 * with the device's descriptor as fd 5, the way lord-server hands it to lord-vm: nothing else of root goes along. Like the
 * helper, it answers "ERR vm.usbNotAtSeat" unless SUDO_USER is the user lord-sessiond has on the monitor (remote/seat.h). */
#include "admin.h"
#include "vm/vm_admin.h"

#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int got_fd = -1;

void admin_say(const char *kind, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("%s ", kind);
    vprintf(fmt, ap);
    printf("\n");
    fflush(stdout);
    va_end(ap);
}

void admin_say_fd(int fd, const char *kind, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("%s ", kind);
    vprintf(fmt, ap);
    printf(" (with a descriptor)\n");
    fflush(stdout);
    va_end(ap);
    if (fd >= 0) got_fd = dup(fd);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: vm-usb-cli '<machine id> <bus> <address> <vvvv:pppp>' [--not-admin] [-- PROGRAM ARGS...]\n"); return 2; }
    const char *who = getenv("SUDO_USER") ? getenv("SUDO_USER") : "root";
    struct passwd *pw = getpwnam(who);
    if (!pw) return 2;
    int admin = 1, prog = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--not-admin")) admin = 0;
        else if (!strcmp(argv[i], "--")) { prog = i + 1; break; }
    }
    char user[64], home[256];
    snprintf(user, sizeof user, "%s", pw->pw_name);
    snprintf(home, sizeof home, "%s", pw->pw_dir);
    AdminReq r = { .id = 1, .user = user, .home = home, .uid = pw->pw_uid, .gid = pw->pw_gid, .admin = admin };
    vm_act_usb_allow(&r, argv[1], strlen(argv[1]));
    if (got_fd < 0) return 1;
    if (!prog || prog >= argc) { close(got_fd); return 0; }
    if (dup2(got_fd, 5) != 5) return 1;
    if (got_fd != 5) close(got_fd);
    fcntl(5, F_SETFD, 0);
    if (initgroups(user, pw->pw_gid) || setgid(pw->pw_gid) || setuid(pw->pw_uid)) { perror("vm-usb-cli: drop"); return 1; }
    execvp(argv[prog], argv + prog);
    perror("vm-usb-cli: exec");
    return 127;
}
