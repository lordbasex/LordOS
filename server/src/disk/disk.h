/* disk.h — disks and partitions for the Disks app (see docs/SYSTEM.md, "Disks").
 *
 * disk.c reads without root: /sys/block, the udev database (/run/udev/data/b<major>:<minor>) and
 * /proc/self/mountinfo. It is used by axia-server (lord.disks, js_disk.c) and by the admin helper
 * (disk_admin.c), which validates with these same functions on the root side.
 * smart.c holds the parsers (pure functions) for ATA SMART and the NVMe health log; the helper
 * reads them with SG_IO / NVME_IOCTL_ADMIN_CMD (action disk-smart). */
#ifndef LORD_DISK_H
#define LORD_DISK_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "../admin.h"

/* Block device name as the kernel gives it: sda, sda1, nvme0n1p2, mmcblk0p1, vdb, loop3
 * (lowercase letters and digits, starts with a letter, up to 31). */
int  disk_valid_name(const char *name);
/* 1 if it is a partition (/sys/class/block/<n>/partition exists); its disk goes in disk. */
int  disk_parent(const char *name, char *disk, size_t n);
/* 1 if the (whole) disk holds something of the system: /, /usr, /var, /home, /boot, /boot/efi, the
 * live medium, an active swap, or a dm/md on top of it that holds any of those. */
int  disk_is_system(const char *disk);
/* 1 if removable: removable=1 in sysfs, connected over USB, or a card (mmc). */
int  disk_is_removable(const char *disk);
/* udev database property for that device ("ID_FS_TYPE"…); "" if missing. */
int  disk_udev_prop(const char *name, const char *key, char *out, size_t n);
/* File system label as is (ID_FS_LABEL_ENC with the \xNN decoded); "" if none. */
void disk_fs_label(const char *name, char *out, size_t n);
/* Size in bytes (/sys/class/block/<n>/size × 512). */
uint64_t disk_size(const char *name);
/* Model and short serial, exactly as disk_list_json() reports them (the imager's identity check). */
int  disk_identity(const char *name, char *model, size_t mn, char *serial, size_t sn);
/* Mount points of the device (separated by \n) from mountinfo; returns how many. */
int  disk_mounts(const char *name, char *out, size_t n);

/* List for JS: [{ name, model, vendor, serial, size, removable, transport, rotational, ro,
 * system, table, fstype, label, uuid, mountpoint, used, avail, partitions: [{ name, number, size,
 * fstype, label, uuid, partlabel, mountpoint, used, avail, start }] }]. malloc'd; NULL on failure. */
char *disk_list_json(void);
/* Format and rename programs present on this machine: {"fat32":1,"exfat":1,"ext4":1,"ntfs":0} */
char *disk_tools_json(void);

/* Change notifications while the app is open: one fd (epoll over mountinfo and udev events) for
 * the main loop. disk_watch(1) opens it, disk_watch(0) closes it; -1 when not open. */
int  disk_watch(int on);
int  disk_watch_fd(void);
/* Reads what arrived; 1 if disks or mounts changed (JS must be told). */
int  disk_watch_dispatch(void);

/* ---- SMART (smart.c, pure functions: tests/smart_test.c) ---- */

enum { SMART_UNKNOWN = 0, SMART_OK = 1, SMART_WARN = 2, SMART_FAIL = 3 };
enum { SMART_ATA = 1, SMART_NVME = 2 };

typedef struct {
    uint8_t id, value, worst, thresh;
    uint16_t flags;                     /* bit 0: pre-fail */
    uint64_t raw;                       /* 48 bits */
    int failing;                        /* value <= threshold (threshold != 0) */
} SmartAttr;

typedef struct {
    int kind;                           /* SMART_ATA / SMART_NVME */
    int health;                         /* SMART_OK… */
    int checksum_ok;                    /* ATA: the 512 bytes sum to 0 */
    int status;                         /* ATA RETURN STATUS: 1 good, 0 threshold exceeded, -1 unknown */
    int temp_c;                         /* -1 if unknown */
    int64_t power_on_hours, power_cycles;           /* -1 if unknown */
    int64_t reallocated, pending, uncorrectable;    /* ATA 5, 197, 198 (-1 if absent) */
    int nattr;
    SmartAttr attr[30];
    /* NVMe */
    int crit_warning, spare, spare_thresh, used_pct;
    uint64_t data_read, data_written, media_errors, unsafe_shutdowns;   /* data_*: bytes */
} SmartReport;

/* data = SMART READ DATA (512 bytes); thresh = READ THRESHOLDS (512) or NULL; status = 1/0/-1. */
int  smart_ata_parse(const uint8_t data[512], const uint8_t *thresh, int status, SmartReport *r);
/* log = NVMe log page 02h (SMART / Health Information, 512 bytes). */
int  smart_nvme_parse(const uint8_t log[512], SmartReport *r);
/* Summary as JSON (without the attributes): {kind, health, temp, hours, cycles, ...}. */
int  smart_summary_json(const SmartReport *r, char *out, size_t n);
/* One ATA attribute as JSON: {id, value, worst, thresh, raw, prefail, failing}. */
int  smart_attr_json(const SmartAttr *a, char *out, size_t n);

