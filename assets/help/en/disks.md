---
title: Disks
app: disks
section: apps
order: 43
keywords: disks, usb stick, usb, format, partitions, smart, health, mount, eject, iso
---
# Disks

To inspect and prepare disks and USB sticks, like GNOME Disks.

## View

- Internal disks, external disks and images, with their partitions, how full they are and where
  they are mounted.
- **Health (SMART):** “Check health” reads what the disk reports: status, temperature, power-on
  hours, reallocated or pending sectors and, on NVMe, the life used. Virtual disks and many cards
  report nothing.

## Do

- **Mount**, **unmount** and **eject** a USB stick before pulling it out.
- **Format** a USB stick or partition as **FAT32** (works everywhere, files up to 4 GB), **exFAT**
  (large sticks, without that limit), **ext4** (Linux) or **NTFS** (for disks shared with other operating
  systems, if its tool is installed).
- **Rename** the volume (unmounted).
- **Erase:** quick (the table and signatures) or writing zeros everywhere.
- **Create a bootable USB** from an `.iso` or `.img` image: it is copied as is and then
  **verified** by reading the disk back.

## Safety

Before erasing anything, a dialog shows exactly what will be lost (disk, partitions, where they
are mounted and how much data they hold) and you have to type the disk's name. **The system disk
cannot be formatted or erased** from here. Internal disks can only be changed by an
administrator; USB sticks by any user.

An administrator can mount a **free partition of the system disk**, such as another operating
system's partition when both are installed on the same disk. Partitions in use (mounted, encrypted,
swap) and the ones the system keeps for itself (EFI, boot, reserved or recovery) are never offered.
If the other operating system was not shut down completely (fast startup or hibernation), its
partition is mounted read-only: shut it down fully to write to it.
