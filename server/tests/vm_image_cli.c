/* vm_image_cli.c — runs Machines' vm-image-* actions (src/vm/vmimage_admin.c) like the admin helper does, against the
 * real makers' servers, for a manual check on a build VM (not part of `make test`: it needs root and the network).
 * Built with its own catalog and store paths: copy it and lord-tls into a root-owned folder first (http.c wants lord-tls
 * next to the binary, root's), then
 *
 *   sudo DIR/vm-image-cli vm-image-list 'debian amd64'
 *   sudo DIR/vm-image-cli vm-image-get 'debian amd64 <file> <sha256>'
 *
 * admin_say and admin_code print to stdout, as in tests/disk_cli.c. */
#include "admin.h"
#include "vm/vm_admin.h"

#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>

void admin_say(const char *kind, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("%s ", kind);
    vprintf(fmt, ap);
    printf("\n");
    fflush(stdout);
    va_end(ap);
}

int main(int argc, char **argv) {
    prctl(PR_SET_NAME, "lord-admin");
    if (argc != 3) { fprintf(stderr, "usage: vm-image-cli vm-image-list|vm-image-get|vm-image-remove ARG\n"); return 2; }
    struct passwd *pw = getpwnam(getenv("SUDO_USER") ? getenv("SUDO_USER") : "root");
    if (!pw) return 2;
    AdminReq r = { .id = 1, .user = pw->pw_name, .home = pw->pw_dir, .uid = pw->pw_uid, .gid = pw->pw_gid, .admin = 1 };
    if (!strcmp(argv[1], "vm-image-list")) vm_act_image_list(&r, argv[2], strlen(argv[2]));
    else if (!strcmp(argv[1], "vm-image-get")) vm_act_image_get(&r, argv[2], strlen(argv[2]));
    else if (!strcmp(argv[1], "vm-image-remove")) vm_act_image_remove(&r, argv[2], strlen(argv[2]));
    else return 2;
    return 0;
}
