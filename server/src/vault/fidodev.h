/* fidodev.h — find and open FIDO2 keys (axia-server side, outside the sandbox).
 *
 * Walks /sys/class/hidraw/hidraw*: a device is FIDO if its HID report descriptor
 * (device/report_descriptor) declares an application collection with Usage Page 0xF1D0 (FIDO
 * Alliance) and Usage 0x01 (CTAPHID). The name and vid:pid come from device/uevent (HID_NAME,
 * HID_ID). axia-server opens the chosen one (after the user's permission) and passes the fd to
 * axia-vault; neither the app nor axia-vault opens /dev. */
#ifndef LORD_VAULT_FIDODEV_H
#define LORD_VAULT_FIDODEV_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char path[32];          /* "/dev/hidraw3" */
    char name[128];         /* "Yubico YubiKey OTP+FIDO+CCID" */
    unsigned vid, pid;
} FidoDev;

/* Up to max keys; returns how many. */
int fidodev_list(FidoDev *out, int max);
/* Opens path (must be /dev/hidrawN and a FIDO key) with O_RDWR|O_CLOEXEC|O_NONBLOCK.
 * fd or -1 (errno). */
int fidodev_open(const char *path);
/* 1 if the HID descriptor declares the FIDO collection (for tests and for fidodev_list). */
int fidodev_desc_is_fido(const uint8_t *desc, size_t len);
/* For tests: another root instead of "/sys/class/hidraw" (NULL restores the default). */
void fidodev_set_sysroot(const char *dir);

#endif
