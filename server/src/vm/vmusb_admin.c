/* vmusb_admin.c — vm-usb-allow in the admin helper (vm_admin.h; docs/VIRTUALIZATION.md 6.6; AGENTS.md "Machines' USB
 * devices"): opens one host USB device's node for the session user's machine and hands the descriptor back with the answer
 * (admin_ok_fd). Nothing on the computer changes: the node keeps its owner, mode and access list, nothing is written, no
 * program runs. The device is found again here by its bus and address in sysfs and must still be the one the person
 * picked (vendor and product); never a hub or a root hub, a device whose keyboard or mouse a process holds (the session's),
 * a disk this computer has mounted, uses as swap or under another block device, or storage when the person isn't an
 * administrator (vmusb_refusal). The node opened is checked by its device number against sysfs, and sysfs read again
 * after the open, so a device unplugged and another one given the same address in between is refused. */
#include "vm_admin.h"
#include "vmusb.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

void vm_act_usb_allow(const AdminReq *r, const char *arg, size_t len) {
    char id[37];
    int bus, dev;
    unsigned vendor, product;
    if (vmusb_req_parse(arg, len, id, &bus, &dev, &vendor, &product)) { admin_err("admin.invalidArgument", NULL); return; }
    VmUsbDev d;
    if (vmusb_find(VMUSB_SYS, bus, dev, &d) || d.vendor != vendor || d.product != product) { admin_err("vm.usbGone", NULL); return; }
    const char *no = vmusb_refusal(VMUSB_SYS, "/proc", "/proc/self/mountinfo", "/proc/swaps", &d, r->admin);
    if (no) {
        fprintf(stderr, "admin: %s vm-usb-allow %04x:%04x refused: %s\n", r->user, vendor, product, no);
        admin_err(no, NULL);
        return;
    }
    char p[64];
    snprintf(p, sizeof p, "/dev/bus/usb/%03d/%03d", bus, dev);
    int fd = open(p, O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NOCTTY);
    if (fd < 0) { admin_err(errno == ENOENT || errno == ENODEV ? "vm.usbGone" : "vm.usbOpen", NULL); return; }
    /* the node is that device (its number), and sysfs still says that device is there */
    VmUsbDev again;
    struct stat st;
    if (fstat(fd, &st) || !S_ISCHR(st.st_mode) || major(st.st_rdev) != d.major || minor(st.st_rdev) != d.minor ||
        vmusb_fd_ok(fd, bus, dev) || vmusb_find(VMUSB_SYS, bus, dev, &again) || again.vendor != vendor || again.product != product ||
        again.major != d.major || again.minor != d.minor || strcmp(again.serial, d.serial)) {
        close(fd);
        admin_err("vm.usbGone", NULL);
        return;
    }
    fprintf(stderr, "admin: %s vm-usb-allow %04x:%04x (bus %d, address %d) for machine %s\n", r->user, vendor, product, bus, dev, id);
    admin_ok_fd(fd, "vm.usbReady", NULL);
    close(fd);
}
