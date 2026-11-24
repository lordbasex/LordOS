/* vmusb.h — a host USB device for a machine (docs/VIRTUALIZATION.md 6.6; approved by the maintainer on 2026-11-24, AGENTS.md
 * "Machines' USB devices"). What this computer's USB devices are, read from sysfs, and the rules about them, shared by
 * three sides that each check for themselves:
 *
 *   lord-server  lists the devices for the Devices menu (lord.vm.usbDevices), with why one can't be given
 *   admin.c      vm-usb-allow (vmusb_admin.c, root): finds the device again by its bus and address, refuses what must
 *                never leave this computer, opens /dev/bus/usb/BBB/DDD and hands the descriptor back (SCM_RIGHTS); the node's
 *                owner and mode never change, nothing is written
 *   lord-vm      takes that descriptor only if it is a USB device node with that bus and address (vmusb_fd_ok) and gives
 *                it to QEMU through an fdset (usb-host,hostdevice=/dev/fdset/N): QEMU never opens a path
 *
 * Never given: a hub or a root hub; a device with an input device (keyboard, mouse) a process holds open (the session's
 * own); a device with a disk this computer has mounted, uses as swap or under another block device; storage when the
 * person isn't an administrator. Pure functions of a sysfs folder, /proc and /proc/self/mountinfo given as paths, so
 * tests/vmusb_test.c runs them on a made-up tree. */
#ifndef LORD_VMUSB_H
#define LORD_VMUSB_H

#include <stddef.h>
#include <sys/types.h>

#define VMUSB_SYS        "/sys/bus/usb/devices"
#define VMUSB_MAX        64             /* devices listed */
#define VMUSB_MAJOR      189            /* usb_device: /dev/bus/usb/BBB/DDD */
#define VMUSB_BUS_MAX    255
#define VMUSB_DEV_MAX    127            /* a USB address */
#define VMUSB_PER_VM     8              /* devices one machine holds at once */
#define VMUSB_RULES      8              /* auto-connect rules per machine (machine.json "usb_auto") */
#define VMUSB_SERIAL_MAX 63

/* what a device has (bDeviceClass and each interface's bInterfaceClass) */
enum {
    VMUSB_F_ROOTHUB = 1 << 0,           /* usbN: the controller itself */
    VMUSB_F_HUB     = 1 << 1,           /* class 0x09 */
    VMUSB_F_INPUT   = 1 << 2,           /* an input device (event, mouse, js node) below it */
    VMUSB_F_STORAGE = 1 << 3,           /* class 0x08 */
    VMUSB_F_VIDEO   = 1 << 4,           /* class 0x0e: a camera */
    VMUSB_F_AUDIO   = 1 << 5,           /* class 0x01: a microphone or speakers */
    VMUSB_F_BLOCK   = 1 << 6,           /* a block device below it */
    VMUSB_F_HID     = 1 << 7,           /* class 0x03 */
    VMUSB_F_PRINTER = 1 << 8,           /* class 0x07 */
};

typedef struct {
    char name[32];                      /* sysfs name, "1-11.3" */
    int bus, dev;                       /* busnum, devnum */
    unsigned vendor, product;           /* idVendor, idProduct */
    unsigned major, minor;              /* the usbfs node's numbers ("dev") */
    int speed;                          /* Mbit/s (1.5 is 1) */
    int flags;                          /* VMUSB_F_* */
    char serial[VMUSB_SERIAL_MAX + 1];  /* printable ASCII only, else "" */
    char maker[64], model[64];          /* manufacturer, product: printable, cut */
} VmUsbDev;

/* the devices under sys (VMUSB_SYS, or a test's tree), sorted by bus and address; root hubs left out. Returns how many */
int vmusb_list(const char *sys, VmUsbDev *out, int max);
/* the device with that bus and address: 0, or -1 */
int vmusb_find(const char *sys, int bus, int dev, VmUsbDev *out);

/* vm-usb-allow's argument: "<machine id> <bus> <address> <vvvv>:<pppp>" (the id as machine.json's, lowercase hex
 * digits and dashes, 36; numbers in decimal without leading zeros or signs; the ids four lowercase hex digits). 0 or -1 */
int vmusb_req_parse(const char *arg, size_t len, char id[37], int *bus, int *dev, unsigned *vendor, unsigned *product);

/* an auto-connect rule: "vvvv:pppp" or "vvvv:pppp:serial" (serial: 1 to 63 of [A-Za-z0-9._-]) */
typedef struct { unsigned vendor, product; char serial[VMUSB_SERIAL_MAX + 1]; } VmUsbRule;
int vmusb_rule_parse(const char *s, VmUsbRule *r);
void vmusb_rule_str(const VmUsbRule *r, char *out, size_t n);
int vmusb_rule_match(const VmUsbRule *r, const VmUsbDev *d);

/* the input and block devices below a device: each one's numbers, character or block. Up to max; returns how many */
typedef struct { unsigned major, minor; int block, held; } VmUsbNode;   /* held: a block device another one holds */
int vmusb_nodes(const char *sys, const VmUsbDev *d, VmUsbNode *out, int max);
/* 1 when a process holds one of the device's input nodes open (proc: "/proc"; skip_pid: 0, or a pid not counted) */
int vmusb_input_held(const char *proc, const VmUsbNode *n, int nn, pid_t skip_pid);
/* 1 when one of its block devices is mounted (mountinfo's major:minor), swap (swaps: /proc/swaps) or held by another
 * block device (sys's holders/); the partitions count */
int vmusb_block_busy(const char *sys, const VmUsbDev *d, const char *mountinfo, const char *swaps);

/* why a device is never given (root's check): NULL when it may be, else an error code of errors.json (vm.usbHub, …) */
const char *vmusb_refusal(const char *sys, const char *proc, const char *mountinfo, const char *swaps, const VmUsbDev *d, int admin);

/* lord-vm's check of the descriptor it got: a character device of usb_device whose minor is that bus and address
 * ((bus - 1) * 128 + address - 1), opened for reading and writing. 0 or -1 */
int vmusb_fd_ok(int fd, int bus, int dev);

/* the device's line in VM_USB's text and VM_USB_STATE's: "BUS DEV VVVV:PPPP" (no serial: it stays on this computer) */
int vmusb_line_parse(const char *s, int *bus, int *dev, unsigned *vendor, unsigned *product);

#endif
