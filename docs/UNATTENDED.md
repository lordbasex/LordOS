# Unattended install

The live ISO can install LordOS with nobody at the keyboard, from a configuration file. It is meant for test
machines and for installing the same setup many times. It is never the default: it starts only from its own
boot menu entry, and it stops, touching nothing, at the first thing that is missing or doesn't match.

- The program: `server/src/install/autoinstall.c` (`/usr/lib/lordos/bin/lord-autoinstall`), run by
  `lordos-autoinstall.service`.
- The install itself is install-run's own code (`server/src/install/install.c`, [ISO.md](ISO.md)): the same
  partitions, copy, configuration and boot loader as the graphical installer.
- Tests: `server/tests/autoinstall_test.c` and `server/tests/install_plan_test.c` (`make -C server test`).

## How it runs

1. Boot the live ISO and pick **Install automatically (lordos-auto.conf: erases the disk it names)** in the boot
   menu. The entry adds `lordos.auto=1` to the kernel command line, shows the boot as text (no splash) and also
   writes it to the first serial port.
2. `lordos-autoinstall.service` starts before the login screen. It runs only with `lordos.auto=1` and only on
   the live system.
3. It waits up to 20 seconds for a file system labelled **`LORDOSAUTO`** (FAT, ISO 9660 or ext4: a USB stick,
   a small second disk or a small ISO), mounts it read-only and reads **`lordos-auto.conf`** from its top
   folder. The file may be at most 16 KB.
4. It checks the file and the disk (below). Anything wrong: it prints `lordos-auto: STOP <reason>. Nothing was
   changed on any disk.`, and the live system starts as usual.
5. It installs, printing the installer's progress (`progress partition|copy|configure|bootloader <done>
   <total>`), then restarts, powers off or stays, as `after` says.

What it printed is on the screen, on the serial port, in `journalctl -u lordos-autoinstall` and, as one line
(`DONE …`, `FAILED <error code>` or `STOP <reason>`), in `/run/lordos-auto/result` until the next restart.

## lordos-auto.conf

`key = value` lines. Lines starting with `#` and empty lines are ignored. Every key has to be one of these:
a misspelled key stops the install instead of falling back to a default.

| Key | Required | Default | Meaning |
|---|---|---|---|
| `disk` | yes | | the target disk's name in `/dev`, such as `vda`, `sda` or `nvme0n1` |
| `disk_bytes` | yes | | that disk's exact size in bytes (`lsblk -bdno SIZE /dev/vda`). Another disk is never erased by mistake |
| `mode` | | `erase` | `erase`: the whole disk. `free`: beside another system, in the disk's largest free region ([ISO.md](ISO.md), "Install in free space") |
| `erase` | with `mode = erase` | | `yes`: says, in the file, that the disk gets wiped |
| `user` | yes | | the account's user name: lowercase letters, numbers, `-` and `_` |
| `fullname` | | the user name | the name shown on the login screen |
| `password_hash` | yes | | the password as a hash: yescrypt (`$y$…`) or SHA-512 (`$6$…`). The password itself never goes in the file |
| `ssh_key` | | | one public key line, written to the user's `~/.ssh/authorized_keys`. It works once remote login is turned on (Settings → Sharing): the image has no SSH server |
| `hostname` | yes | | the computer's name |
| `lang` | | `en` | the interface language: `en`, `es`, `pt` or `fr` |
| `locale` | | `en_US.UTF-8` | formats and Linux programs' language, from `/usr/share/i18n/SUPPORTED` |
| `timezone` | | `UTC` | such as `America/Argentina/Buenos_Aires` |
| `keyboard`, `keyboard_variant` | | `us`, none | the XKB layout and variant |
| `swap` | | `0` | a swap file, in MiB, up to 16384 |
| `autologin` | | `no` | `yes`: the user is signed in at boot, without the login screen (Settings → Users can turn it off) |
| `after` | | `reboot` | `reboot`, `poweroff` or `stay` (the live system starts) |

An example:

```
# LordOS unattended install
disk = vda
disk_bytes = 34359738368
mode = erase
erase = yes
user = alex
fullname = Alex Rivera
password_hash = $y$j9T$...
hostname = lordos-test
lang = en
locale = en_US.UTF-8
timezone = UTC
keyboard = us
autologin = yes
after = reboot
```

Making the hash, on any Debian or LordOS system: `mkpasswd -m yescrypt` (package `whois`) or
`openssl passwd -6`. Both ask for the password without showing it.

## What it checks before touching anything

- It was started from the boot entry (`lordos.auto=1`) on the live system.
- The `LORDOSAUTO` file system exists and `lordos-auto.conf` is a regular file of at most 16 KB.
- Every line is `key = value` with a known key; numbers are numbers.
- `/dev/<disk>` exists, is not the disk the live system started from or the one holding `lordos-auto.conf`,
  and has exactly `disk_bytes` bytes. The installer then checks it is in its own list of installable disks,
  at least 8 GiB, and not mounted.
- An erase install has `erase = yes`. An install in free space finds a region that qualifies.
- `password_hash` is a yescrypt or SHA-512 hash that `crypt(3)` accepts; `ssh_key` is one plain public key
  line (no `command=` or other options).
- Then every check of the graphical installer: user name, computer name, language, region, time zone and
  keyboard.

The fields only the unattended install may use (`passwordHash`, `sshKey`, `autologin` in install-run's
plan) are refused when the plan comes from anywhere else: the graphical installer can't send them.

## The LORDOSAUTO disk

`tools/iso/make-auto-disk.sh lordos-auto.conf lordos-auto.iso` makes a small ISO with the file, labelled
`LORDOSAUTO`, to attach to a virtual machine as a second CD drive. On a USB stick: a FAT file system labelled
`LORDOSAUTO` (`mkfs.vfat -n LORDOSAUTO /dev/sdX1`) with the file in its top folder.

## On a virtual machine (Proxmox)

1. A machine with the target disk, the live ISO and the `LORDOSAUTO` ISO as two CD drives, booting from the
   live ISO first.
2. Pick the entry in the boot menu (it is the last one).
3. After the restart, the live ISO's menu starts its default entry, which doesn't install: take the CD out,
   or put the disk first in the boot order.

## What it doesn't do

- It doesn't download anything: the system installed is the ISO's.
- The Raspberry Pi image has no installer, so it has no unattended install.
- No encrypted disk, no other partition layout and no second account.

## Test (November 3)

A development ISO of this branch on a Proxmox virtual machine (VM 127: 4 CPUs, 4 GB, a 32 GB VirtIO disk, SeaBIOS),
with the live ISO and a `LORDOSAUTO` ISO made by `make-auto-disk.sh` as two CD drives:

| Check | Result |
|---|---|
| `make -C server test` | `autoinstall-test` 18 checks and `install-plan-test` 40 checks pass; the suites 26 passed, 0 failed |
| The boot menu | Install automatically is the last entry; the menu still starts the live system by default |
| An erase install of `/dev/vda` from the file, `autologin = yes`, `after = reboot` | found the file in about 8 s, printed the plan, partitioned, copied, configured and installed the boot loader in 72 s, printed `DONE` and restarted; the whole log on the serial port |
| The installed system, started from the disk | reached the desktop signed in as the file's user, with no login screen |
| The same entry with `disk_bytes` one byte off | `STOP /dev/vda has 34359738368 bytes, lordos-auto.conf expects 34359738369`; the disk's partition table identical before and after, and the live system started |

Not tested yet: mode `free`, `ssh_key` with remote login turned on, `after = poweroff`, a USB stick with a FAT
`LORDOSAUTO` file system, UEFI firmware and the arm64 ISO.

