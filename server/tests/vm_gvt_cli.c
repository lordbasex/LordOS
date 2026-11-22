/* vm_gvt_cli.c — runs Machines' vm-gvt-create, vm-gvt-remove and gpu-min-freq (src/vm/vmgpu_admin.c) like the admin
 * helper does, for a manual check on a computer whose iGPU runs GVT-g (not part of `make test`: it needs root and makes a
 * real mdev). gpu-role and gpu-memlock-cap are left out on purpose: they write /etc (vmgpu-test runs them under a fake
 * root folder).
 *
 *   sudo build/vm-gvt-cli vm-gvt-create '<machine id> <mdev type>'
 *   sudo build/vm-gvt-cli vm-gvt-remove '<machine id>'
 *   sudo build/vm-gvt-cli gpu-min-freq '<pci address> <MHz|default>'
 *   sudo build/vm-gvt-cli vm-memlock '<qemu pid>'
 *   sudo build/vm-gvt-cli sweep
 *
 * The caller is SUDO_USER (as the helper's session user). The desktop's GPU is read from sudo's descriptors, which hold
 * no card, so the firmware's primary display counts as the desktop's. admin_say prints to stdout. */
#include "admin.h"
#include "vm/vm_admin.h"

#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void admin_say(const char *kind, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("%s ", kind);
    vprintf(fmt, ap);
    printf("\n");
    fflush(stdout);
    va_end(ap);
}

int admin_run(const char *const argv[]) { (void)argv; return -1; }

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: vm-gvt-cli vm-gvt-create|vm-gvt-remove|gpu-min-freq ARG | sweep\n"); return 2; }
    struct passwd *pw = getpwnam(getenv("SUDO_USER") ? getenv("SUDO_USER") : "root");
    if (!pw) return 2;
    AdminReq r = { .id = 1, .user = pw->pw_name, .home = pw->pw_dir, .uid = pw->pw_uid, .gid = pw->pw_gid, .admin = 1 };
    if (!strcmp(argv[1], "sweep")) { vm_gvt_sweep(r.uid); return 0; }
    if (argc != 3) return 2;
    if (!strcmp(argv[1], "vm-gvt-create")) vm_act_gvt_create(&r, argv[2], strlen(argv[2]));
    else if (!strcmp(argv[1], "vm-gvt-remove")) vm_act_gvt_remove(&r, argv[2], strlen(argv[2]));
    else if (!strcmp(argv[1], "gpu-min-freq")) vm_act_gpu_min_freq(&r, argv[2], strlen(argv[2]));
    else if (!strcmp(argv[1], "vm-memlock")) vm_act_memlock(&r, argv[2], strlen(argv[2]));
    else return 2;
    return 0;
}
