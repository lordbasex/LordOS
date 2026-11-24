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
 *   vm-image-get-user "<https URL> <sha256> <size|-> [<signature URL> <checksum file URL>]"   a system the person
 *              added (Machines settings → Systems; approved by the maintainer on 2026-11-22, AGENTS.md "Machines'
 *              downloads of your own systems"). Each address must pass vmurl_check (vmurl.h): https://, a public DNS
 *              name (pkg/pubhost.h: no IP literal, localhost, .local, .internal, .lan…), port 443 only, no user name or
 *              password, a plain path to a file, at most 1000 bytes; the downloader (pkg/http.c, public_only) connects
 *              only to public addresses and follows a redirect only to an address that passes the same rules. The
 *              SHA-256 is required (64 hex digits); the size, when given, is 1 B to 16 GiB and is the most it may be
 *              (else 16 GiB); the image store must keep 2 GiB free after it (vm.noSpace need= free=). Into
 *              images/<sha256>.iso like vm-image-get (resumed from its .part, kept only with that SHA-256, progress
 *              lines, vm-image-cancel stops it), with <sha256>.json {"id": "user", "file", "from": the address without
 *              its query}. The optional signature files (64 KB, 1 MB) go to lists/u-<sha256>.sig and .sums, root's;
 *              root never reads them: lord-server hands them to lord-pgpv. Refused: vm.urlRefused             admin
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
 *   gpu-role        "<pci address> <unused|machines-shared|machines-dedicated|ai>"   a spare GPU's role (Settings → Graphics
 *              cards; AGENTS.md "Machines' graphics cards"): for an Intel iGPU GVT-g supports, machines-shared writes the
 *              fixed files /etc/modprobe.d/lordos-gpu.conf (options i915 enable_gvt=1 enable_guc=0) and
 *              /etc/modules-load.d/lordos-gpu.conf (kvmgt), adopting hand-written i915-gvt.conf / kvmgt.conf that say
 *              exactly that (removed), and runs update-initramfs -u; unused removes them. The desktop's GPU is refused
 *              (gpu.desktop); machines-dedicated and ai aren't built yet (gpu.roleNotYet). Takes effect after a
 *              restart. Answers gpu.roleSet                                                                    admin
 *   vm-gvt-create   "<machine id> <mdev type>"     an Intel GVT-g vGPU for that machine: the type must be one the iGPU
 *              (not the desktop's) lists with an instance available; the mdev gets the machine's UUID, root notes the
 *              caller in /run/lordos/vm-gvt/<id>, and /dev/vfio/<group> is given to the caller 0600. One the same caller
 *              made is given again; anyone else's is refused (vm.gvtBusy). Answers vm.gvtReady name=<group>    anyone
 *   vm-gvt-remove   "<machine id>"                  removes that vGPU: only the caller's, never while a process holds
 *              its /dev/vfio/<group> (vm.gvtInUse)                                                             anyone
 *   gpu-min-freq    "<pci address> <MHz|default>"   the GVT iGPU's minimum frequency (rps_min_freq_mhz) within its own
 *              RPn..RP0, only while the caller has a vGPU; default puts back the value read before the first change
 *              once no vGPU is left                                                                            anyone
 *   vm-memlock      "<pid>"                         the locked-memory limit of the caller's QEMU of a machine with a vGPU
 *              (maintainer's decision 2026-11-21): a live /usr/bin/qemu-system-* of the caller whose parent is the
 *              installed /usr/lib/lordos/bin/lord-vm, whose -uuid and vfio-pci-nohotplug sysfsdev name a vGPU the caller
 *              made and which holds that vGPU's /dev/vfio/<group> open; one grant per vGPU (pid and start time noted,
 *              another refused while it lives); soft and hard RLIMIT_MEMLOCK of that one process = its -m + 512 MB,
 *              refused when every live grant together would pass the cap (vm.memlockCap need= cap=<free>). Nothing else
 *              in the session changes. Answers vm.memlockSet                                                   anyone
 *   gpu-memlock-cap "<MB|default>"                  "Memory machines may lock" (all together): writes /etc/lordos/gpu-memlock.conf
 *              (cap_mb=<MB>, 2048 to total - 2048) or removes it (default: total - max(4 GB, 25 %), down to a GB)
 *              Answers gpu.memlockSet                                                                          admin
 *   vm-usb-allow    "<machine id> <bus> <address> <vvvv>:<pppp>"   a host USB device for a running machine (6.6; AGENTS.md
 *              "Machines' USB devices", approved by the maintainer on 2026-11-24): the device at that bus and address
 *              (sysfs, /sys/bus/usb/devices) must still have that vendor and product; never a hub or a root hub
 *              (vm.usbHub), a device with an input node (keyboard, mouse) any process holds open (vm.usbInputInUse), a
 *              device with a disk this computer has mounted, uses as swap or under another block device (vm.usbMounted), or
 *              storage, a network card or a Bluetooth adapter for someone who isn't an administrator (vm.usbAdminOnly). Opens /dev/bus/usb/BBB/DDD read and
 *              write, checks it by its device number and sysfs again, and answers vm.usbReady with the descriptor
 *              (admin_ok_fd, SCM_RIGHTS): the node's owner, mode and access list never change and nothing is written.
 *              lord-server hands it to that machine's lord-vm and closes its own copy                         anyone
 * At the helper's start and end (vm_net_sweep, vm_gvt_sweep) the caller's macvtaps that no process holds are removed; a restart takes
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
void vm_act_image_get_user(const AdminReq *r, const char *arg, size_t len);
void vm_act_image_cancel(const AdminReq *r, const char *arg, size_t len);
void vm_act_image_remove(const AdminReq *r, const char *arg, size_t len);
void vm_act_net_bridge(const AdminReq *r, const char *arg, size_t len);
void vm_act_net_unbridge(const AdminReq *r, const char *arg, size_t len);
/* removes uid's macvtaps (by their alias) that no process holds */
void vm_net_sweep(uid_t uid);
void vm_act_gpu_role(const AdminReq *r, const char *arg, size_t len);
void vm_act_gvt_create(const AdminReq *r, const char *arg, size_t len);
void vm_act_gvt_remove(const AdminReq *r, const char *arg, size_t len);
void vm_act_gpu_min_freq(const AdminReq *r, const char *arg, size_t len);
void vm_act_memlock(const AdminReq *r, const char *arg, size_t len);
void vm_act_gpu_memlock_cap(const AdminReq *r, const char *arg, size_t len);
void vm_act_usb_allow(const AdminReq *r, const char *arg, size_t len);
/* removes uid's vGPUs no process holds; the iGPU's minimum frequency back when none is left */
void vm_gvt_sweep(uid_t uid);

/* the packages vm-enable installs for `arches` (VM_ARCH bits: 1 arm64, 2 amd64) that this computer lacks, by the
 * files each package brings; space-separated into out. Returns how many. Also used by lord.vm.requirements. */
int vm_missing_packages(int arches, char *out, size_t n);

#define VM_ADMIN_ACTIONS \
    { "vm-enable", vm_act_enable, A_FORK }, \
    { "vm-kvm", vm_act_kvm, 0 }, \
    { "vm-image-list", vm_act_image_list, A_FORK | A_ANYONE }, \
    { "vm-image-get", vm_act_image_get, A_FORK | A_ANYONE }, \
    { "vm-image-get-user", vm_act_image_get_user, A_FORK }, \
    { "vm-image-cancel", vm_act_image_cancel, A_ANYONE }, \
    { "vm-image-remove", vm_act_image_remove, 0 }, \
    { "vm-net-bridge", vm_act_net_bridge, 0 }, \
    { "vm-net-unbridge", vm_act_net_unbridge, A_ANYONE }, \
    { "gpu-role", vm_act_gpu_role, A_FORK }, \
    { "vm-gvt-create", vm_act_gvt_create, A_ANYONE }, \
    { "vm-gvt-remove", vm_act_gvt_remove, A_ANYONE }, \
    { "gpu-min-freq", vm_act_gpu_min_freq, A_ANYONE }, \
    { "vm-memlock", vm_act_memlock, A_ANYONE }, \
    { "gpu-memlock-cap", vm_act_gpu_memlock_cap, 0 }, \
    { "vm-usb-allow", vm_act_usb_allow, A_ANYONE },

#endif
