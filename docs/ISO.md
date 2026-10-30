# LordOS live ISO

An image to boot LordOS from a USB stick or a CD on any 64-bit PC, without installing anything. It
is the LordOS distribution itself (a minimal Debian 13 with the LordOS desktop environment) and
it boots straight into the desktop. It's for testing real hardware: the GPU (AMD or Nvidia),
sound, Wi-Fi and displays. It is the first step of M8 (the
installable image).

| | |
|---|---|
| ![bios](iso-bios-boot.png) | ![uefi](iso-final/live-about.png) |
| VM with SeaBIOS: boots on its own into LordOS | VM with OVMF (UEFI); screenshot from the final 0.7.0 ISO test |
| ![grub](iso-grub-menu.png) | ![terminal](iso-final/live-services.png) |
| Boot menu (screenshot from before the grub font fix: the borders showed up as "?") | Network via LordOS's own DHCP, DNS, `ping` without root, sound (PipeWire) and boot in 11.8 s (screenshot: services and network in the final 0.7.0 ISO test) |

## Writing it to a USB stick

The ISO is **hybrid**: it is written as is, without special tools. Careful: the whole USB stick is
erased.

- **Linux and other Unix-like systems:**
  ```sh
  sha256sum -c lordos-live-0.8.6-amd64.iso.sha256      # or: shasum -a 256 -c …
  sudo dd if=lordos-live-0.8.6-amd64.iso of=/dev/sdX bs=4M status=progress conv=fsync
  ```
  `/dev/sdX` is the whole USB stick (not a partition). On BSD-derived systems it is usually `/dev/rdiskN`: find
  it with the system disk tool and unmount it first.
- **Other systems:** Rufus in **DD** mode (when it asks "ISO or DD", choose DD) or balenaEtcher.
- **Ventoy** also works: copy the `.iso` onto the stick.

Then boot the PC from the USB stick: usually F12, F11, F8 or Esc at power-on, depending on the
motherboard. It works with **UEFI** and with **BIOS/Legacy**. **Secure Boot** has to be off (see
"Pending").

## Boot menu

| Entry | What for |
|---|---|
| LordOS (live) | normal, in English (the default language) |
| LordOS (safe mode: no GPU) | passes `lordos.gpu=0` → `LORD_GPU=0`: lord-server doesn't use the GPU path and composites on the CPU |
| LordOS (Español) / LordOS (Português) | `lordos.lang=es` / `lordos.lang=pt`: the interface in Spanish or Portuguese the first time |
| LordOS (copy to RAM) | `toram`: copies the system to memory (~1 GB free is needed) and the USB stick can be removed |

The GRUB menu itself is in English (GRUB's default). Every entry except safe mode boots with the
boot splash (`splash`); safe mode boots in text (`nosplash`).

## Firmware and displays

The ISO boots with **UEFI** and with **legacy BIOS** (a hybrid image from `grub-mkrescue`: El
Torito for BIOS and `efi.img` for UEFI). Secure Boot has to be off. For virtual machines the
smoothest setup is UEFI firmware, 4 GB of memory, 2–4 processors and the "Debian 64-bit" guest
type; legacy BIOS works too since 0.8.1.

**The 0.8.0 legacy BIOS bug.** On VMware Fusion with BIOS firmware (2 GB, 2 vCPUs, SVGA) the
machine triple-faulted when the menu's 5 s timeout fired; with UEFI it booted. On Proxmox with
SeaBIOS and the "VMware compatible" display, GRUB hung right after drawing the menu (the CPU
halted in real mode, keys did nothing); with standard VGA it booted. Cause, found by bisecting the
GRUB configuration on VM 123 with small test ISOs (kernel and initrd only):
- with BIOS, GRUB draws through VBE, and `gfxmode=auto` falls back to **640×480** when the
  display reports no EDID (both VMware's SVGA and QEMU's `vmware-svga`; VMware's log shows the
  640×480 mode being set right before the fault);
- at 640×480 GRUB's graphical terminal (`gfxterm`) corrupts GRUB's own memory when its font is
  **Inter 16**, the proportional font the theme used for `terminal-font`. Without the theme's fonts,
  with Inter 14 alone, at 800×600 or 1024×768, or with GRUB's Unifont as the terminal font, the
  same 640×480 boot works.

**Fix (0.8.1).** The theme's `terminal-font` is GRUB's fixed-width "GNU Unifont Regular 16"
(the command line, the entry editor and boot messages; the menu items keep Inter), and on BIOS
`grub.cfg` asks for `1024x768x32,800x600x32,auto` so the theme's layout fits (the crown no longer
starts above the top edge). UEFI keeps `gfxmode=auto` (GOP, native mode). The theme
(`tools/iso/grub/lordos/theme.txt`) is also the installed system's, so installed systems get the
fix through the `lordos` package; their `/etc/default/grub` keeps `GRUB_GFXMODE=auto`, which is
now safe at 640×480.