/* ---- smartctl (smartctl.c, pure: tests/smartctl_test.c) ---- */

/* sd<letters>, hd<letters> or nvme<N>n<M>: the whole disks the smart-* actions accept. */
int    smartctl_valid_disk(const char *name);
/* Drops the whitespace outside JSON strings, in place; returns the new length. */
size_t smartctl_compact(char *s, size_t n);
/* Length of the next LOG piece (at most max bytes, never inside a UTF-8 sequence). */
size_t smartctl_chunk(const char *s, size_t n, size_t max);

/* ---- helper actions (disk_admin.c, smartctl_admin.c) ---- */

/* The disk-* target() check for a whole disk (the system disk allowed): /dev path into dev, and
 * whether it is on USB. need_admin: an internal disk needs an administrator. -1 after admin_err. */
int  disk_admin_target(const AdminReq *r, const char *name, int need_admin, char *dev, size_t n, int *usb);
void smart_act_info(const AdminReq *r, const char *arg, size_t len);
void smart_act_health(const AdminReq *r, const char *arg, size_t len);
void smart_act_test(const AdminReq *r, const char *arg, size_t len);
void smart_act_abort(const AdminReq *r, const char *arg, size_t len);

/* ---- for the LordOS Imager's actions (imager/imager_admin.c), the same checks as disk-* ---- */
typedef struct {
    char name[32], disk[32], dev[48];
    int part, removable;
    uint64_t size;
    unsigned major, minor;
} DiskTarget;
/* A whole disk that is not the system disk (after admin_err on failure: -1). */
int  disk_admin_whole(const AdminReq *r, const char *name, DiskTarget *t);
/* Unmounts its partitions under /media or /run/media; refuses anything mounted elsewhere. */
int  disk_admin_unmount(const DiskTarget *t);
/* O_RDWR | O_EXCL | extra, with admin_err on failure. */
int  disk_admin_open_excl(const DiskTarget *t, int extra);
/* The user's image file: regular, from their home or their mounts, opened as them. */
int  disk_admin_open_user_file(const AdminReq *r, const char *path, struct stat *st);
int  disk_admin_wipe_ends(int fd, uint64_t size);
/* Cancellable job (disk-cancel), low priority; disk_admin_cancelled() after SIGTERM. */
void disk_admin_job_begin(const AdminReq *r, const char *disk);
void disk_admin_job_end(const char *disk);
int  disk_admin_cancelled(void);
void disk_admin_reread(const char *disk);     /* uevent "change" and udevadm settle */
/* The disk holding the file st describes is this one (an image can't be read from the disk being written). */
int  disk_admin_file_on(const struct stat *st, const char *disk);

void disk_act_smart(const AdminReq *r, const char *arg, size_t len);
void disk_act_mount(const AdminReq *r, const char *arg, size_t len);
void disk_act_unmount(const AdminReq *r, const char *arg, size_t len);
void disk_act_eject(const AdminReq *r, const char *arg, size_t len);
void disk_act_format(const AdminReq *r, const char *arg, size_t len);
void disk_act_label(const AdminReq *r, const char *arg, size_t len);
void disk_act_erase(const AdminReq *r, const char *arg, size_t len);
void disk_act_write_image(const AdminReq *r, const char *arg, size_t len);
void disk_act_cancel(const AdminReq *r, const char *arg, size_t len);

/* All are A_ANYONE: each one requires an administrator for internal disks (removable ones are
 * open to any session user, like udisks) and refuses the system disk. The smart-* ones
 * (smartmontools) accept the system disk: reading is open to anyone, a self-test on an internal disk
 * needs an administrator. */
#define DISK_ADMIN_ACTIONS \
    { "disk-smart", disk_act_smart, A_FORK | A_ANYONE }, \
    { "disk-mount", disk_act_mount, A_ANYONE }, \
    { "disk-unmount", disk_act_unmount, A_ANYONE }, \
    { "disk-eject", disk_act_eject, A_FORK | A_ANYONE }, \
    { "disk-format", disk_act_format, A_FORK | A_ANYONE }, \
    { "disk-label", disk_act_label, A_FORK | A_ANYONE }, \
    { "disk-erase", disk_act_erase, A_FORK | A_ANYONE }, \
    { "disk-write-image", disk_act_write_image, A_FORK | A_ANYONE }, \
    { "disk-cancel", disk_act_cancel, A_ANYONE }, \
    { "smart-info", smart_act_info, A_FORK | A_ANYONE }, \
    { "smart-health", smart_act_health, A_FORK | A_ANYONE }, \
    { "smart-test", smart_act_test, A_FORK | A_ANYONE }, \
    { "smart-abort", smart_act_abort, A_FORK | A_ANYONE },

#endif
