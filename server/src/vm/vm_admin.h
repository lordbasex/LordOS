/* vm_admin.h — Machines' closed system actions in the admin helper (docs/VIRTUALIZATION.md 3.4, 10 and "Phase 1
 * implementation", The app). Nothing in the argument names a program, a package or a path: only words checked here.
 *
 *   vm-enable  "amd64" | "arm64" | "amd64 arm64"   installs what Machines needs for those guest architectures and
 *              isn't there yet, through LordOS's package engine (pkg-install): QEMU's emulator for each, its
 *              UEFI firmware, the D-Bus display module, qemu-utils, passt                                   admin
 *   vm-kvm     ""                                    lets the session user use /dev/kvm: adds them to the kvm
 *              group (for the next sessions) and gives them read and write on /dev/kvm now with an access
 *              list entry, so no new login is needed (udev drops it when the device is made again; the group
 *              keeps working)                                                                                 admin
 */
#ifndef LORD_VM_ADMIN_H
#define LORD_VM_ADMIN_H

#include "../admin.h"

void vm_act_enable(const AdminReq *r, const char *arg, size_t len);
void vm_act_kvm(const AdminReq *r, const char *arg, size_t len);

/* the packages vm-enable installs for `arches` (VM_ARCH bits: 1 arm64, 2 amd64) that this computer lacks, by the
 * files each package brings; space-separated into out. Returns how many. Also used by lord.vm.requirements. */
int vm_missing_packages(int arches, char *out, size_t n);

#define VM_ADMIN_ACTIONS \
    { "vm-enable", vm_act_enable, A_FORK }, \
    { "vm-kvm", vm_act_kvm, 0 },

#endif
