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
 *              (assets/machines/systems.json) and, when the maker signs it apart, its signature, checks the list's
 *              OpenPGP signature with the keys the entry pins (assets/machines/keys, from packaging/keys/machines)
 *              and keeps only the text that was signed, as /var/lib/lordos/machines/lists/<id>-<arch>.list; a list
 *              that isn't signed is refused (vm.listUnsigned), and one whose signature doesn't verify with those
 *              keys too (vm.listBadSignature). When the list's name changes per build it first fetches the release
 *              folder's listing (<id>-<arch>.index); then, given a name from that listing that matches the entry's
 *              pattern, that list. Answers the file's path                                                     anyone
 *   vm-image-get    "<id> <arch> <file> <sha256>"   downloads that image of the entry when the checked list above
 *              has exactly that line, into /var/lib/lordos/machines/images/<sha256>.iso, and keeps it only if its
 *              SHA-256 is that one. A cut or stopped download stays as <sha256>.iso.part and the next try continues
 *              it with a Range request (the hash is still over the whole file). Also writes <sha256>.json (what
 *              the image is). Answers the path. Progress: "progress download <done> <total>"                 anyone
 *   vm-image-cancel ""                               stops this user's download (its part is kept)           anyone
 *   vm-image-remove "<sha256>"                       deletes a downloaded image, or the part of a stopped
 *              download (every account's machines share it); refused while it downloads or any running machine
 *              has it in its CD drive (vm.imageInUse)                                                           admin
 *   vm-net-bridge   "<machine id> <card> <mac>"     a bridged machine's network (6.5): a macvtap in bridge mode named
 *              after the machine ("lvm" + 12 hex digits of its id) on that physical wired card (not Wi-Fi, a bridge, a
 *              bond or anything virtual: refused with why), with that unicast MAC (never the card's own), the alias
 *              "lordos-vm <uid> <id>", up and with nothing else (no address, route or forwarding), and its
 *              /dev/tap<index> given to the calling user, 0600. One that is already there for the same user and machine
 *              is used again (made again when its card or address changed and no process holds it); another's is
 *              refused (vm.bridgeBusy). Answers vm.bridgeReady name=<macvtap>                                   admin
 *   vm-net-unbridge "<machine id>"                   removes that machine's macvtap: only the caller's own (by the
 *              alias), never while a process holds its device (vm.bridgeInUse)                                anyone
 * At the helper's start and end (vm_net_sweep) the caller's macvtaps that no process holds are removed; a restart takes
 * them all (they are never written to disk).
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
void vm_act_net_bridge(const AdminReq *r, const char *arg, size_t len);
void vm_act_net_unbridge(const AdminReq *r, const char *arg, size_t len);
/* removes uid's macvtaps (by their alias) that no process holds */
void vm_net_sweep(uid_t uid);

/* the packages vm-enable installs for `arches` (VM_ARCH bits: 1 arm64, 2 amd64) that this computer lacks, by the
 * files each package brings; space-separated into out. Returns how many. Also used by lord.vm.requirements. */
int vm_missing_packages(int arches, char *out, size_t n);

#define VM_ADMIN_ACTIONS \
    { "vm-enable", vm_act_enable, A_FORK }, \
    { "vm-kvm", vm_act_kvm, 0 }, \
    { "vm-image-list", vm_act_image_list, A_FORK | A_ANYONE }, \
    { "vm-image-get", vm_act_image_get, A_FORK | A_ANYONE }, \
    { "vm-image-cancel", vm_act_image_cancel, A_ANYONE }, \
    { "vm-image-remove", vm_act_image_remove, 0 }, \
    { "vm-net-bridge", vm_act_net_bridge, 0 }, \
    { "vm-net-unbridge", vm_act_net_unbridge, A_ANYONE },

#endif