**No KMS display.** LordOS draws through kernel modesetting. QEMU's VMware-compatible display is
refused by `vmwgfx` ("unsupported hypervisor", "no pitchlock"), and Debian's kernel has no
`simpledrm`, so there is no `/dev/dri/card0` with any firmware: the boot ended on a black screen.
Since 0.8.1 `vmwgfx` loads only on VMware itself (`/etc/modprobe.d/lordos-vmwgfx.conf`, DMI vendor
check), so the firmware framebuffer stays, and lord-sessiond writes on the text console that no
display device with KMS was found and which display to choose instead. On VMware itself `vmwgfx`
works (the user's VMware machine reaches the desktop with UEFI and, since 0.8.1, BIOS).

**VMware guests** get `open-vm-tools` (shutdown and restart from the hypervisor, time sync, the
guest's IP); its units have `ConditionVirtualization=vmware` and stay inactive elsewhere
(checked on Proxmox: "unmet condition"). Shared clipboard and automatic resize need the desktop
part (`open-vm-tools-desktop`, X11) and are not supported yet. GPU or CPU composition on VMware:
see `GPU.md`.

**Typing lag in Chromium on VMware (0.8.0 and the first 0.8.1 ISOs):** it was a GPU-path
presentation bug, not `open-vm-tools`, and it is fixed in 0.8.1. See
[TROUBLESHOOTING.md](TROUBLESHOOTING.md), "Typing feels slow"; `tools/diag/keylag.sh` checks a
machine in two minutes.

Tested with the rebuilt ISO on VM 123 (Proxmox, q35, 4 vCPUs, emulated HD Audio):

| Firmware | Display | RAM | Result |
|---|---|---|---|
| SeaBIOS | VMware compatible | 2 GB | menu at 1024×768 ([screenshot](boot/bios-menu-1024.png)), the timeout boots the kernel (0.8.0: GRUB hung at the menu); no KMS device, so the text console explains it ([screenshot](boot/bios-no-kms-notice.png)) |
| SeaBIOS | standard VGA | 2 GB | menu, splash, desktop ([screenshot](boot/bios-std-2g-desktop.png)); 1.6 GB available after login; `open-vm-tools` inactive (unmet condition); sink ERR 0 with audio playing from ~1 to ~5 minutes after login and 2 CPU burners |
| SeaBIOS | standard VGA | 4 GB | desktop |
| SeaBIOS | standard VGA | 2 GB | "safe mode: no GPU" entry: desktop |
| OVMF (UEFI) | VirtIO GPU | 2 GB | menu at the native 1280×800 with the theme, splash with its bar, desktop: as in 0.8.0 |
| VMware Fusion, BIOS | SVGA | 2 GB | 0.8.0: triple fault; 0.8.1: not re-tested on VMware here (the same GRUB code path as the QEMU case) |
| VMware Fusion, UEFI | SVGA | 4 GB | 0.8.0 already boots to the desktop and the installer (the user's machine) |

Not tested: installing to disk on a BIOS machine with this ISO (no free test disk; the installer
writes both BIOS and UEFI GRUB, as tested on VM 117 in 0.7.0).

## Boot splash and boot menu

From power-on to the login screen LordOS shows its own look: the boot menu, then the crown with a
progress bar, then the login screen (or the desktop, with autologin) without a black frame or console
text in between.

| Boot menu (GRUB theme) | Boot splash (Plymouth) |
|---|---|
| ![Boot menu](boot/iso-grub.png) | ![Boot splash](boot/iso-splash.png) |

- **Boot menu:** the GRUB theme `tools/iso/grub/lordos/` (`theme.txt`, images and Inter as GRUB
  fonts): the navy background, a small crown, the entries in the brand colours with the selected one
  on a rounded violet band,. The ISO copies it to
  `/boot/grub/themes/lordos/`; the package installs it in `/usr/share/grub/themes/lordos/` (amd64
  only) and the disk installer sets `GRUB_THEME` to it. The entries (English, Español, Português,
  safe mode, copy to RAM) are the same as before.
- **Boot splash:** the Plymouth theme `lordos` (`tools/boot/plymouth/lordos/`, the `script`
  module), installed by the package in `/usr/share/plymouth/themes/lordos/`. The ISO installs
  `plymouth` and `plymouth-label` and selects it (`plymouth-set-default-theme lordos`) before building
  the initramfs, so it shows from the initramfs on. The bar follows Plymouth's boot progress, eased so
  it never jumps or goes back; there are no text messages, and **Esc** shows the text console as
  usual. If a disk is encrypted (LUKS), a styled password field with a lock replaces the bar.
  Since 0.8.5 the bottom right corner shows the time since the splash started, in seconds and
  milliseconds (it never goes backwards); the time from the kernel's start to LordOS's first frame
  is logged by lord-sessiond on every boot (`journalctl -b -u lordos | grep "first frame"`, kept in
  `/var/lib/lordos/boot-times.jsonl`, the last 200 boots) and shown in About LordOS → Startup time
  (`RESULTS.md`, "Boot time").
- **Installed system:** the installer writes `GRUB_CMDLINE_LINUX_DEFAULT="quiet splash
  vt.global_cursor_default=0 loglevel=3"`, `GRUB_GFXPAYLOAD_LINUX=keep`, `GRUB_TERMINAL_OUTPUT=gfxterm`
  and `GRUB_THEME` (each only when Plymouth and the themes are in the copied system), then rebuilds
  the initramfs and the GRUB menu. `nosplash` on the kernel command line (`e` in the menu) boots in
  text for one boot.
- **Hand-off:** see `SESSIONS.md`, "Boot splash": Plymouth gives up the card just before
  lord-sessiond takes it, its last frame stays until LordOS's first frame replaces it, and only then
  is Plymouth ended with `--retain-splash`.
- **On Debian (install.sh):** optional and asked for; see `INSTALL-DEBIAN.md`.

**Tested on VM 117** (OVMF, `vga: virtio`, 2026-10-18), recording the screen at 4 frames per second
over QMP (`tools/demo/record.py`): the boot menu, ~2.5 s of black while the kernel loads virtio-gpu
and Plymouth starts, the splash from ~19 s, and at ~43 s the desktop (autologin) replaces the
splash directly: no black or console frame between the last splash frame and the first LordOS one.
The journal (of the first test boot, the same hand-off code) shows the chain: `lordos.service` started (21.2 s), lord-sessiond "first frame on the
monitor" (27.9 s), `lordos-splash-quit.service` finished, then `plymouth-quit.service`.

| Last splash frame | Next frame: LordOS | The journal |
|---|---|---|
| ![Splash before the hand-off](boot/iso-handoff-1-splash.png) | ![Desktop after the hand-off](boot/iso-handoff-2-desktop.png) | ![Journal](boot/iso-handoff-journal.png) |

Not done yet: while GRUB loads the kernel it clears its terminal area to black for about a second
(visible in the recording; a known issue of 0.8.5-beta: on legacy BIOS, which reads the disc slowly,
the black rectangle inside the theme's background lasts a few seconds, seen on QEMU with SeaBIOS; 0.8.4's
ISO does the same, and neither a background image in each entry nor a hidden countdown in the theme
changed it); and an encrypted-disk (LUKS) boot was not tested on a VM, only the
static preview of the password field (`docs/boot/splash-preview-password.png`).

The images come from `python3 tools/boot/gen-boot-assets.py` (rsvg-convert; `--preview` also
writes the static previews in `docs/boot/`). The GRUB fonts are made on a Debian with `grub-common`:

```sh
R=0x20-0x7E,0xA0-0x17F,0x2010-0x2027
grub-mkfont -s 16 -r $R -o tools/iso/grub/lordos/inter-regular-16.pf2  assets/fonts/Inter-Regular.ttf
grub-mkfont -s 14 -r $R -o tools/iso/grub/lordos/inter-regular-14.pf2  assets/fonts/Inter-Regular.ttf
grub-mkfont -s 16 -r $R -o tools/iso/grub/lordos/inter-semibold-16.pf2 assets/fonts/Inter-SemiBold.ttf
```

Inter is under the SIL Open Font License 1.1 (`assets/fonts/Inter-LICENSE.txt`).

In the live session:
- **User:** `lordos`, no password, with passwordless `sudo`.
- **root:** password `lordos`, only for the emergency console.
- **Changes:** everything you change stays in RAM and is lost on power-off. The Store and package
  installation work the same, but in memory.

## Installing to disk

On the live ISO the **Install LordOS** assistant (`apps/installer`) opens on its own. It only exists
on the live system: the manifest has `"live": true` and the SDK hides it if
`/run/live/medium/live/filesystem.squashfs` is not there.

| | |
|---|---|
| ![welcome](iso-installer-welcome.png) | ![disk](iso-installer-disk.png) |
| Welcome | Disk: model, size, contents and a strong confirmation |
| ![user](iso-installer-user.png) | ![progress](iso-installer-progress.png) |
| User, password and computer name, with validation | Installing: progress bar by phase |
| ![installed](iso-installer-installed.png) | ![terminal](iso-installer-terminal.png) |
| Booted from the disk (UEFI), with the created user | Groups, fstab with UUIDs, es_AR, time zone, no live-boot, network; sudo asks for a password |
| ![bios](iso-installer-bios.png) | ![gpu](iso-live-gpu.png) |
| The same disk booting through SeaBIOS (BIOS + GPT) | The ISO ships the GPU path (`GPU=1`); in the VM (virtio without 3D) it falls back to the CPU on its own |

Assistant steps:

1. **Welcome.**
2. **Language, region and time zone.** The language changes the interface live.
3. **Keyboard.** The layout is applied live and there's a field to try it.
4. **Disk.** Lists the disks (model, size, connection and what's on them), excluding the disk the
   live system booted from. To continue you have to tick "I understand the whole disk will be
   erased" and type the disk's name (`sda`). Optional: swap as a file, of 2, 4 or 8 GB.
5. **User.**
   - The user name is suggested from the full name.
   - The password is typed twice.
   - The computer name is suggested as `<user>-pc`.
   - The same rules are validated again later, on the root side.
6. **Summary** and "Install".
7. **Progress**, in four phases:
   - partitioning;
   - copying the system, with the percentage reported by `unsquashfs`;
   - configuring;
   - installing the bootloader.

   At the end "Restart" appears.

How the disk ends up:

| Partition | Size | What for |
|---|---|---|
| 1 `biosboot` (EF02) | 1 MiB | grub for BIOS on a GPT disk |
| 2 `EFI` (EF00) | 512 MiB, FAT32 | grub for UEFI, at the removable path (`EFI/BOOT/BOOTX64.EFI`: boots on any firmware without an NVRAM entry) |
| 3 `LordOS` (8304) | the rest, ext4 | the system; swap is `/swapfile` |

The installed system is a copy of the live system with these changes:

- **Live-only things removed:**
  - `live-boot` and the `lordos` user are removed, along with its passwordless sudo;
  - root is locked (`*`).
- **The new user** is in the `sudo`, `audio`, `video`, `input` and `render` groups, and has
  linger, so their PipeWire starts with the system.
- **Configuration** that gets written:
  - `fstab` with UUIDs;
  - hostname and `/etc/hosts`;
  - `/etc/default/locale`, `/etc/localtime` and `/etc/default/keyboard`;
  - the user's LordOS settings (language and keyboard);
  - a new `machine-id`;
  - the regenerated initramfs and `grub.cfg`.
- **Login:**
  - `lordos.service` starts `lord-sessiond`, which shows LordOS's own login screen
    (`SESSIONS.md`): the installer writes an empty `AUTOLOGIN=` to `/etc/lordos/session.conf`;
  - automatic login is turned on in Settings → Users;
  - the password is used for `sudo` and for administration actions.

### Installer security

It adds three actions to the privileged helper (`server/src/install/install.c`, macro
`INSTALL_ADMIN_ACTIONS` in `admin.c`):

| Action | Who | What it does |
|---|---|---|
| `install-disks` | administrator | lists the installable disks (JSON via `LOG`) |
| `install-run <json>` | administrator, in a separate process | installs |
| `install-reboot` | administrator | `systemctl reboot` |

All three answer **ERR outside live mode**: `/run/live/medium/live/filesystem.squashfs` has to
exist. On an installed system that file doesn't exist.

What `install-run` validates on the root side, without trusting the interface:

- **The disk:**
  - it has to be in the same list as `install-disks` (from `/sys/block`, without loop, ram, zram,
    sr, dm, md or nbd, without read-only ones and without the disk that contains
    `/run/live/medium`);
  - it has to be at least 8 GB;
  - it can't have anything mounted or active swap.
- **User:**
  - `^[a-z_][a-z0-9_-]{0,31}$`;
  - it can't be a reserved name or a user that already exists on the system.
- **Full name:** UTF-8, without `:`, `,` or control characters.
- **Computer name:** `^[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?$`.
- **Region:** it has to appear in `/usr/share/i18n/SUPPORTED` with UTF-8.
- **Time zone:** a regular file in `/usr/share/zoneinfo`, without `..`.
- **Keyboard:** the layout has to exist in `/usr/share/X11/xkb/symbols`; the variant,
  `[A-Za-z0-9_-]`.
- **Swap:** from 0 to 16384 MiB.
- **The password:**
  - never goes into an argv: it arrives through the helper's socketpair;
  - it is hashed with `crypt(3)` yescrypt (`$y$`, like Debian, with `crypt_gensalt_ra`);
  - the hash is written directly to the new system's `/etc/shadow`;
  - afterwards it is wiped from memory (`explicit_bzero`), and the interface also forgets it when
    starting.
- **Fixed-path programs:** `wipefs`, `sgdisk`, `udevadm`, `mkfs.vfat`, `mkfs.ext4`, `unsquashfs`,
  `blkid`, `fallocate`, `mkswap`, `chroot` (with `dpkg`, `userdel`, `useradd`, `locale-gen`,
  `systemd-machine-id-setup`, `update-initramfs`, `grub-install` and `grub-mkconfig` inside) and
  `systemctl`. `install.c` builds the arguments from the already validated values.

## What it ships

- **LordOS** as its own package, `lordos_<version>_amd64.deb`, built by `tools/iso/mkdeb.sh`. The
  version and the release channel come from `package.json` (`"version"` and `"lordos": {"channel": "beta"}`;
  an empty channel is a stable release). `make -C server` bakes both, plus the build date and git
  commit, into `lord-server`, and About and Settings → General show them. The package contains:
  - `/usr/lib/lordos/bin/lord-sessiond` (root: sessions, login screen, remote desktop off, port
    5673) and `/usr/lib/lordos/bin/lord-server` (each session);
  - the prebuilt themes, in `/usr/lib/lordos/themes`;
  - `assets/`: fonts, texts, icons and the shell integration;
  - `lordos.service`.
- **Boot:** `lordos.service` starts `lord-sessiond` on tty1 as root.
  - `/etc/lordos/session.conf` says `AUTOLOGIN=lordos`: it logs in on its own, as before. The user
    has no password; after locking the screen you get back in with the field empty (only on the
    physical display).
  - Before that, `lordos-live-prep` (`ExecStartPre`, only if `/run/live` exists) applies
    `lordos.lang` (to `/etc/default/locale` and the user's settings), creates their folders and
    leaves `/etc/network/interfaces` with only `lo`.
  - `lordos.gpu=0` (safe mode) still works: lord-sessiond reads it from `/proc/cmdline` and passes
    `LORD_GPU=0` to every session. Without that option, the session on the display composites on
    the GPU as before (Mesa: EGL, GBM, GLES2, linux-dmabuf and direct scanout).
  - The user has linger, so their PipeWire starts with the system.
  - The system user `lordos-greeter`, the login screen's user, is created when building the ISO
    (lord-sessiond also creates it if it's missing).
- **Network:** LordOS's own DHCP, in `lord-netd` from boot (`NETWORK.md`). No ifupdown,
  systemd-networkd or NetworkManager.
- **Sound:** PipeWire, pipewire-pulse, WirePlumber, alsa-ucm-conf and the SOF firmware (audio on
  Intel laptops).
- **GPU:** Mesa (radeonsi, iris, nouveau, llvmpipe and RADV/ANV/NVK for Vulkan), `libgbm1`,
  `libegl1`, `libgles2`, `firmware-amd-graphics` and `firmware-misc-nonfree`. To test it:
  `glmark2-es2-wayland`, `eglinfo`/`glxinfo` (mesa-utils) and `vulkaninfo`.
- **Wi-Fi:** wpa_supplicant and Intel (iwlwifi), Realtek and Atheros firmware.
- **Fonts:** Noto (core, CJK, emoji) and DejaVu; the rest of what M5b needs: libthai-data, skkdic
  and rime-data-pinyin-simp.
- **Linux apps:** Chromium and Firefox ESR (preinstalled browsers that draw their own bar with
  LordOS's window buttons, `WAYLAND.md`), plus foot and FeatherPad as samples.
- **X11 apps (0.8.3):** `xwayland`, started by lord-server only when an X11 app connects
  ([WAYLAND.md](WAYLAND.md), "X11 apps (Xwayland)").

## Building it

On a Debian 13, from the root of an already compiled repository. Since M8 it is built on **VM 116**
(`lordos-gpu`, 172.17.0.52, a clone of 115), in `~/LordOS-iso`: 115 is the user's development VM and
is not used for this.

```sh
# on the development machine: the themes (also the login screen, themes/greeter)
npm run build
COPYFILE_DISABLE=1 tar czf - --no-xattrs --exclude build --exclude node_modules \
  package.json README.md server third_party assets themes tools | \
  ssh lord@172.17.0.52 'rm -rf ~/LordOS-iso && mkdir ~/LordOS-iso && cd ~/LordOS-iso && tar xzf -'
# on VM 116
make -C server -j4                            # lord-server (GPU=1) and lord-sessiond (needs libssl-dev, libsrtp2-dev, libusrsctp-dev)
sudo tools/iso/build.sh /var/tmp/lordos-iso   # → /var/tmp/lordos-iso/out/
```

The ISO is copied to Proxmox (`local:iso/lordos-live-<version>-amd64.iso`). Before overwriting it, the
previous one is kept with its date: `lordos-live-0.7.0-amd64-20261016-1529.iso` is the last one with
`lordos-session` (sha256 `d4a8b25f…4b4dc5`).

`build.sh` steps:
1. `mkdeb.sh`: the LordOS package.
2. `debootstrap --variant=minbase` of trixie, with main, contrib, non-free and non-free-firmware.
3. `apt-get install --no-install-recommends` of the list above and the LordOS package.
4. Configuration:
   - locales `es_ES`, `es_AR`, `es_MX`, `en_US` and `pt_BR`;
   - the `lordos` user, with linger;
   - `lordos.service` enabled and `getty@tty1` masked;
   - `/etc/resolv.conf` as a regular file, for LordOS's DHCP;
   - `/etc/network/interfaces` with only `lo`;
   - `net.ipv4.ping_group_range`, so `ping` works without root.
5. Cleanup: apt lists, caches and `machine-id`.
6. `mksquashfs` (xz, BCJ x86).
7. `grub-mkrescue`: a single ISO with El Torito for BIOS and an EFI image (`efi.img`) for UEFI, with
   the hybrid MBR for USB.

lord-server is compiled with `make -C server GPU=1` (it needs `libgbm-dev libegl-dev libgles-dev`) so
the ISO ships GPU compositing (Mesa; see `GPU.md`).

Measured on VM 116 (4 Xeon Silver 4108 cores, 4 GB) for 0.7.0: **~8 minutes** (14.5 minutes for 0.8.0, see
"Release test (0.8.0 Beta)"), almost all of it downloading and
installing packages and compressing.

| | |
|---|---|
| ISO | 651 MB (650 809 344 bytes; with lord-sessiond, the login screen, the installer and the GPU path) |
| squashfs | 545 MB |
| uncompressed system | 1.5 GB |
| `lordos_0.7.0_amd64.deb` | 2.9 MB |

### Build packages

```sh
sudo apt-get install -y --no-install-recommends debootstrap squashfs-tools xorriso grub-pc-bin \
  grub-efi-amd64-bin grub-common mtools dosfstools lintian libgbm-dev libegl-dev libgles-dev libssl-dev \
  libpoppler-glib-dev libarchive-dev libavcodec-dev libavformat-dev libavutil-dev libswscale-dev \
  libswresample-dev libva-dev libsrtp2-dev libusrsctp-dev clang
```

| Package | What for |
|---|---|
| `libssl-dev` | compiling `lord-sessiond` (remote desktop TLS), `lord-maild` (Mail) and, since 0.8.5, `lord-fetch` (the blocklists) and `lord-tls` (the package engine's HTTPS); without it, `make` builds only lord-server and `mkdeb.sh` refuses |
| `libpoppler-glib-dev`, `libarchive-dev` | compiling `lord-doc` (PDF and archives); without them `make` skips it and the package ships without a PDF viewer |
| `libavcodec-dev`, `libavformat-dev`, `libavutil-dev`, `libswscale-dev`, `libswresample-dev` | compiling `lord-media` (video, photos, camera, recorder); without them `make` skips it |
| `libva-dev` | optional: GPU decoding (VA-API) in `lord-media` |
| `libsrtp2-dev`, `libusrsctp-dev` | the remote desktop (WebRTC: SRTP and the data channels) in `lord-sessiond`; required: without them `make` stops with an error for `lord-sessiond` (or build with `SESSIOND=0`), and the package always depends on `libsrtp2-1` and `libusrsctp2` |
| `clang` | compiling `lord-netperm`'s BPF programs (per-app network permissions); without it the build still works, but no rules are applied |
| `debootstrap` | the Debian base system |
| `squashfs-tools` | `mksquashfs` |
| `xorriso`, `grub-pc-bin`, `grub-efi-amd64-bin`, `grub-common`, `mtools`, `dosfstools` | `grub-mkrescue`: the hybrid BIOS + UEFI ISO |
| `lintian` | checking the `.deb` |

### lintian

`lintian lordos_0.7.0_amd64.deb` reported no errors and two warnings, on purpose (the later release
tests list theirs: 0 errors and 17 known warnings for 0.8.4.1). The two:

- **`duplicate-font-file`:** JetBrains Mono ships in `assets/fonts` because the Terminal needs it even
  when `fonts-jetbrains-mono` isn't installed.
- **`uses-dpkg-database-directly`:** our own package engine reads `/var/lib/dpkg/status` (see
  `PACKAGES.md`); it doesn't write the dpkg database.

## Test with lord-sessiond (M8, October 16)

The ISO starts `lord-sessiond` instead of `lordos-session`. ISO tested:
`lordos-live-0.7.0-amd64.iso`, 650 817 536 bytes, sha256
`7cf1c1b1d97ee040e74dcf8d93a6996f1ae369b457f51aba23cdd2bdd64e898c`.

| | |
|---|---|
| ![grub](iso-grub-m8.png) | ![desktop](iso-desktop.png) |
| UEFI (OVMF): grub menu | Logs straight into the desktop, without going through the login screen |
| ![terminal](iso-terminal-m8.png) | ![locked](iso-final/live-locked.png) |
| `lordos`, `AUTOLOGIN=lordos`, `/sys/firmware/efi`, network via DHCP, port 5673 closed | ❖L: locked screen, with the IP at the bottom |
| ![unlocked](iso-session-unlocked.png) | ![sharing](iso-final/live-settings-sharing.png) |
| Enter with the field empty: back to the same session | Settings → Sharing: remote desktop off (there's no `/etc/lordos/remote/config`) |
| ![safe grub](iso-grub-safe-mode.png) | ![safe mode](iso-safe-mode.png) |
| "Safe mode" entry | `lordos.gpu=0` → the session receives `LORD_GPU=0` and composites on the CPU |
| ![english](iso-english.png) | ![disk](iso-installer-disk-m8.png) |
| `lordos.lang=en`: the interface in English | Installer, in a temporary VM with an empty 24 GB disk (OVMF) |
| ![user](iso-installer-user-m8.png) | ![summary](iso-installer-summary-m8.png) |
| User `jordan` / «Jordan Lee», computer `jordan-pc` | Summary, with the new login text |
| ![progress](iso-installer-progress-m8.png) | ![done](iso-installer-ready-m8.png) |
| Installing (~1 minute) | Done; "Restart" with one click |
| ![greeter](iso-final/installed-greeter.png) | ![session](iso-final/installed-desktop.png) |
| The installed system boots to the login screen | Logging in with the new user's password (final 0.7.0 ISO test) |
| ![installed terminal](iso-final/installed-terminal.png) | |
| Empty `AUTOLOGIN=`; no `live-boot` or `lordos` user; groups sudo, audio, video, render and input; network; UEFI | |

**Fixes found in this test:**

- **The network didn't come up.** With lord-sessiond, every session is `--headless`, including the
  one on the display. `main.c` only sent `net-restore` without `--headless`, so `ens18` stayed DOWN.
  - Now every user session sends `net-restore`.
  - `net_act_restore` (`server/src/net/actions.c`) records in `/run/lordos/net-owner` the pid of the
    helper that brought the network up. While that helper is alive, the other sessions don't start
    another DHCP on the same interface.
- **The installer summary said** the system logged in on its own and that the login screen would
  come "in an upcoming version". `installer.summary.login` was changed in es, en and pt.

The installer screenshots up to "Done" and those of the installed system are from the ISO before that
text was fixed (sha256 `daba6313…c8ec65`). That ISO has the same binaries; only those texts change.
With the final ISO, UEFI boot, the network, ❖L/Enter and the installer summary were repeated. Nothing
was installed in that repeat.

## Release test (0.8.6 Beta, October 30)

`lordos-live-0.8.6-amd64.iso` (1,054,357,504 bytes, sha256
`3de95e586041de3a8d0a8dbea11970600468a1988527862efac5a679568b7efe`) and the Raspberry Pi image `lordos-0.8.6-rpi5-arm64.img.xz` (783,667,748 bytes, sha256
`a81a2e068870099ac70aef8eaa08d0f1e33c0b3825f18f231f2e43ee251ea613`) are the 0.8.6-beta release files, from the tag `v0.8.6-beta`, built on VM 116 with `GPU=1 RELEASE=1`
from a signed tree. What was tested before the release:

| Test | Result |
|---|---|
| Development build of the tag's code on VM 116 (not rerun on the final ISO or on the Pi) | `make test`: 23 passed, 0 failed, 13 skipped (12 need node and ran on the maintainer's machine, 12/12; `bidi-test` needs its data file); `engine-test` 7,010 checks; `defaults-test` 38/38; `test-full` 7 passed |
| Update 0.8.5 → 0.8.6 with `apt` against lordos.org/apt-staging, a live-session amd64 VM (no restart is possible there) | no migration ran, settings files unchanged |
| Update 0.8.5 → 0.8.6 with `apt` against lordos.org/apt-staging, Raspberry Pi 5, then a restart | no migration ran, settings files unchanged; came back with 0.8.6-beta build 20261030, first frame 7.63 s after the kernel, a release build with signed code |

The Store's own package engine and the Store → Updates screen were not exercised in these updates, and a second
reinstall was not run.

The final `lordos-live-0.8.6-amd64.iso` booted once under QEMU/KVM on VM 116 (BIOS, 1.8 GB of memory, 2 cores, virtio display) to the live desktop with the installer open, on its own. That is the whole boot test: UEFI, an install from it, and a real PC were not tried with this file.

Known issues of the release that concern the image: the legacy BIOS black rectangle between the boot
menu and the splash, as in 0.8.5 ("Boot splash and boot menu" above). The others are in the CHANGELOG.

## Release test (0.8.5 Beta, October 27)

`lordos-live-0.8.5-amd64.iso` and the Raspberry Pi image `lordos-0.8.5-rpi5-arm64.img.xz` are the
0.8.5-beta release files. What the CHANGELOG records as tested before the release:

| Test | Result |
|---|---|
| Update with apt, the same package built as `0.8.5~rc2`: a VMware machine on 0.8.4.1 and a Raspberry Pi 5 on 0.8.4, then a restart | no migration (`/var/lib/lordos/migrations` unchanged from 0.8.4), settings kept |
| Update 0.8.4.1 → 0.8.5 through LordOS's package engine against lordos.org/apt-staging, VMware Fusion guest (`results/0.8.5-beta/vmware-fusion/`) | first frame on the monitor 7.4 s after the kernel started; GPU composition on SVGA3D; Test Bench 59.8 frames a second at 60 Hz |

Known issues of the release that concern the image: the legacy BIOS black rectangle between the boot
menu and the splash ("Boot splash and boot menu" above). The others (the remote desktop's black corners
on a dragged window, llama.cpp on a GPU not tested on real hardware, the "displays" search, a small
display's name cut in Settings → Displays) are in the CHANGELOG.

## Release test (0.8.4.1 Beta, October 25)

An update of 0.8.4 (no new ISO: the 0.8.4 ISO stays the current one). `lordos_0.8.4.1_amd64.deb` built
on VM 116 (build `20261025.9796cda3`) and `lordos_0.8.4.1_arm64.deb` with the Raspberry Pi 5 image on
the Pi itself, natively, in `build-img.sh`'s arm64 chroot (build `20261025.5adba3b7`; the two builds
differ only in `tools/rpi/build-img.sh`), both `RELEASE=1` from a tree whose `code-manifest.json` was
signed on the maintainer's machine; `lord-server --check-code`: verified, release build.
`make -C server test`: 23 passed, 0 failed, 8 skipped on VM 116 and 21 passed, 0 failed, 11 skipped in
the Pi's arm64 chroot (`settings-hw-test` 160/160 on both, with the new `rpi-eeprom-update` parser
cases); the seven node suites pass on the maintainer's machine; 0 compiler warnings. Both `.deb`:
lintian 0 errors (17 warnings, the known ones); both SBOMs validated with `pyspdxtools`.

The image: `lordos-0.8.4.1-rpi5-arm64.img.xz`, 784 982 908 bytes, sha256
`da398fc6ee1cad611a07e34399a018250ab403a525157aa50a52172293d0e0da`, signed into its own
`SHA256SUMS`/`SHA256SUMS.sig` with the release key. Against 0.8.4's image: 533 → 749 MiB compressed,
2 747 → 3 674 MiB raw (Chromium, Firefox ESR and the ISO's system packages); the system is 2.3 GB.

| Check | Result |
|---|---|
| Image, read-only (loop device on the Pi): `config.txt` has `display_auto_detect`/`camera_auto_detect` (lines 15–16) **before** `dtoverlay=vc4-kms-v3d` (line 21); `lordos` 0.8.4.1 verified, release build; Chromium 156 and Firefox ESR 153 (Debian's) with `custom_chrome_frame` true, the GTK theme and `inTitlebar` 1; `systemd-timesyncd`, `cups`, `avahi-daemon`, `exfatprogs`, `ntfs-3g`, `power-profiles-daemon`, `gsettings-desktop-schemas`, `rpi-eeprom` 28.34-1 installed; `lord-doc`, `lord-media` and `lord-netperm` with its BPF programs in the package; `dpkg --audit` clean | pass |
| Store → Updates on VM 120 (0.8.4 installed from its ISO, UEFI, user `rtester`, `lordos.sources` on `apt-staging`): "LordOS 0.8.4.1 is available" with the notes; the plan is `lordos` only (11 MB); Update → no migration → "Restart required" → Restart (the sheet with the open apps) → login screen → About "Version 0.8.4.1 · Beta", build `20261025.9796cda3`, **Verified**; `lordos`, `lord-netperm`, `lord-fand` active, 0 failed units; Settings has no Board firmware on a PC (search "firmware": no results) | pass |
| `apt update && apt-get upgrade` (VM 120 back at the fresh 0.8.4): 27 s, `lordos` and a Debian update, nothing kept back, no migration, `settings.json` byte-identical, `/run/reboot-required.pkgs` = `lordos`; `apt install --reinstall lordos` runs no migration; after the restart 0 failed units | pass |
| Staging repository: the 11 Raspberry Pi packages verified against Raspberry Pi's signed index and the pinned SHA-256s; only the arm64 index lists them; `apt-repo.sh verify staging` passes. On the Pi, `apt-get install -s rpi-eeprom` with only Debian and LordOS sources (Raspberry Pi's archive left out, empty dpkg status) resolves everything from `LordOS:stable` and Debian | pass |
| Raspberry Pi 5 (test SD, 0.8.4 image with the `config.txt` fix by hand): `apt-get install lordos` from staging: 0.8.4.1 in 18 s, no migration, `sshd -t` clean, `lord-server --check-code` verified | pass |
| Settings → Board firmware on the Pi (a test account over the LordOS remote desktop): "Update available", installed Tuesday, May 26, 2026, latest Friday, September 25, 2026, release Default; **Update** → "Update ready: restart to finish" with the note and **Restart…** (the session's Restart sheet); the board staged `pieeprom.upd`, `pieeprom.sig` and `recovery.bin` and wrote `/run/rpi-eeprom-update-pending` (no `flashrom`, no `rpieepromab`) | pass |
| After the restart: `vcgencmd bootloader_version` 2026/09/25 (`1accd665…`, was 2026/05/26 `086b83e3…`), the staged files gone; the page says "Up to date", installed = latest = September 25, 2026 | pass |
| Browsers on the Pi (installed on the test SD the same way as the image, defaults from `browser-defaults.sh`): Chromium 156 on Wayland with one bar and LordOS's traffic lights; Firefox ESR 153 with its own single bar; Settings → Displays → Video: "Automatic", this board can't decode VP9 or AV1 in hardware, so sites are asked for H.264 | pass |

Found in this test:

- **The first arm64 package was incomplete.** Built in `build-img.sh`'s arm64 chroot (0.8.4's was built
  natively on the development SD), it had no `lord-doc`, no `lord-media` and no BPF programs in
  `lord-netperm`: the chroot lacked Poppler, libarchive, FFmpeg and clang, and `make` leaves those
  parts out silently. It was published to staging only, then replaced there (the CDN path
  invalidated). `build-img.sh` now installs the ISO's build packages in the chroot, copies
  `packaging/`, and refuses a package without `lord-doc`, `lord-media` or the BPF programs.
- **The Pi image had no network time client** (`systemd-timesyncd`): the board starts months behind
  and refuses every signed index ("not valid yet"); the test SD needed its clock set by hand. Added.
- **Restart from a session that is not on the monitor** (here the remote desktop) shows the Restart
  sheet, but `lord-sessiond` only accepts power requests from the session shown on the monitor, so
  nothing happens after the countdown; the board was restarted with `systemctl reboot`. The same
  holds for Store → Updates' Restart Now; to fix in a later version (`canPower()` should be false
  there).
- **The Store's package engine ignored `/etc/apt/preferences.d`**, so on the Pi image its "Update all"
  would take Raspberry Pi's `+rpt` builds (Chromium, BlueZ…) over Debian's; `apt` follows the
  priorities (release notes, Known issues). Fixed for 0.8.5 (`PACKAGES.md`, "How it resolves").

## Release test (0.8.4 Beta, October 19–25)

`lordos-live-0.8.4-amd64.iso`, built from ca52be1 (build `20261025.ca52be13`; the `v0.8.4-beta` tag,
b022dc3, only adds the website's Imager guide on top), 1 052 588 032 bytes, sha256
`73bf0124149ecd4e46efc751c1d082d2eb3e7d4c2406a19451f4dedfc144dc72`, built on VM 116 with `GPU=1
RELEASE=1` from a tree whose `code-manifest.json` was signed on the maintainer's machine
(`REQUIRE_SIGNED_CODE=1`); `lord-server --check-code`: verified, release build. `make -C server test`:
23 passed, 0 failed, 8 skipped on VM 116 (the seven node suites ran on the maintainer's machine, all
pass) and 24 passed, 0 failed, 7 skipped on the Raspberry Pi 5; 0 compiler warnings. Both `.deb`:
lintian 0 errors (17 warnings, the known ones); both SBOMs validated with `pyspdxtools`. New
dependencies: `libsrtp2-1` and `libusrsctp2` (the WebRTC remote desktop). The release also carries the
LordOS Imager's desktop downloads (`lordos_imager_0.8.4_universal.dmg`, signed and notarized;
`_amd64.deb`; `_x64_setup.exe`, not code-signed), listed in the signed `SHA256SUMS`. The test VMs got
a fresh 0.8.3 install from the published 0.8.3 ISO first (user `rtester`), so the upgrade ran on a
clean system. The tests started on October 19 with build `20261019.13bdd697` and were repeated on
October 25 with the final build (it adds the Pi HDMI wait, 4d17d2d, and strips `lordos-firstrun`).

| Check | Result |
|---|---|
| Store → Updates on VM 120 (0.8.3 installed from its ISO, UEFI, `lordos.sources` pointed at `apt-staging`): "LordOS 0.8.4 is available" with the notes; the plan is `lordos` plus `libsrtp2-1` and `libusrsctp2`; Update → migration 005 only → "Restart required" → Restart (the sheet with the open apps) → login screen → About "Version 0.8.4 · Beta", build `20261025.ca52be13`, **Verified**; `lordos`, `lord-netperm`, `lord-fand` active, 0 failed units | pass |
| `apt update && apt upgrade` (0.8.3 → 0.8.4, VM 120 back at the clean 0.8.3): 41 s, migration 005, the two libraries installed, `settings.json` byte-identical, restart, 0 failed units; `apt install --reinstall lordos` runs no migration again. `apt-get upgrade` keeps `lordos` back (new dependencies; noted in the release notes) | pass |
| Migration 005 on the upgraded system: `/etc/chromium/master_preferences` `custom_chrome_frame` false → true, `/etc/firefox-esr/lordos.js` `inTitlebar` 0 → 1. Settings → Appearance → "Browsers: tabs in the title bar" lists "1 Chromium profile still uses the LordOS frame" (a profile opened on 0.8.3); **Change them** → Chromium opens with a single bar and LordOS's traffic lights | pass |
| Smoke checks: XTerm opens (Xwayland `:0`, `app-lordos-debian-xterm.scope`); the native LordOS Imager opens (Device step); the overlay scroll bar shows while scrolling and is gone once the pointer leaves; smartmontools installed and smartd forced past its virtualization check: it stays off | pass |
| Remote desktop on the upgraded VM 120 (Settings → Sharing on, "Local network"), from the maintainer's machine in headless Chrome: H.264 video over UDP, 1280×800, the data channel up, audio packets flowing; with UDP and every TCP connection to 5673 beyond the first dropped (`iptables` on the VM, for the test only) the page shows "Couldn't connect to the remote desktop" with why and what to do, and Try again | pass |
| VM 117, OVMF: the final ISO boots to the live desktop, About "Version 0.8.4 · Beta", build `20261025.ca52be13`, **Verified**; the install itself was tested with the first build's ISO (`20261019.13bdd697`, the same installer): install on the disk (en_US, Buenos Aires, 2 GB swap) in under 2 minutes; installed: login screen, `lordos` 0.8.4, `xwayland` and `libsrtp2-1` installed, Chromium's new-profile default `custom_chrome_frame: true`, `lordos-firstrun` present, 0 failed units, boot 6.8 s, 4.6 GB used | pass |
| Raspberry Pi 5 (lordos 0.8.3, kernel `6.18.57-lordos.3-rpi-2712`): `apt-get install lordos` from staging: 0.8.4 in 13 s, migration 005, `sshd -t` clean, `lord-server --check-code` verified; `lordos`, `lord-netperm`, `lord-fand` restarted (no reboot), `display: /dev/dri/card1`; then the source switched back to the live repository: `lordos 0.8.4` installed and candidate | pass |
| Pi firewall cut on kernel `-3` with 0.8.4 (`server/tests/netperm_cut.sh` as root with a hand-made session cgroup, removed afterwards): TCP download cut 0.34 s after the rule, idle session 0.02 s, HTTP/3 0.07 s, a domain rule cuts only that destination, "no internet" keeps the LAN; 15/15 | pass |
| Raspberry Pi 5 image (build `20261019.13bdd697`): decompressed and attached as a loop device on the Pi; a first-run file written by the Imager's own writer (`apps/imager/core/firstrun.js`: Spanish, Classic, user with autologin, hidden Wi-Fi, SSH keys only, remote desktop, active cooler with the performance profile and overclock) applied by the image's own arm64 `lordos-firstrun` with `--root/--boot`: 26/26 checks (every file, owners and modes, no Wi-Fi password, the file shredded, a broken file applies nothing) | pass |
| GitHub: the release downloaded back, the key fingerprint `c7155b8e…53ea108e`, "Signature Verified Successfully" and every SHA-256 OK; the downloaded `.dmg`, marked as downloaded, accepted by the system's app check ("Notarized Developer ID") and its staple validated | pass |

Found in this test:

- `lordos-firstrun` was never built by `make -C server` (the Makefile reset `ALL` after adding it), so
  the package couldn't be made; fixed (13bdd69). It wasn't stripped either (lintian error); fixed.
- The Store refused a re-published staging package with "the file is larger than allowed": the CDN
  still served the first 0.8.4 test build at the same URL after the staging pool was replaced. The
  check did its job (nothing was installed); invalidating the path fixed it. Versions are never
  replaced in the live repository, so this only concerns staging.
- **The Pi image is not published:** a user report showed the 0.8.3 Pi image staying on the text
  console on an HDMI monitor at its first boot. 4d17d2d makes the session wait for the display
  driver; the rebuilt 0.8.4 image (`20261025.ca52be13`) hasn't been booted on a board with a monitor
  yet, so it is held back (release notes, Known issues).

## Release test (0.8.3 Beta, October 19)

`lordos-live-0.8.3-amd64.iso`, built from 36cf07b (build `20261019.36cf07b4`; the `v0.8.3-beta` tag,
9eff265, only adds two CHANGELOG notes on top), 1 051 269 120 bytes, sha256
`2416c730da65afe37ed6be615fd67448edf100452d443786a9113b90a05c7f5d`, built on VM 116 with `GPU=1
RELEASE=1` from a tree whose `code-manifest.json` was signed on the maintainer's machine
(`REQUIRE_SIGNED_CODE=1`); `lord-server --check-code`: verified, release build. `make -C server test`:
23 passed, 0 failed, 7 skipped on VM 116 (the six node suites ran on the maintainer's machine, all
pass) and 24 passed, 0 failed, 6 skipped on the Raspberry Pi 5; 0 compiler warnings. Both `.deb`:
lintian 0 errors (17 warnings, the known ones); both SBOMs validated with `pyspdxtools`. New in the
images: `xwayland` (also recommended by the package). The test VMs got a fresh 0.8.2 install from
the published 0.8.2 ISO first (user `rtester`), so the upgrade ran on a clean system.

| Check | Result |
|---|---|
| Store → Updates on VM 120 (0.8.2 installed from its ISO, UEFI, `lordos.sources` pointed at `apt-staging`): "LordOS 0.8.3 is available" with the notes; the plan is `lordos` only; Update → migration 004 only (a running smartd, started for the test, was stopped) → "Restart required" → Restart (the sheet with the open apps) → login screen → About "Version 0.8.3 · Beta", build `20261019.36cf07b4`, **Verified**; `lordos`, `lord-netperm`, `lord-fand` active, 0 failed units | pass |
| `apt update && apt upgrade` (0.8.2 → 0.8.3, VM 120 back at the clean 0.8.2): 31 s, migration 004, `xwayland` installed as a new recommendation, `settings.json` byte-identical, restart, 0 failed units, X display reserved; `apt install --reinstall lordos` runs no migration again. `apt-get upgrade` keeps `lordos` back (new recommended package; noted in the release notes) | pass |
| Smoke checks on the upgraded system: XTerm opens (Xwayland `:0`, `app-lordos-debian-xterm.scope`); the Passwords padlock and the Backups clock centered in the Dock; Settings → Appearance → Classic and back, live; overlay scroll bar visible while scrolling and gone 3 s later; smartmontools installed and smartd forced past its virtualization check: it stays off (`ConditionPathExists=/etc/lordos/smartd-enabled`) | pass |
| Firewall: Chromium playing a video, its row switched off in Monitor → Network: its established sockets went from 10 to 0 within 0.5 s of the click, received bytes stopped; Monitor counts the denials; switching it back on works | pass |
| Remote desktop on the upgraded VM 120 (Settings → Sharing on, "Local network"): login from the LAN (303 + `__Host-` cookie); 3 wrong passwords (1.25 s each) block the address, the 4th connection is cut before TLS; the block notification and the "Temporarily blocked" row in Settings; **Unblock** → login works again; Monitor → Network shows the "Remote desktop" row with the 3 denials | pass |
| VM 117, OVMF: live desktop, About 0.8.3 Verified; install on the disk (en_US, Buenos Aires, 2 GB swap) in under 5 minutes; installed: login screen, `lordos` 0.8.3 and `xwayland` installed, `DISPLAY=:0`, 0 failed units, boot 7.4 s, 4.6 GB used | pass |
| Raspberry Pi 5 (lordos 0.8.0, no repository yet): the 0.8.0 one-time step (key and `lordos.sources`), then `apt-get install lordos` from staging: 0.8.3 in 15 s, migrations 001–004, `xwayland` installed, `sshd -t` clean with root keys kept (migration 002), `lord-server --check-code` verified, `lordos` restarted to the login screen without a reboot; then the source switched to the live repository: `lordos 0.8.3` is the candidate | pass |
| Raspberry Pi 5 image, read-only: the signed manifest (same SHA-256 as the maintainer's), build `20261019.36cf07b4` in `lord-server`, migrations 001–004, `/usr/bin/Xwayland`, the smartd drop-in, the LordOS kernel `6.18.57-lordos-rpi-2712` | pass |

Found in this test:

- `xwayland` was not in the ISO, the Pi image or the package's Recommends, so a new install could
  not open X11 apps; it is now in all three (`tools/iso/build.sh`, `tools/rpi/build-img.sh`,
  `tools/iso/mkdeb.sh`). The Store's update installs only dependencies, so a system updated from
  the Store needs `xwayland` installed once (release notes).
- The Pi image keeps 0.8.2's kernel, built before `CONFIG_INET_DIAG_DESTROY`: on the Pi the
  firewall drops a denied connection's packets but doesn't close its socket (release notes).

## Release test (0.8.2 Beta, October 19)

`lordos-live-0.8.2-amd64.iso` from the `v0.8.2-beta` tag (dcb8293, build `20261019.dcb8293e`),
1 047 648 256 bytes, sha256 `00d52aa1408805a869c383484f3e1ef675e6ffd42da299d63db916e76e1f4e73`, built on
VM 116 with `GPU=1 RELEASE=1` from a tree whose `code-manifest.json` was signed on the maintainer's
machine (`REQUIRE_SIGNED_CODE=1`); `lord-server --check-code`: verified, release build; `make -C server
test`: 23 passed, 0 failed, 5 skipped (the four node suites ran on the maintainer's machine), 0 compiler
warnings. Both `.deb`: lintian 0 errors; both SBOMs validated with `pyspdxtools`. This is the first
version delivered to installed systems as an update, so the upgrade path was tested first.

| Check | Result |
|---|---|
| Store → Updates on VM 120, 0.8.1 installed from the published 0.8.1 ISO (UEFI), `lordos.sources` only (`https://lordos.org/apt`): "LordOS 0.8.2 is available", the whole "What's new" with no overlapping lines | pass |
| Update from the Store (a `file:` copy of the final live tree, then the staging repo with the first 0.8.2 build): migration 003 only, "Restart required" → Restart Now (the sheet with the open apps) → login screen with Crystal Crown → About "Version 0.8.2 · Beta", build `20261019.dcb8293e`, **Verified**; wallpaper `aurora` kept; 0 failed units; `apt install --reinstall lordos` runs no migration again | pass |
| `apt update && apt upgrade` (0.8.1 → 0.8.2): migration 003, `settings.json` byte-identical, restart, code verified | pass |
| `install.sh --deb` 0.8.1 → 0.8.2 and 0.8.0 → 0.8.2 (0.8.0 from its ISO, after `apt install curl`): "valid Ed25519 signature of SHA256SUMS", migrations 001–003, `session.conf` unchanged, settings kept, restart | pass |
| VM 117, OVMF: boot menu, live desktop, About 0.8.2 Verified; install on the disk (en_US, Buenos Aires, 2 GB swap) in about 95 s; installed: login screen with Crystal Crown, new user on **Celestial**, boot 7.5 s, 0 failed units | pass |
| VM 123, SeaBIOS, standard VGA: menu at 1024×768, live desktop (VM restored afterwards) | pass |
| Keys and system prompts: a Wayland test client (`tools/diag/keylag-client`, `server/tests/keyfocus/shellkeys.sh`) got 6 key presses typed into the search with the first 0.8.2 build and **0** with the fix; it gets them again when the search closes | pass |
| Disks offers smartmontools; Monitor → Disks says "No sensor" for the virtual disk; Settings → Displays → Video "Automatic" (virtual GPU: H.264); Mail, Calendar, Contacts and Reminders as `lord-server --app <id>` (seccomp, own scope); Monitor → Network history (1 h) and per-destination switches with Chromium; typing in Chromium; a copy of `/usr/lib/lordos` with one changed byte → `--check-code` FAILED | pass |
| Raspberry Pi 5 image, read-only on the board: 33 checks pass (kernel with BPF LSM, signed manifest, build, migrations 001–003, wallpapers, drivetemp, video extension); `apt-get update` against `https://lordos.org/apt` in temporary lists: `lordos 0.8.2 arm64`, SHA-256 of the released `.deb` | pass |

Found in this test: Store → Discover showed no suggestions on the QEMU VM (`qemu-guest-agent` needs the
`org.qemu.guest_agent.0` virtio port, which these VMs don't have), so that card was not seen here.

## Release test (0.8.1 Beta, October 19)

`lordos-live-0.8.1-amd64.iso` from the `v0.8.1-beta` tag (7cba92d, build `20261019.7cba92d2`),
1 043 156 992 bytes, sha256 `04f388d6d3d252c5c51d0ff4260a553b7e3dfaf904d7c0d04b7122fce21950e4`, built on
VM 116 with `GPU=1`; `make -C server test`: 23 passed, 0 failed, 3 skipped, 0 compiler warnings.
`lordos_0.8.1_amd64.deb` and `_arm64.deb`: lintian 0 errors (the known warnings). Both SBOMs validated
with `pyspdxtools`.

| Check | Result |
|---|---|
| VM 117, OVMF: menu, splash, desktop; About "Version 0.8.1 · Beta", build `20261019.7cba92d2`; `lordos`, `lord-netperm` (11 BPF programs), `lord-fand` active, 0 failed units | pass |
| Crown menu → Restart…: the sheet with its countdown and the open apps, Cancel | pass |
| Settings → Sharing → Remote login (SSH): installs `openssh-server`, port 22 open, host key fingerprint matches; off closes the port | pass |
| Monitor → Processes (Get Info, the Quit/Force Quit sheet, cancelled), Network (Chromium's traffic counted, the map), the Dock's quick actions | pass |
| Chromium typing: no "buggy presentation feedback" lines; audio through the HDA sink with 0 errors; a per-app firewall restriction blocks and unblocks | pass |
| Install on VM 117's disk (OVMF, en_US, Buenos Aires, 2 GB swap) in about 90 s, "Restart" in one click; installed: login, services, 0 failed units, boot 7.3 s, 474 MB used | pass |
| VM 123, SeaBIOS, VMware-compatible display, 2 GB: menu at 1024×768, the timeout boots the kernel, the "no KMS display" notice on the console (0.8.0: GRUB hung) | pass |
| VM 123, SeaBIOS, standard VGA, 2 GB: menu, splash, live desktop | pass |
| Upgrade with apt (VM 120, 0.8.0 from the ISO, `apt-staging`): 0.8.1 in 33 s, migrations 001 and 002, settings and `session.conf` unchanged, restart, `--reinstall` runs no migration again | pass |
| Upgrade with `install.sh` (VM 117, 0.8.0 installed from the 0.8.0 ISO): after adding `apt-staging`, the 0.8.0 Store's Updates lists `lordos 0.8.0 → 0.8.1`; `install.sh --deb` (the tag's file) upgrades in place, migrations 001 and 002, `session.conf` unchanged, settings kept (only window positions and recent apps changed, `schema: 1` added), after the restart About shows 0.8.1 | pass |
| Raspberry Pi 5 image, checked read-only: LordOS kernel `6.18.57-2+lordos` with BPF LSM, BTF and Yama, Bluetooth packages and helper, services enabled, SSH off, the repository and migrations; `apt-get update` against staging on the board in temporary lists lists `lordos 0.8.1 arm64` | pass |

Found in this test: a system installed from the 0.8.0 ISO has no `curl`, so the one-time
`curl -fsSL https://lordos.org/install.sh | sh` step needs `sudo apt update && sudo apt install -y curl`
first (added to the release notes, [UPDATES.md](UPDATES.md) and the FAQ). The 0.8.1 ISO doesn't need
that step: its package carries the repository.

## Release test (0.8.0 Beta, October 18)

`lordos-live-0.8.0-amd64.iso` from the `v0.8.0-beta` tag (46c972a, build `20261018.46c972ac`),
1 041 377 280 bytes, sha256 `e3a370eca1c756888fd0efcc6f02def8cf6d4c99b3618b8ed3f31465074af90c`, built on
VM 116 with `GPU=1` in 14.5 minutes; `make -C server test`: 23 passed, 0 failed, 2 skipped, 0 compiler
warnings. `lordos_0.8.0_amd64.deb` (6.3 MB): lintian 0 errors, 14 warnings (the known ones). SBOM
validated with `pyspdxtools` (spdx-tools 0.8.5). Most checks were made on the build just before it
(f4f266d), which differs only in the build id and `/etc/.updated`; boot, About, services, audio,
network, the greeter restart and the full install were repeated with the tagged ISO.

| Check | Result |
|---|---|
| Boot menu with the LordOS theme ("LordOS 0.8.0 (live)"), splash with its bar, hand-off to the desktop without a black or text frame (recorded at 5 fps on VM 117) | pass |
| About "Version 0.8.0 · Beta", build `20261018.46c972ac`; Settings → General 0.8.0 | pass |
| `lordos`, `lord-netperm` (9 BPF programs, devices and Bluetooth: yes), `lord-fand` active; 0 failed units; `ptrace_scope = 1`; pipewire-pulse module loading off; remote desktop off | pass |
| Network (DHCP from `lord-netd`, ping) and audio (tone through the HDA sink, 0 errors in `pw-top`) | pass |
| Chromium and Firefox ESR open pages with the LordOS frame; Passwords opens as `lord-server --app passwords` (own scope, seccomp, NoNewPrivs) | pass |
| Firewall: Foot launched from the app launcher, "No internet (LAN only)" set in Settings → Privacy while it runs: a connection to the internet gets EPERM (logged by `lord-netperm`), the LAN still works; Monitor's per-app switch blocks it too | pass |
| Domain rules: Settings → Privacy → Network rules by app → Add rule opens the "Block a destination" sheet | pass |
| SIGPIPE: Foot killed with `kill -9` while flooding output; `lord-server` (same pid) and the session survive; shells keep the default SIGPIPE | pass |
| `systemctl restart lordos` twice: exactly one login screen process (the old one is stopped) | pass (fixed in this release) |
| Live boot: 5.4 s kernel + 16.1 s userspace; 666 MB used after login; `ldconfig.service` no longer runs on every live boot | pass |
| Install on VM 120 (OVMF, 24 GB): en / en_US / Buenos Aires / US / 2 GB swap, 81 s; "Restart" in one click | pass |
| Installed: splash, login screen, sign in, `lordos`/`lord-netperm`/`lord-fand` active, 0 failed units, boot 4.3 + 3.0 s, 493 MB used, UEFI, network | pass |
| `install.sh --remote-desktop` on the installed VM 120: port 5673 listening, login page and a login from another machine | pass |

Seen and left for later: the installed system's GRUB prints "Booting `LordOS GNU/Linux`" and
"Loading Linux …" in the theme's text box for about a second (Debian's `10_linux` echoes them).

## Final ISO test (0.7.0 Beta, October 18)

`lordos-live-0.7.0-amd64.iso`, 963 794 944 bytes, sha256
`999afa4009de0a67ba980e982cdeac42c2c9121f4d1fcf945346a6852919474a`, built on VM 116 from `main`
8941033 (new brand and Lumo) with `GPU=1`; `make -C server test`: 23 passed, 0 failed, 1 skipped
(bidi data not installed). `lordos_0.7.0_amd64.deb` (6.0 MB): lintian 0 errors. squashfs 843 MB,
uncompressed system 2.5 GB, build 14.5 minutes. Screenshots in `docs/iso-final/`.

The live checks were made on VM 117 (OVMF) with the build just before this one (sha256
`881c24c3…88dd9cfd`), which only lacks `gsettings-desktop-schemas`; the GTK app, both browsers and
the whole installation were repeated with the final ISO.

| Check | Result |
|---|---|
| Boot menu: English default, safe mode, Español, Português, copy to RAM | pass ([grub](iso-final/grub.png), [es](iso-final/live-es-desktop.png)) |
| Live user → desktop (autologin), keyboard follows the entry's language (US / LA) | pass (after fix 1) |
| `lordos`, `lord-netperm`, `lord-fand` active; `lord-fand` enabled | pass ([terminal](iso-final/live-services.png)) |
| `lord-fand` on a VM: "0 fan(s) … profile auto", Cooling says "No controllable fans" calmly | pass |
| Firewall step 2: 7 BPF programs loaded and attached to `lordos.service` and `user@1000.service` | pass |
| Remote desktop off: no `/etc/lordos/remote/config`, port 5673 closed, toggle off in Sharing | pass |
| Chromium 155 and Firefox ESR 153 preinstalled, open pages, LordOS frame | pass |
| GTK 3 app (Mousepad, installed with apt in the live session) with the LordOS titlebar and dark theme | pass (after fix 2) |
| Passwords: separate `lord-server --app passwords` in its own scope, seccomp, NoNewPrivs; create, lock | pass |
| Mail, Monitor (Cooling and Network tabs), Settings (General, Cooling, Sharing), Files, Store (73 097 packages), Terminal, About "0.7.0 · Beta" | pass |
| Audio: 30 s tone through PipeWire (HDA), 0 errors in `pw-top`, data loop real time (FIFO 88) | pass |
| Network: DHCP from `lord-netd`, ping to the internet | pass |
| ❖L lock and unlock with an empty password | pass |
| Install on VM 120 (24 GB, OVMF): en / en_US / Buenos Aires / US / 2 GB swap, ~2 minutes | pass |
| Installed: login screen, sign in, services active, no `live-boot` or `lordos` user, `AUTOLOGIN=` empty, groups, UEFI, network, Chromium, Firefox, audio | pass |

Idle (after a minute without input):

| | Used RAM (`free`) | CPU idle (`vmstat`) | Boot |
|---|---|---|---|
| Live, VM 117 (4 GB, 4 cores) | 465 MB | 99 % | 16.3 s (4.9 kernel + 11.4 userspace) |
| Installed, VM 120 (4 GB, 4 cores) | 487 MB | 99 % | — |

**Fixes made in this test:**

1. **English live session with the Latin American keyboard.** The SDK's default layout is `latam`,
   and `lordos-live-prep` only wrote the language. It now also writes `keyboards` for the live user
   from the boot entry: English → `us`, Español → `latam`, Português → `br`.
2. **GTK 3 apps drew light Adwaita.** lord-server writes `gtk-theme` to the GSettings keyfile, but
   without `gsettings-desktop-schemas` GTK 3 can't read it. The ISO installs it and the package
   recommends it.
3. **Package copyright** said the license was undefined; it now carries the MIT text and lists
   Plus Jakarta Sans with the other OFL fonts.

Noted then, fixed for 0.7.0-beta: Notes stored its notes in `~/Notas`, a Spanish folder name that
also appeared in English sessions. It now uses `~/Notes` and moves `~/Notas` there on first run
(`docs/NOTES.md`).

## Tests (VM 117 `lordos-live`, Proxmox)

VM 117 has 4 GB of RAM, 4 cores, q35, `vga: virtio`, an HDA sound card and the ISO as a CD. It also
has a 24 GB `scsi0` disk, left over from an earlier installer test, with an old installation on it.
It boots from the CD first.

- **SeaBIOS:** boots on its own into LordOS; `systemd-analyze` gives 11.8 s (4.3 s kernel and 7.5 s
  userspace). Working:
  - the Terminal;
  - PipeWire, which sees the HDA card: `wpctl status` lists the output and the input, and LordOS as
    a client;
  - Settings, Files and the Store;
  - the network, with LordOS's own DHCP: IP, route, DNS and internet access.
- **OVMF (UEFI)**, with the efidisk without preloaded keys (Secure Boot off): boots on its own into
  LordOS (`/sys/firmware/efi` present) in 12.5 s, with the network and the rest the same as with BIOS.
- **Installation to disk** (24 GB, OVMF): the full assistant in ~3 minutes; afterwards it boots from the disk via **UEFI and SeaBIOS**, logs in on its own with the created user, with network (DHCP), sound (HDA), 2 GB swap, es_AR, America/Argentina/Buenos_Aires and the Latin American keyboard; `sudo` asks for the chosen password; `live-boot` and the `lordos` user are gone.
- **"Restart" when finished** (bug found on VM 120): after installing, the first click did nothing and the second left a black screen without restarting. The cause was the recursive `bind` of `/dev`, `/proc`, `/sys` and `/run` for the chroot: with systemd those mounts are *shared*, so unmounting the target also unmounted the live system's `/run/live/medium`. From then on the live system lost files and the action answered "only from the live system". Now the `bind`s are slaves (`MS_SLAVE`). Also:
  - `install-reboot` launches `systemctl --no-block reboot` from a separate process and doesn't wait. If nothing has ended it after 20 s, it does `sync` + `reboot(2)`;
  - `lordos.service` has `TimeoutStopSec=5`;
  - the button shows "Restarting…".
  Measured on VM 117 after the fix: a single click, shutdown completes and after ~5 s the firmware is already up.
- **Bugs found and fixed along the way:**
  - **Network down:** `live-boot` writes `/etc/network/interfaces` with the interface on DHCP. LordOS
    reads that as "managed by ifupdown" (even though ifupdown isn't there) and didn't touch it, so
    the network stayed down. Now `lordos-live-prep` leaves that file with only `lo` when there's no
    ifupdown. I tried `ip=frommedia`, but Debian's initramfs takes it as an interface name and wastes
    a few seconds looking for it.
  - **No `ping` for the user:** the executable's `cap_net_raw` capability doesn't survive on the live
    system. `ping_group_range` was added.
  - **Menu borders as "?":** `loadfont unicode` was missing from `grub.cfg`.
  - **PipeWire without real time:** `rtkit` was missing.
  - **The service didn't restart:** `systemctl restart lordos` waited for the terminals, which are
    designed to outlive the server. Now it uses `KillMode=process`.
  - **Missing symbols:** some Settings icons (◐ ▣ ◧) came out blank. Now the ISO ships
    `fonts-dejavu-core`.

## Pending

- **Secure Boot:** sign with shim and our own MOK, or with Debian's shim plus a signed grub. Today
  Secure Boot has to be turned off.
- **Switching consoles** (Ctrl+Alt+F2) while LordOS runs (still not possible, see `SESSIONS.md`).
- Optional persistence on the USB stick (live-boot's `persistence`). (The installer to disk is done:
  "Installing to disk" above.)
- **Keyboard:** `lordos-live-prep` sets the live user's layout from the boot entry's language
  (English: US, Español: Latin American, Português: Brazilian) and it's changed in Settings →
  Keyboard. A selector on first boot is missing.
- **Real GPU (AMD/Nvidia):** to be tested with the ISO on real hardware. The GPU path is built
  (`GPU.md`); the ISO ships Mesa and the firmware, and the "safe mode" entry passes `LORD_GPU=0`.
- **Network robustness:** `net/state.c` takes `/etc/network/interfaces` as a sign of ifupdown even
  when ifupdown isn't installed. It should also check that `/sbin/ifup` exists. Today the ISO works
  around it in `lordos-live-prep`.
- Audio diagnostic tools (`pactl`) aren't included; `wpctl status` is.
- ~~**Network before the first login**~~ and ~~**folders of the installed user**~~: fixed in M9.
  The network belongs to the system (`lord-netd`, started by `lord-sessiond` at boot; see `NETWORK.md`)
  and the installer creates Desktop, Documents, Downloads, Music, Pictures and Videos in the chosen
  language (in Spanish: Escritorio, Documentos, Descargas, Música, Imágenes, Videos), with
  `~/.config/user-dirs.dirs` (`server/src/users/homedirs.c`, the same list as Settings → Users and
  `lordos-live-prep`). Still to be repeated with a new ISO installed to disk.
- **First mouse movement after unlocking:** on VM 117, the first absolute QMP event after ❖L/Enter
  left the cursor at the top (y≈8). The following ones worked fine. Not investigated.

## For VM-SETUP.md

M8 step (live ISO), on the development VM:

```sh
sudo apt-get install -y --no-install-recommends debootstrap squashfs-tools xorriso grub-pc-bin \
  grub-efi-amd64-bin grub-common mtools dosfstools lintian libssl-dev libsrtp2-dev libusrsctp-dev
```

In Proxmox, VM 117 `lordos-live` is used to test the ISO. It has 4 GB, 4 cores, q35, `vga: virtio`,
`audio0: ich9-intel-hda`, OVMF with an efidisk and the ISO at
`local:iso/lordos-live-<version>-amd64.iso`. For BIOS: `qm set 117 --bios seabios`.
