/* vmusbshim.c — lord-vm-usb.so, preloaded into a machine's QEMU (docs/VIRTUALIZATION.md 6.6; AGENTS.md "Machines' USB
 * devices"). QEMU's usb-host calls libusb_init the first time a device is added, and libusb then looks for every USB device
 * of this computer: a netlink socket for udev's events, /sys/bus/usb, /run/udev. In QEMU's sandbox none of that is
 * allowed (no socket, Landlock), so libusb_init fails and no device can be added. Machines never needs it to look: lord-vm
 * hands QEMU the one device's descriptor (usb-host,hostdevice=/dev/fdset/N, libusb_wrap_sys_device). So before QEMU's
 * main() this sets libusb's default for every context it will make to LIBUSB_OPTION_NO_DEVICE_DISCOVERY (libusb ≥ 1.0.27
 * takes options for the default with a NULL context): no enumeration, no hotplug monitor, nothing opened by path.
 *
 * It holds no state, reads nothing, opens nothing but the library the dynamic loader already maps for QEMU's usb-host
 * module (libusb-1.0.so.0, from /usr), and does nothing when that library isn't there. It links nothing but libdl's
 * dlopen (in the C library). QEMU's sandbox is unchanged by it: its Landlock rules and seccomp allowlist are applied
 * before exec, and the loader maps this file from /usr (read-only, as QEMU's other libraries). */
#include <dlfcn.h>

#define LIBUSB_OPTION_NO_DEVICE_DISCOVERY 2     /* libusb.h's enum libusb_option (also LIBUSB_OPTION_WEAK_AUTHORITY) */

__attribute__((constructor)) static void lord_vm_usb_init(void) {
    void *h = dlopen("libusb-1.0.so.0", RTLD_NOW | RTLD_GLOBAL | RTLD_NOLOAD);
    if (!h) h = dlopen("libusb-1.0.so.0", RTLD_NOW | RTLD_GLOBAL);
    if (!h) return;
    int (*set)(void *, int, ...) = (int (*)(void *, int, ...))dlsym(h, "libusb_set_option");
    if (set) set((void *)0, LIBUSB_OPTION_NO_DEVICE_DISCOVERY);
    /* the handle stays: the option lives in the library, which must stay mapped until QEMU's module uses it */
}
