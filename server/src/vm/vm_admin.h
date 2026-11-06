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
 *   vm-image-list   "<id> <arch>[ <list name>]"     fetches the maker's SHA-256 list of a catalog entry
 *              (assets/machines/systems.json) into /var/lib/lordos/machines/lists/<id>-<arch>.sums, or the release
 *              folder's listing (<id>-<arch>.index) when the list's name changes per build; then, given a name from
 *              that listing that matches the entry's pattern, that list. Answers the file's path             anyone
 *   vm-image-get    "<id> <arch> <file> <sha256>"   downloads that image of the entry when the list fetched above has
 *              exactly that line, into /var/lib/lordos/machines/images/<sha256>.iso, and keeps it only if its
 *              SHA-256 is that one. Answers the path. Progress: "progress download <done> <total>"            anyone
 *   vm-image-cancel ""                               stops this user's download                             anyone
 *   vm-image-remove "<sha256>"                       deletes a downloaded image (every account's machines share it)
 *                                                                                                              admin
 * The downloads run in lordos-net-machines.scope (firewall app lordos:machines), with the Store's downloader
 * (pkg/http.c fetch_batch_in: a child as nobody, TLS by lord-tls).
 */
#ifndef LORD_VM_ADMIN_H
#define LORD_VM_ADMIN_H

#include "../admin.h"

void vm_act_enable(const AdminReq *r, const char *arg, size_t len);
void vm_act_kvm(const AdminReq *r, const char *arg, size_t len);
void vm_act_image_list(const AdminReq *r, const char *arg, size_t len);
void vm_act_image_get(const AdminReq *r, const char *arg, size_t len);
void vm_act_image_cancel(const AdminReq *r, const char *arg, size_t len);
void vm_act_image_remove(const AdminReq *r, const char *arg, size_t len);

/* the packages vm-enable installs for `arches` (VM_ARCH bits: 1 arm64, 2 amd64) that this computer lacks, by the
 * files each package brings; space-separated into out. Returns how many. Also used by lord.vm.requirements. */
int vm_missing_packages(int arches, char *out, size_t n);

#define VM_ADMIN_ACTIONS \
    { "vm-enable", vm_act_enable, A_FORK }, \
    { "vm-kvm", vm_act_kvm, 0 }, \
    { "vm-image-list", vm_act_image_list, A_FORK | A_ANYONE }, \
    { "vm-image-get", vm_act_image_get, A_FORK | A_ANYONE }, \
    { "vm-image-cancel", vm_act_image_cancel, A_ANYONE }, \
    { "vm-image-remove", vm_act_image_remove, 0 },

#endif
