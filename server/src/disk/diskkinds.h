/* diskkinds.h — what disk-mount mounts: blkid's name, the kernel's, whether it takes owner options, read-only. */
#ifndef LORD_DISK_KINDS_H
#define LORD_DISK_KINDS_H

static const struct { const char *udev, *kernel; int owner, ro; } DISK_KINDS[] = {
    { "vfat", "vfat", 1, 0 }, { "exfat", "exfat", 1, 0 }, { "ntfs", "ntfs3", 1, 0 }, { "ext4", "ext4", 0, 0 },
    { "ext3", "ext3", 0, 0 }, { "ext2", "ext2", 0, 0 }, { "btrfs", "btrfs", 0, 0 }, { "xfs", "xfs", 0, 0 },
    { "f2fs", "f2fs", 0, 0 }, { "iso9660", "iso9660", 0, 1 }, { "udf", "udf", 0, 1 },
    { "hfsplus", "hfsplus", 1, 1 }, { "apfs", "apfs", 1, 1 },   /* HFS+ always read-only: its write path isn't trusted with a journal */
};

#endif
