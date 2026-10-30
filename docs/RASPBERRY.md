# LordOS on Raspberry Pi (ARM64)

Since M8, ARM64 is an official LordOS platform. **The only supported board is the Raspberry Pi 5**:
it is the only one we test. Other boards may boot the same image, but they are not supported.
This document gathers what's needed to build the image, write it, develop on the board and test it.

> **Status (2026-10-18):** first bring-up on a real **Raspberry Pi 5 (16 GB, SD card)** running
> Raspberry Pi OS-style Debian 13 (kernel `6.18.50+rpt-rpi-2712`, 16K pages, no monitor attached).
> Verified on the board: native build, the headless test suites, NEON, the arm64 `.deb`, lord-sessiond
> and the login screen starting on vc4 without a monitor, and `build-img.sh` (see "Results on the
> board"). **Not verified yet:** anything with a monitor on HDMI (KMS modeset, GPU composition on
> screen, hardware cursor, FPS), the desktop session, remote-desktop screenshots, and booting the
> image that `build-img.sh` produced.
>
> **Update (2026-10-18, later):** the image now ships **the LordOS build of the Raspberry Pi kernel**
> (`6.18.57-lordos-rpi-2712`, Raspberry Pi's source and `bcm2712_defconfig` plus Landlock; see "The
> LordOS kernel"), **switch files** on the boot partition (`ssh`, `remote-desktop`, Wi-Fi in
> `lordos-user.txt`; see "Switch files") and a post-flash check (`verify-board`, see "After
> flashing"). Built on the board; it is verified by booting the reflashed card.
>
> **Update (2026-10-19, 0.8.4):** `build-kernel.sh` now builds revision 3,
> **`6.18.57-lordos.3-rpi-2712`** (`6.18.57-3+lordos`): the same source and options plus sock_diag
> `SOCK_DESTROY` (`INET_DIAG_DESTROY`, `INET_UDP_DIAG`), so the firewall closes the connections a new
> rule denies instead of leaving them stalled. Each kernel revision now has its own release, so the
> previous one stays installed and bootable (see "Switching kernels on a board"). Cross-compiled on
> VM 116, installed on the Pi 5 and verified there: `verify-board`, the firewall cut test (15/15,
> TCP and QUIC closed in 0.04–0.37 s) and `make -C server test`.
>
> **Update (2026-10-25, 0.8.4.1):** the Pi 5 image shows the login screen and the desktop on an HDMI
> monitor from the first boot (the display driver line in `config.txt` now comes after
> `display_auto_detect`/`camera_auto_detect`, plus 0.8.4's longer wait for the display driver;
> `CHANGELOG.md`, 0.8.4.1, Fixed). The image also brings Chromium, Firefox ESR and the ISO's system
> packages, and the bootloader is updated from Settings → Board firmware. A monitor plugged in after
> boot is followed since 0.8.5 (the login screen listens for the kernel's display events too). Frame rates with GPU composition on a 1920×1080 monitor
> were measured with LordBench's desktop test (`BENCHMARK.md`, "The desktop under load"). **Still not measured:**
> two monitors and 4K.
>
> **Update (2026-10-27, 0.8.5):** released with the image `lordos-0.8.5-rpi5-arm64.img.xz`. A Pi 5 on
> 0.8.4 updated to the same package built as `0.8.5~rc2` with `apt` and restarted (no migration;
> `CHANGELOG.md`, 0.8.5, Upgrading). What changes on the Pi: the login screen follows a monitor plugged
> in after boot (`SESSIONS.md`), the Store follows apt's priorities so "Update all" no longer takes
> Raspberry Pi's builds of Debian packages, and packages with a `+` in their name (`kms++-utils`,
> `libkms++0`) download from lordos.org, so Settings → Board firmware can install the bootloader tools
> from the LordOS repository alone (`PACKAGES.md`). Still not measured: the same as above.
>
> **Update (2026-10-30, 0.8.6):** released with the image `lordos-0.8.6-rpi5-arm64.img.xz` (783,667,748
> bytes, sha256 `a81a2e068870099ac70aef8eaa08d0f1e33c0b3825f18f231f2e43ee251ea613`). A Pi 5 on 0.8.5 updated to 0.8.6 with `apt` against
> `lordos.org/apt-staging` and restarted: no migration ran, the settings files are unchanged, and it
> came back with 0.8.6-beta build 20261030, the first frame 7.63 s after the kernel, in a release build
> with signed code (`UPDATES.md`, "Release tests"). The Store's own package engine and the Store → Updates
> screen were not exercised, and a second reinstall was not run. The test suites were not rerun on the
> Pi for 0.8.6. Results are in `results/0.8.6-beta/pi5-16gb-hdmi/` (`RESULTS.md`). Still not
> measured: the same as above.
>
> **Not tested:** the `lordos-0.8.6-rpi5-arm64.img.xz` file itself was not written to a card or booted. Its package is
> the one tested on the Pi above, built in the same run.

## Models

| Board | Status | Notes |
|---|---|---|
| **Raspberry Pi 5** (4, 8 or 16 GB) | **supported (the only one)** | BCM2712, 4× Cortex-A76 at 2.4 GHz, V3D 7.1 (OpenGL ES 3.1, Vulkan 1.3), kernel with 16K pages |
| Raspberry Pi 500 / CM5 | not supported | same SoC, so it may work; untested |
| Raspberry Pi 4 / 400 / CM4 | not supported | BCM2711; the image still carries a `rpi-v8` kernel, but it is untested |
| Pi 3 and earlier, Zero 2 | no | too little RAM and an old vc4 GPU (GLES 2 without modern dmabuf) |

## Recommended equipment

- **Raspberry Pi 5 with 8 or 16 GB.** It works with 4 GB, but GTK/Qt apps and Firefox take quite a
  lot.
- **NVMe with the M.2 HAT+** (or any PCIe base for M.2 2230/2242). It's several times faster than an
  SD card, and it shows when booting and opening apps. The image ships `dtparam=pciex1_gen=3`; if the
  disk gives errors, comment that line out in `config.txt` and it stays at gen 2.
- The official **Active Cooler**, or a heatsink with a fan. Without a fan, the Pi 5 throttles within
  a few minutes of load (video, compiling). The kernel drives the fan on its own.
- **Official 27 W power supply (5 V, 5 A).** With a 3 A one, the board limits USB to 600 mA and warns
  about it.
- HDMI monitor (micro-HDMI on the board) and a USB keyboard and mouse.

## The image

`tools/rpi/build-img.sh` builds `lordos-<version>-rpi5-arm64.img.xz`. It is written the same way to an
SD card or an NVMe, and the same image boots on the Pi 5 and the Pi 4.

### What it ships

- **Debian 13 (trixie) arm64**, minbase, with the same LordOS packages as the ISO: PipeWire,
  wpa_supplicant, OpenVPN, Noto, foot, FeatherPad, automatic time (`systemd-timesyncd`: the board
  has no clock that keeps time without a battery, and without it the clock started months behind,
  so every signed package index was refused as "not valid yet"), printing (`cups`, `avahi-daemon`),
  exFAT and NTFS drives, power profiles, the MIME database and the GSettings schemas (since 0.8.4.1).
- **Chromium and Firefox ESR** (Debian's arm64 builds, since 0.8.4.1), with the ISO's defaults from
  the same script, `tools/iso/browser-defaults.sh`: Chromium's `master_preferences` with
  `custom_chrome_frame` and the GTK theme, Firefox's `/etc/firefox-esr/lordos.js` (`inTitlebar` 1),
  so both draw a single bar with LordOS's window buttons; the GTK themes, the video extension and
  the video settings (`video-optimize`, Settings → Displays → Video) come with the lordos package.
  Measured cost in `ISO.md`, "Release test (0.8.4.1 Beta, October 25)".
- **The LordOS build of the Raspberry Pi kernel** (see "The LordOS kernel"), when
  `tools/rpi/build-kernel.sh` has built it; otherwise Raspberry Pi's archive kernel, with a loud
  warning (it has no Landlock, BPF LSM or BTF):
  - `linux-image-<version>-lordos-rpi-2712`: the Pi 5 one, with 16K pages (archive:
    `linux-image-rpi-2712`);
  - `linux-image-<version>-lordos-rpi-v8`: the Pi 4/400/CM4 one, with 4K pages (archive:
    `linux-image-rpi-v8`). The Pi 5 can also use it, with `kernel=kernel8.img` in `config.txt`.
- **Firmware from the Raspberry Pi archive** (`archive.raspberrypi.com/debian`, suite trixie), only
  these packages (`/etc/apt/preferences.d/raspberrypi`):
  - `raspi-firmware`: the boot files, device trees and overlays in `/boot/firmware`;
  - `firmware-brcm80211`: Wi-Fi (CYW43455);
  - `rpi-eeprom` and `raspi-utils` (`vcgencmd`, `pinctrl`), if available.

  Everything else comes from Debian, Mesa included. Since 0.8.4.1 `rpi-eeprom` and what it needs
  are also in the LordOS repository (`UPDATES.md`, "Raspberry Pi firmware packages"): the same
  files, so either source updates the bootloader images; with both, `apt` takes the Raspberry Pi
  archive's copy (priority 990) and the result is the same `.deb`. Since 0.8.5 the Store's package
  engine follows `/etc/apt/preferences.d` like `apt` (`PACKAGES.md`, "How it resolves"); until then
  its "Update all" could take Raspberry Pi's builds of packages Debian also has (Chromium, BlueZ…).
- **The bootloader** (in the board's EEPROM) is updated from **Settings → Board firmware**
  (`SETTINGS.md`, "Board firmware"), not `raspi-config`: `rpi-eeprom-update`'s hint to run
  `raspi-config` doesn't apply on LordOS.
- **Bluetooth** (the CYW43455's Bluetooth half, on a UART; see "Bluetooth"), from Debian: `bluez`,
  `bluez-firmware` (the chip's patch, `brcm/BCM4345C0.hcd`), `libspa-0.2-bluetooth` (headsets in
  PipeWire) and `rfkill`, plus our SCO-routing helper (`lordos-rpi-bt@hciN`).
- **Debian 13's Mesa:** v3d (GLES 3.1), v3dv (Vulkan) and kmsro (vc4 for the display, v3d for
  rendering). lord-server is compiled with `GPU=1`.
- **The same LordOS package as the ISO.** Built by `tools/iso/mkdeb.sh` with `DEB_ARCH=arm64`.
- **Partitions:** MBR, in the layout Raspberry Pi Imager and the firmware expect:

  | Partition | Size | What it holds |
  |---|---|---|
  | 1 `bootfs` | 512 MiB, FAT32 | firmware, kernels (`kernel_2712.img`, `kernel8.img`), initramfs, device trees, `config.txt`, `cmdline.txt` |
  | 2 `rootfs` | the rest, ext4 | the system; it grows on its own at first boot |

  `fstab` and `cmdline.txt` use PARTUUIDs, so the same image works for SD (`mmcblk0`) and NVMe
  (`nvme0n1`).

### Kernel: why Raspberry Pi's and not Debian's

Researched as of October 2026 (confirm on the board):

- **Debian 13's kernel** (`linux-image-arm64`, 6.12 at release) boots on the Pi 4 with Debian's
  `raspi-firmware`. On the Pi 5 it lacks full board support: the RP1 chip (USB, Ethernet, GPIO) and
  the BCM2712's display part reached Linux after 6.12, so without keyboard, network or HDMI it's no
  use.

  Also, Debian's `raspi-firmware` regenerates `config.txt` and `cmdline.txt` with every kernel, and
  would overwrite LordOS's configuration.
- **The Raspberry Pi kernel** (`linux-image-rpi-2712`; 6.18 with its patches as of October 2026, it was 6.12 at trixie's release) is the one from
  Raspberry Pi OS Trixie. It has everything: RP1, vc4 with HDMI 4K60, v3d, the HEVC decoder, PCIe
  gen 3 and the fan.

  Its hooks copy the kernel to `/boot/firmware` under the name the firmware looks for
  (`kernel_2712.img`). If they don't, `build-img.sh` copies it by hand. **Chosen.**

With the pinning, `apt upgrade` on the installed system keeps bringing the firmware from Raspberry
Pi, and everything else from Debian. The kernel is LordOS's own build of it (next section).

### The LordOS kernel

Raspberry Pi's kernel leaves Landlock out (`# CONFIG_SECURITY_LANDLOCK is not set`, `CONFIG_LSM=""`,
so `/sys/kernel/security/lsm` is just `capability`). LordOS confines apps (`.lpk`) and its helpers
(`lord-doc`, `lord-media`, `lord-maild`) with Landlock (`server/src/sec/sandbox.c`, see
`PRIVACY.md`); without it `doc_evil.sh` fails 5 of 25 and `sandbox-test` is skipped. It also has no
BPF LSM and no BTF (`/sys/kernel/btf/vmlinux`), which `lord-netperm` needs for "No Bluetooth" of
Linux apps (its per-cgroup LSM program on `socket_create`, `FIREWALL.md`); without them
`npverify-test` fails 35/36. The tests stay as they are: the fix is the kernel.

`tools/rpi/build-kernel.sh` builds it from **Raspberry Pi's own source and defconfig**, changing only
what LordOS needs:

```sh
# on the Pi (arm64 Debian 13): about 75 min per flavour on a Pi 5 (4 cores, SD card), ~10 GB free
sudo apt-get install -y --no-install-recommends build-essential bc bison flex libssl-dev libelf-dev \
  libdw-dev dwarves cpio kmod rsync dpkg-dev debhelper git
tools/rpi/build-kernel.sh /var/tmp/lordos-kernel all     # 2712 (Pi 5), v8 (Pi 4), or all
# → /var/tmp/lordos-kernel/out/linux-image-6.18.57-lordos.3-rpi-2712_6.18.57-3+lordos_arm64.deb, …

# or cross-compiled on an x86-64 Debian 13 (VM 116: 4 vCPU, 4 GB RAM; 67 min for 2712), the same
# packages plus the cross compiler and the arm64 libc headers (for the headers package's tools)
sudo apt-get install -y --no-install-recommends gcc-aarch64-linux-gnu libc6-dev-arm64-cross
RPI_KERNEL_REF=234188315037848e5b07d65b85d38edc64095caa tools/rpi/build-kernel.sh /var/tmp/pik-kernel 2712
```

The script cross-compiles by itself when the host isn't arm64 (`CROSS_COMPILE=aarch64-linux-gnu-`,
and `DPKG_FLAGS=-d`: `dpkg-buildpackage` would otherwise ask for arm64 build dependencies the kernel
doesn't use). The 0.8.4 packages were built that way: same commit, same `.config` (the diff
against the defconfig is identical to a native build's apart from the release name), same package
size as the native `-2` (43 MB). With 4 GB of RAM a temporary 4 GB swap file was added for the
final link; the work folder peaked at about 16 GB. Don't build on the Pi in a tmpfs or with less
than 10 GB free on its SD card.

- **Source:** `github.com/raspberrypi/linux`, branch `rpi-6.18.y` (`RPI_KERNEL_BRANCH`;
  `RPI_KERNEL_REF=<commit>` pins one). The first build (2026-10-18) used commit `234188315037`,
  kernel **6.18.57**; the archive shipped 6.18.50 at the time. The 0.8.1 rebuild (`-2+lordos`, BPF
  LSM and BTF) and the 0.8.4 one (`-3+lordos`, sock_diag `SOCK_DESTROY`) use the same commit.
  `out/SOURCE` records it.
- **Config:** `bcm2712_defconfig` (Pi 5, 16K pages) or `bcm2711_defconfig` (Pi 4, 4K pages), then
  `scripts/config` and `olddefconfig`. The whole diff against the defconfig (`out/config-*.diff`):

  ```diff
  -CONFIG_LOCALVERSION="-v8-16k"
  +CONFIG_LOCALVERSION="-lordos.3-rpi-2712"
  -# CONFIG_SECURITY_LANDLOCK is not set
  +CONFIG_SECURITY_LANDLOCK=y
  -CONFIG_LSM=""
  +CONFIG_LSM="landlock,lockdown,yama,integrity,bpf"
  -# CONFIG_SECURITY_YAMA is not set
  +CONFIG_SECURITY_YAMA=y
  -# CONFIG_BPF_LSM is not set
  +CONFIG_BPF_LSM=y
  -CONFIG_DEBUG_INFO_NONE=y
  +CONFIG_DEBUG_INFO=y
  +CONFIG_DEBUG_INFO_DWARF_TOOLCHAIN_DEFAULT=y
  +CONFIG_DEBUG_INFO_BTF=y
  +CONFIG_DEBUG_INFO_BTF_MODULES=y
  -# CONFIG_INET_UDP_DIAG is not set
  +CONFIG_INET_UDP_DIAG=m
  -# CONFIG_INET_DIAG_DESTROY is not set
  +CONFIG_INET_DIAG_DESTROY=y
  ```

  plus what `olddefconfig` derives from those (`PAHOLE_HAS_SPLIT_BTF`, `PROBE_EVENTS_BTF_ARGS`,
  `FUNCTION_TRACE_ARGS`, and a few new options answered "no").

  `CONFIG_LSM` uses the usual Debian order; the kernel skips the ones it doesn't build (lockdown
  and IMA/EVM stay off, as Raspberry Pi has them), so the active list is
  **`capability,landlock,yama,bpf`**. The image's `cmdline.txt` has the same `lsm=` list.
- **Yama (0.8.1)**, so the lordos package's `kernel.yama.ptrace_scope = 1` (a process may only
  trace its own descendants) applies on the Pi as on a PC; with 0.8.0's kernel the sysctl had
  nothing to set.
- **BPF LSM and BTF (0.8.1).** `CONFIG_BPF_LSM` lets `lord-netperm` attach its per-cgroup LSM
  program (`BPF_LSM_CGROUP`) to `socket_create`, so "No Bluetooth" is enforced for Linux apps on the
  Pi as on a PC; its attach point is the BTF id of `bpf_lsm_socket_create`, read from
  `/sys/kernel/btf/vmlinux` (`CONFIG_DEBUG_INFO_BTF`, generated by `pahole` from the DWARF debug
  info: Debian's `dwarves`). The debug info is only there to make BTF: the modules are stripped by
  the packaging (their `.BTF` sections stay) and the `-dbg` package isn't built
  (`DEB_BUILD_PROFILES=pkg.linux-lordos-rpi-<flavour>.nokerneldbg`), so the package hardly grows.
  The script checks `vmlinux` has a `.BTF` section. The build needs about 10 GB free while it runs
  (the object files carry DWARF) and takes about 50 % longer.
- **sock_diag `SOCK_DESTROY` (0.8.4, `-3+lordos`).** When a rule changes to deny, `lord-netperm`
  drops the packets of the app's open connections and destroys its TCP and connected UDP (QUIC)
  sockets over sock_diag netlink, so the app sees the connection end (`FIREWALL.md`, "Existing
  connections are cut"). Destroying needs `CONFIG_INET_DIAG_DESTROY` (TCP) and `CONFIG_INET_UDP_DIAG`
  (UDP); `INET_DIAG` and `INET_TCP_DIAG` are already modules in the defconfig (enforced too), and the
  kernel loads them when `lord-netperm` opens its sock_diag socket. Raspberry Pi's defconfig leaves
  the other two off. The options were in `build-kernel.sh` since 0.8.3, but the 0.8.3 image kept
  0.8.2's `-2+lordos` kernel, so on the Pi the connections stalled until the app's own timeout (the
  journal says `the kernel can't destroy TCP sockets`). The script checks `INET_DIAG_DESTROY` stuck,
  `build-img.sh` and `verify-board` check the kernel config.
- **Already in the defconfig, checked by the script** so a future defconfig can't drop them:
  `SECCOMP_FILTER` (sandbox.c), cgroup v2 with `MEMCG`, `CGROUP_PIDS` and `CGROUP_SCHED` (per-app
  limits; `cgroup_enable=memory` is in `cmdline.txt` because Raspberry Pi turns memcg off at boot),
  and `BPF_SYSCALL`, `BPF_JIT`, `CGROUP_BPF` (lord-netperm's cgroup hooks). `USER_NS` stays as
  Raspberry Pi has it, on (the launcher's own view for restricted Linux apps uses it).
- **Packages:** `make bindeb-pkg`, version `6.18.57-3+lordos` (`LORDOS_KREV`; `-1` was 0.8.0's,
  without the BPF LSM, `-2` 0.8.1's, without `SOCK_DESTROY`), release `6.18.57-lordos.3-rpi-2712`
  (`-lordos.3-rpi-v8` for the Pi 4).
  - **One release per revision (since `-3`).** Revisions 1 and 2 were both
    `6.18.57-lordos-rpi-2712`, so a new revision replaced the old one's `vmlinuz` and
    `/lib/modules` in place: the old kernel couldn't stay bootable, since with `MODVERSIONS` and
    `DEBUG_INFO_BTF_MODULES` (without `MODULE_ALLOW_BTF_MISMATCH`) a kernel refuses another build's
    modules, Wi-Fi among them. Now `-lordos.<rev>-rpi-<flavour>` is its own package
    (`linux-image-6.18.57-lordos.3-rpi-2712`) next to the previous one, as Debian does with its ABI
    numbers. The `.<rev>` form is chosen for the order: `sort -V` (the hook's "newest") puts
    `-lordos.3-` after `-lordos-`, while `-lordos3-` or `-3-lordos-` would sort before it and the
    hook would keep copying the old kernel.
  - The `-rpi-<flavour>` suffix is what `raspi-firmware`'s hook keys
  on, so it installs exactly like Raspberry Pi's: `kernel_2712.img`, initramfs, device trees and
  overlays in `/boot/firmware`. The script checks the package carries the device trees and
  overlays. Headers are built too (`linux-headers-*`), not put in the image.
  - `make` gets `LOCALVERSION=` (empty): otherwise, in a git tree that isn't at a tag, the release
    gets a trailing `+` (`…-rpi-2712+`) and the hook skips it as an unknown flavour.
  - `make dtbs_install` doesn't install `overlays/README`, which the hook copies (and fails
    without): the script adds it to the package.
  - Measured on 2026-10-18 on the Pi 5 (load 4, 40–55 °C with the Active Cooler): 2712 in 50 min,
    v8 in 49 min. `linux-image-6.18.57-lordos-rpi-2712_6.18.57-1+lordos_arm64.deb` 32 MB (vmlinuz
    10 MB, 385 overlays) and `linux-image-6.18.57-lordos-rpi-v8_…` 33 MB; `out/SHA256SUMS` lists
    them. The v8 diff is the same three options (`-v8` → `-lordos-rpi-v8`).
- **Not replaced by the archive.** `build-img.sh` installs the LordOS `.debs` from `KERNEL_DEBS`
  (default `/var/tmp/lordos-kernel/out`) instead of `linux-image-rpi-<flavour>`, and writes
  `/etc/apt/preferences.d/lordos-kernel` with priority -1 for Raspberry Pi's kernel packages of
  that flavour (`linux-image-rpi-2712`, `linux-image-*+rpt-rpi-2712` and the headers; checked with
  `apt-cache policy`: no candidate). Otherwise a newer `+rpt` kernel would win the hook's "newest
  `vmlinuz-*-rpi-2712`" and boot without Landlock. Kernel updates come as new LordOS packages.
- **Per flavour:** if only the 2712 kernel was built, the Pi 4 kernel comes from the archive (with
  a warning, and without the pin).

### Switching kernels on a board

The firmware reads `config.txt` on `bootfs`; `os_prefix=<folder>/` makes it take the kernel,
initramfs, `cmdline.txt`, device trees and overlays from that folder instead of the top level. On
an installed system the top level is always what the packages put there: the newest LordOS kernel
(`raspi-firmware`'s hook copies it on every install). The previous kernel stays installed (its own
`/lib/modules`) and its boot files go in a folder, `lordos-prev/`:

```sh
# keep the running kernel's boot files before installing a new one (once per revision)
K=$(uname -r); D=/boot/firmware/lordos-prev
sudo mkdir -p $D/overlays && sudo cp /boot/vmlinuz-$K $D/kernel_2712.img && sudo cp /boot/initrd.img-$K $D/initramfs_2712
sudo cp /boot/firmware/cmdline.txt $D/ && sudo cp /usr/lib/linux-image-$K/broadcom/bcm27*.dtb $D/
sudo cp /usr/lib/linux-image-$K/overlays/* $D/overlays/
sudo dpkg -i linux-image-6.18.57-lordos.3-rpi-2712_6.18.57-3+lordos_arm64.deb   # the hook: top level = new
# first boot of the new kernel only once: tryboot.txt = config.txt without os_prefix
sudo reboot "0 tryboot"         # if it doesn't come up, any reset boots config.txt again
```

- **Use the new kernel:** `config.txt` without `os_prefix` (the image's default).
- **Back to the previous kernel:** add, at the end of `config.txt` (it's on the FAT partition, so
  any computer with a card reader can edit it if the board doesn't boot):

  ```
  [all]
  os_prefix=lordos-prev/
  ```

  Removing those two lines boots the new kernel again. To try the previous one **once** without
  editing `config.txt`, put that line in `tryboot.txt` (a copy of `config.txt`) and
  `sudo reboot "0 tryboot"`; the next reset is back on the top level.
- `uname -r` tells them apart: `6.18.57-lordos.3-rpi-2712` (0.8.4) or `6.18.57-lordos-rpi-2712`
  (0.8.1–0.8.3's `-2+lordos`).
- The Pi 5 used for development (2026-10-19) has exactly this: the top level is `-3`,
  `lordos-prev/` is `-2`, and `tryboot.txt` boots `lordos-prev/` once. Both were booted and checked.

### Packages to build it

On a Debian 13 (x86-64 or arm64):

```sh
sudo apt-get install -y --no-install-recommends debootstrap fdisk dosfstools e2fsprogs xz-utils curl gpg \
  util-linux dpkg-dev
# only on an x86-64 host: aarch64 emulation for debootstrap, apt and compiling
sudo apt-get install -y --no-install-recommends qemu-user-static binfmt-support
```

### Building it

```sh
# on the development machine: the themes (they are JS, the same on every architecture)
npm run build
# on the Debian where it's built (VM 116 or the Pi itself), from the repository root
sudo tools/rpi/build-img.sh /var/tmp/lordos-rpi     # → /var/tmp/lordos-rpi/out/
```

Steps:

1. **lord-server and lord-sessiond for aarch64.**
   - If `server/build/` is already aarch64 (`make -C server` on the Pi), it is used.
   - If not, a separate arm64 chroot is built (`buildroot/`, reused) and it's compiled inside with
     `GPU=1`.
   - Then `build/canvas-test` runs, which compares NEON with C. If they differ, the build stops.
2. **The package:** `DEB_ARCH=arm64 tools/iso/mkdeb.sh`.
3. **debootstrap arm64:** native on an arm64 host. On x86-64 it's two stages, with qemu.
4. **The Raspberry Pi archive.** Its key's fingerprint is verified (`RPI_KEY_FPR`). Then the packages
   and the `.deb` are installed.
5. **Configuration:** the same as the ISO (locales, `ping` without root, lordos.service on tty1,
   PipeWire with linger, the `lordos-greeter` user), plus:
   - the first boot (`lordos-rpi-firstboot.service`);
   - waiting for vc4 (`lordos.service.d/rpi.conf`).

   root is locked.
6. **The image:** `sfdisk` (MBR), FAT32 and ext4. Then the system is copied and `fstab` and
   `cmdline.txt` are written with the PARTUUIDs.
7. **`xz -T0 -6`** and the `.sha256`.

Variables:

| Variable | What for |
|---|---|
| `RPI_SSH=1` | SSH server on from the start (for development); without it `openssh-server` ships off and the `ssh` switch file turns it on |
| `KERNEL_DEBS` | folder with the LordOS kernel packages (`build-kernel.sh`), `/var/tmp/lordos-kernel/out` by default |
| `IMG_EXTRA_MB` | free space in the image, 512 by default |
| `MIRROR`, `RPI_MIRROR` | Debian and Raspberry Pi mirrors |
| `RPI_KEY_FPR`, `RPI_KEY_NOCHECK=1` | the Raspberry Pi key |

**Before the first build:** check the fingerprint of the Raspberry Pi archive key
(`gpg --show-keys raspberrypi.gpg.key`) against an official source. The one in the script
(`CF8A1AF5…7FA3303E`) was checked on 2026-10-18: it is the same fingerprint as the key in the signed
`raspberrypi-archive-keyring` package (2025.1+rpt1) installed on a Raspberry Pi OS board.

The loose `raspberrypi.gpg.key` at the archive root still carries its 2012 SHA-1 binding signatures,
and apt's `sqv` rejects them since 2026-02-01 ("SHA1 is not considered secure": `InRelease is not
signed`). So `build-img.sh` takes the key from the archive's `raspberrypi-archive-keyring` package
(re-signed with SHA-512), found through the `Packages` index; trust still comes only from the
fingerprint check. `RPI_KEY_URL=<url>` uses another key file instead.

Estimate: on x86-64 with qemu, 30–60 minutes the first time. Natively on the Pi 5 it was run on
2026-10-18 from the SD card (see "Results on the board").

### Writing it

- **LordOS Imager** (recommended; `IMAGER.md`): in LordOS (App Launcher → System) or as a small app
  for other computers. It downloads the latest release, checks its signature, writes
  and verifies the card, and prepares the first boot (computer name, language, user, Wi-Fi, SSH and
  remote desktop, appearance, cooling) in `lordos-firstrun.txt` (next section).
- **Raspberry Pi Imager:** "Choose OS" → "Use custom" → the `.img.xz`, and then the card or the
  NVMe.
  - Skip Imager's OS customization (it's designed for Raspberry Pi OS; newer versions write
    cloud-init files that LordOS doesn't read). If it leaves a `userconf.txt` on the boot
    partition, LordOS uses it (see below).
  - Instead, add the switch files and `lordos-user.txt` to the `bootfs` partition (see "Switch
    files"); `out/FLASHING.txt` next to the image has the exact steps for macOS.
- **dd** (Linux and other Unix-like systems):
  ```sh
  shasum -a 256 -c lordos-<version>-rpi5-arm64.img.xz.sha256
  xz -dc lordos-<version>-rpi5-arm64.img.xz | sudo dd of=/dev/rdiskN bs=4m   # Linux: of=/dev/sdX bs=4M conv=fsync
  ```
- **balenaEtcher:** opens the `.img.xz` directly.
- **NVMe:**
  - it can be written with a USB-NVMe adapter from another machine;
  - or boot the Pi from an SD card with LordOS (or Raspberry Pi OS) and write it with `dd` to
    `/dev/nvme0n1`.

  To make the Pi 5 boot from NVMe first:
  ```sh
  sudo rpi-eeprom-config --edit
  # BOOT_ORDER=0xf416   (NVMe, then SD, then USB)
  ```

### First boot

`lordos-rpi-firstboot.service` runs before lord-sessiond (`/usr/lib/lordos/rpi/firstboot`) and does
two things.

0. **The LordOS Imager's settings (0.8.4).** If the `bootfs` partition has a `lordos-firstrun.txt`,
   `/usr/lib/lordos/bin/lordos-firstrun` checks it with a strict parser and, only if all of it is
   valid, applies it once: computer name, language, time zone, keyboard, Wi-Fi country and network
   (the derived WPA2 key, never the password), the user (from a yescrypt or SHA-512 crypt hash),
   automatic login, SSH (keys only or with passwords), remote desktop (local network only),
   Bluetooth, appearance and cooling (fan profile, CPU governor, a `config.txt` block for a mild
   overclock or a lower clock). The file is then overwritten and deleted. An invalid file applies
   nothing and the steps below run as before. Format and details: `IMAGER.md`, "The first-run
   file". Tested by applying it (`--root`) to the real 0.8.3 image on a virtual USB stick; not yet
   booted on a board.

1. **Grows partition 2** to the end of the card or disk (`sfdisk -N 2`, `partx -u` and an online
   `resize2fs`).
2. **The user**, like the installer's "User" step (`docs/ISO.md`).
   - **With `lordos-user.txt`** on the `bootfs` partition: it's copied from
     `lordos-user.txt.example`, which ships in the image. It has user, name, `password_hash` (or
     `password`), computer name, language, time zone and keyboard.
     - It's validated with `install.c`'s rules: the name, reserved names, the computer name, the
       zone and the keyboard.
     - The user is created: groups `sudo`, `audio`, `video`, `input` and `render`, linger and the
       language's folders.
     - The file is overwritten with zeros and deleted.
     - It boots to the **login screen**, like an installed system (empty `AUTOLOGIN=`).
     - The `userconf.txt` from Raspberry Pi OS / Imager (`user:hash`) also works.
   - **Without the file: setup mode.**
     - It logs in on its own with the `lordos` user, without a password and with passwordless sudo,
       like the live ISO.
     - In **Settings → Users** you create your own account, as an administrator. On the next boot
       `firstboot` sees there's an administrator, deletes `lordos` and its sudo and switches to the
       login screen.
     - In the meantime, **don't turn on remote desktop** and don't leave the board exposed on the
       network.

3. **Switch files** (next section), on every boot.

Pending: a "create user" step of our own on the login screen, so setup mode isn't needed (see
ROADMAP, M8).

### Switch files

Like Raspberry Pi OS's `ssh` file: files dropped on the `bootfs` partition (FAT, editable from any
computer) before booting. `firstboot` applies each one, logs it (`journalctl -u
lordos-rpi-firstboot`) and deletes it (overwritten with zeros first, since they can hold a password).
A name with `.txt` (as some editors save it) works the same.

| File | Contents | What it does |
|---|---|---|
| `ssh` | empty | Turns the SSH server on (`systemctl enable ssh`, started right away) and keeps it on. The image ships `openssh-server` off; each board creates its own host keys. Logging in needs an account with a password: root is locked and the setup user has none |
| `remote-desktop` | empty, or `port=5673` and/or `allow=all\|group` | Turns LordOS remote desktop on (`https://<board IP>:5673`, `REMOTE.md`), writing the same `/etc/lordos/remote/config` (`enabled=1`, `port=`, `allow=`) that Settings → Sharing's `remote-enable`/`remote-config` write; lord-sessiond reads it when `lordos.service` starts. It still asks for a user and password: while there's no account with a password (setup mode) the file is **kept** and applied on a later boot. `port` must be 1024–65535 (5673 by default); `allow=group` limits it to the `lordos-remote` group |
| `lordos-user.txt` | from `lordos-user.txt.example` | The first-boot user (above), read only on the first boot. Optional Wi-Fi: `wifi_ssid=`, `wifi_password=` (8–128 characters; empty for an open network) and `wifi_country=` (two letters) |
| `userconf.txt` | `user:hash` | Raspberry Pi OS / Imager's user file, first boot only |

**Wi-Fi at first boot.** The image's network stack is **lord-netd** (no NetworkManager, no
systemd-networkd): at boot it brings up every free physical interface with its own DHCP client, and
on Wi-Fi interfaces it starts `wpa_supplicant` and loads the saved networks. `firstboot` saves the
network from `lordos-user.txt` the same way Settings → Network does
(`/etc/lordos/net/wifi/<SSID in hex>.json`, and `.key` with mode 0600), before lord-netd starts, so
the board joins it on the first boot. `wifi_country` adds `cfg80211.ieee80211_regdom=<XX>` to
`cmdline.txt` (the 5 GHz channels of that country, from the second boot; until then the world
domain). lord-netd skips interfaces NetworkManager manages, which only matters on Raspberry Pi OS.

Options in `cmdline.txt` (a single line), as on the ISO:

| Option | What it does |
|---|---|
| `lordos.gpu=0` | safe mode: everything on the CPU |
| `lordos.lang=es` / `pt` | first-boot language (English by default, like the ISO) |
| `video=HDMI-A-1:1920x1080@60` | pins the mode, for example on a 4K monitor (on the CPU, 4K is heavy: see below) |
| remove `splash` (or add `nosplash`) | boots in text, without the boot splash |

### Boot splash

From power-on to the login screen the board shows the LordOS crown with a progress bar (the same
Plymouth theme as the ISO, `ISO.md`, "Boot splash and boot menu"), with no rainbow square, no kernel
logo and no console text.

![Boot splash (static preview, 1x)](boot/splash-preview.png)

- **`config.txt`:** `disable_splash=1`: the firmware's rainbow square is not drawn.
- **`cmdline.txt`:** `console=serial0,115200 console=tty1 … quiet splash
  plymouth.ignore-serial-consoles logo.nologo vt.global_cursor_default=0 loglevel=3`, keeping
  `root=`, `lsm=`, `cgroup_enable=memory` and `lordos.lang=`. The local console moved from `tty3`
  to **`tty1`**: Plymouth takes its terminal (keyboard, password prompt, Esc) from the local
  `console=` and LordOS runs on tty1, so both stay on the same VT (Raspberry Pi OS does the same). The kernel's messages
  stay hidden (`quiet`, Plymouth); **Esc** shows them during the boot, as usual.
- **initramfs:** `plymouth` and `plymouth-label` are installed and `lordos` is the selected theme
  (`plymouth-set-default-theme lordos`), so Plymouth's initramfs hook copies the theme, its plugins
  and a font (for the encrypted-disk password prompt). `/etc/initramfs-tools/modules` lists `vc4`
  and `conf.d/lordos-splash.conf` keeps `MODULES=most` (the hook then adds the DRM drivers), so the
  HDMI output is up in the initramfs and the splash shows within the first seconds instead of after
  the root file system is mounted. The LordOS kernels have `DRM`, `DRM_VC4` and `DRM_V3D` as modules,
  and their packages build an initramfs through `/etc/kernel/postinst.d` (initramfs-tools); the
  raspi-firmware hook and `build-img.sh` copy it as `initramfs_2712` / `initramfs8`
  (`auto_initramfs=1`). `build-img.sh` fails if an initramfs has no `lordos.script`.
- **Hand-off:** the same as the ISO (`SESSIONS.md`, "Boot splash"); on the Pi
  `lordos-splash deactivate` runs after `wait-kms`, so Plymouth keeps drawing until vc4 is ready.
  Plymouth draws on vc4 (`card1`), the card with the HDMI connectors; `v3d` (`card0`) has none.
- **First boot:** `lordos-rpi-firstboot` (growing the partition, the user, the switch files) runs
  under the splash; its messages go to the journal and to the console, which Plymouth keeps hidden
  (Esc shows them).

Checked on the image (read-only, loop-mounted on the Pi): `config.txt`, `cmdline.txt`, the default
theme and `lsinitramfs` of both initramfs files. Still to check on a board: the timing of the first
frame of the splash after power-on, the hand-off with no flash on vc4, and 4K.

### Bluetooth

The Pi 5's Bluetooth is the second half of its CYW43455 (Wi-Fi is the first, on SDIO), wired to a
UART of the BCM2712. On the Pi 5 the kernel brings it up by itself, with no `hciattach` and none of
Raspberry Pi OS's `pi-bluetooth` scripts:

- **The device tree** (`bcm2712-rpi-5-b.dtb`) has a `brcm,bcm43438-bt` node under the UART, so the
  serial-device bus binds `hci_uart` with its BCM protocol (`CONFIG_BT_HCIUART_SERDEV`,
  `CONFIG_BT_HCIUART_BCM`, both in Raspberry Pi's defconfig and so in the LordOS kernel). It also
  carries `shutdown-gpios` (power), `max-speed` and `local-bd-address`, which the board firmware
  fills in with the board's own address before the kernel starts.
- **`btbcm`** loads the chip's patch: it looks for
  `brcm/BCM4345C0.raspberrypi,5-model-b.hcd` and then `brcm/BCM4345C0.hcd`. The image takes it from
  **Debian's `bluez-firmware`** (`non-free-firmware`, the same 63,806-byte file as Raspberry Pi's
  `+rpt` package, which only adds per-model symlinks), so the generic name is the one used. The
  chip reports the placeholder address `43:45:c0:00:1f:ac`; the kernel replaces it with the device
  tree's.
- **BlueZ** (Debian's `bluez`, `bluetooth.service` enabled) and the rest of the desktop side are the
  same as on a PC (`SETTINGS.md`, "Bluetooth"): our own D-Bus client in `lord-server`, the
  system-drawn pairing prompts, and headsets in PipeWire through `libspa-0.2-bluetooth` with the
  lordos package's WirePlumber drop-in (`lordos-bluez.conf`).
- **SCO routing, our helper.** Call audio (HFP/HSP: a headset's microphone) can leave the chip over
  HCI or over its PCM pins, which the Pi doesn't wire. Raspberry Pi OS's `bthelper` sets HCI with a
  vendor command of the chip; LordOS does the same with its own `/usr/lib/lordos/rpi/bt-uart`, run by
  `lordos-rpi-bt@hciN.service` (udev starts it for an `hciN` whose parent is on the serial bus,
  `/etc/udev/rules.d/90-lordos-rpi-bt.rules`; before `bluetooth.service`). It reads the setting
  back first (`Read_SCO_PCM_Int_Param`, 0xfc1d) and only writes (`Write_SCO_PCM_Int_Param`,
  0xfc1c, `01 02 00 01 01`: routing over HCI, with the same other parameters as `bthelper`) when it
  differs,
  leaves the controller powered as it found it, skips a controller blocked by rfkill or not on a
  UART, warns if the address is still the chip's placeholder, and never fails the boot. It uses
  `hciconfig`/`hcitool` from Debian's `bluez`; `lord-server` still never runs either (it speaks
  D-Bus to `bluetoothd` only). The setting survives a power cycle of the controller from Settings
  (an HCI reset): checked on the board.
- **Not used from Raspberry Pi OS:** `pi-bluetooth` (`hciuart.service`/`btuart` are for the older
  boards' `hciattach` and exit at once when the kernel already drives the chip; `bthelper` is
  replaced by the helper above, without its "power off and on again after 5 s" workaround, which
  the kernel driver doesn't need: Secure Simple Pairing is on from the start).

To turn Bluetooth off for good, `dtoverlay=disable-bt` in `config.txt` (the UART is then free); the
switch in Settings → Bluetooth only powers the controller off.

**Checked on a Pi 5 (2026-10-18, LordOS kernel `6.18.57-2+lordos`):** `hci0` on the UART with the
board's own address (`88:A2:…`, not the chip's placeholder), `bluetoothctl show` powered, and a
12-second scan lists the devices around it (4 to 5 in that room). "No Bluetooth" for a Linux app:
in the app's scope `socket(AF_BLUETOOTH)` gets `EPERM`, outside it works (`FIREWALL.md`, "Bluetooth
sockets"). **Not checked yet:** pairing and a headset's audio on the board (they need a device in
pairing mode next to it), and Settings → Bluetooth on the board's own screen (no LordOS session was
open; it is the same page and the same D-Bus client as on a PC, `SETTINGS.md`).
The image built with this kernel (`lordos-0.8.1-rpi5-arm64.img.xz`, 2,717 MB uncompressed) was
checked read-only, loop-mounted: our `kernel_2712.img` on `bootfs` with `BPF_LSM`, BTF, Yama and
Landlock in its config, `lsm=` with `bpf` in `cmdline.txt`, `bluez`, `bluez-firmware` (with
`brcm/BCM4345C0.hcd`), `libspa-0.2-bluetooth`, `rfkill`, `hciconfig`/`hcitool`, the helper, its unit
and udev rule, `lordos`, `lord-netperm`, `lord-fand` and `bluetooth` enabled, SSH off. The Pi 4
kernel in it (`-rpi-v8`) is still 0.8.0's `6.18.57-1+lordos`, without the BPF LSM: "No Bluetooth" is
only enforced on a Pi 4 once that flavour is rebuilt (`build-kernel.sh … v8`).

### After flashing

`tools/rpi/verify-board.sh` (in the image as `/usr/lib/lordos/rpi/verify-board`) checks a freshly
booted board and prints a PASS/FAIL table. It only reads `/sys`, `/proc`, the kernel log and the
configs; it changes nothing.

```sh
sudo /usr/lib/lordos/rpi/verify-board                  # system checks
sudo /usr/lib/lordos/rpi/verify-board --repo ~/LordOS  # plus make -C server test-full and doc_evil.sh
```

| Check | PASS when |
|---|---|
| kernel | `uname -r` is a `-lordos-rpi-*` or `-lordos.<rev>-rpi-*` kernel; page size 16K on the Pi 5 (4K with `-rpi-v8`) |
| socket destroy | the kernel config has `INET_DIAG_DESTROY`, `INET_TCP_DIAG` and `INET_UDP_DIAG` (the firewall closes the connections a new rule denies) |
| Landlock | `/sys/kernel/security/lsm` lists `landlock`; the kernel log says `landlock: Up and running`; the kernel config has `SECURITY_LANDLOCK`, `SECCOMP_FILTER`, `CGROUP_BPF`, `BPF_SYSCALL`, `MEMCG` |
| BPF LSM, BTF | `bpf` in `/sys/kernel/security/lsm`; `/sys/kernel/btf/vmlinux` present; the kernel config has `BPF_LSM` and `DEBUG_INFO_BTF`; `lord-netperm` active |
| seccomp, cgroups | seccomp available; the cgroup v2 `memory` controller is on (`cgroup_enable=memory`) |
| first boot | the root partition has grown; no switch file left on `bootfs` (WARN for a `remote-desktop` still waiting for an account); not in setup mode |
| display, GPU | a `connected` KMS output with its mode (WARN with no monitor); `eglinfo -p gbm` shows V3D; the session log shows `GPU composition` |
| fan, temperature | a fan hwmon (`pwmfan`, rpm and PWM; `lord-fand` active if it's installed); SoC < 75 °C; `throttled=0x0` |
| Bluetooth | an `hciN` on the UART (with the patch file from the kernel log); `bluetoothd` active; `lordos-rpi-bt@hciN` active (WARN otherwise); the board's own address, not the chip's placeholder `43:45:c0:…` |
| remote desktop | port 5673 (or the configured one) listens **only** when `/etc/lordos/remote/config` has `enabled=1` |
| services | `lordos.service` active; SSH shown as on or off |
| `--repo` | `make -C server test-full` passes; `doc_evil.sh` N/N (25/25 with the archive tools installed); the three `sandbox-test` runs ok |

`--repo` needs the tree and the build packages ("Developing on the board"); it builds as the tree's
owner.

## Display and GPU on the Pi 5

### Two DRM devices

| Node | Driver | What it is |
|---|---|---|
| `/dev/dri/cardN` (usually `card1`) | `vc4` | the display: HVS, HDMI ×2, planes, cursor. It has no render node |
| `/dev/dri/cardM` (usually `card0`) | `v3d` | the 3D GPU, without connectors |
| `/dev/dri/renderD128` | `v3d` | where Mesa renders |

The order of `card0` and `card1` depends on which driver loads first.

- **`server/src/drmcard.h`**, new, is used by lord-server (`main.c`) and lord-sessiond
  (`remote/monitor.c`). It picks the first card that has connectors and CRTCs, and prefers the one
  with a monitor plugged in (`GETRESOURCES` + `GETCONNECTOR`).
  - `card0` is no longer assumed. This also fixes PCs where simpledrm leaves the real card at
    `card1`.
  - `LORD_CARD=/dev/dri/cardN` pins it, and lord-server's `--card` still works.
- **`lordos.service.d/rpi.conf`** waits up to 10 s for a connector to appear in `/sys/class/drm`
  (`wait-kms`, up to 30 s, and it asks for `vc4` with `modprobe`; lord-sessiond then waits up to 30 s more for a card with KMS before showing the "no display" text). If lord-sessiond starts before vc4 is loaded, without this it would only see v3d.
- **GBM and EGL** are created on the display card (vc4), as always. Mesa recognizes it (kmsro) and
  renders with v3d. Scanout buffers are created on vc4 and v3d imports them. lord-server doesn't need
  to know anything about this.
- **The linux-dmabuf feedback** announces the render node where Mesa renders: `gpu_device()` in
  `gpu/gpu.c`. That way apps (GTK, Qt, games) ask v3d for their buffers and not vc4, which has no
  render node. It looks it up like this:
  1. EGL (`EGL_EXT_device_query` + `EGL_EXT_device_drm_render_node`);
  2. the same card's render node;
  3. if the display card has none, the first render node there is (v3d).

  The log says `gpu: GPU composition (V3D 7.1…, render /dev/dri/renderD128, display vc4)`.

### Modifiers

- **vc4's primary plane** shows RGB only as **linear**. It also accepts `BROADCOM_VC4_T_TILED`, which
  v3d doesn't render, and `BROADCOM_SAND64/128/256` only for YUV (NV12/P030, video). v3d renders in
  **UIF** (`DRM_FORMAT_MOD_BROADCOM_UIF`), which vc4 can't display.
- **kms.c** now reads the `IN_FORMATS` property of the CRTC's primary plane (with
  `DRM_CLIENT_CAP_UNIVERSAL_PLANES`) and records the 32-bit RGB format/modifier pairs. The log says:

  ```
  kms: card vc4, XRGB8888 on the primary plane with modifiers: 0x0 …
  ```
- **Direct scanout** of a fullscreen app (`gpu_can_scanout`) now requires the plane to accept the
  buffer's modifier (`kms_scanout_ok`). A v3d UIF buffer isn't sent to the display (before, ADDFB2 was
  tried and, if the driver accepted it anyway, garbage showed up).
  - The implicit modifier (`INVALID`) counts as linear.
  - If the driver doesn't report `IN_FORMATS`, everything stays as before.
- **The UI surface** (`gbm_surface_create` with `SCANOUT | RENDERING`, without modifiers): with
  kmsro, Mesa creates it on vc4, linear. So it always displays.
- **Apps' dmabufs** are imported via EGL with their modifier. UIF is fine for textures: they're
  composited, they don't go to the display.

### What to verify on the board

1. `grep -E 'kms:|gpu:' ~/.cache/lordos/session.log` (or the session log). It has to show:
   - the chosen card, with the `vc4` driver;
   - the plane's modifiers (with `0x0`, linear, present);
   - `GPU composition (V3D 7.1…)`;
   - `render /dev/dri/renderD128`;
   - `cursor by hardware (cursor plane)`.
2. `journalctl -u lordos` shows `lord-sessiond: display: /dev/dri/card1` (or whichever).
3. `LORD_GPU_DEBUG=1`:
   - the list of dmabuf formats EGL announces (it has to include UIF `0x0700000000000006` and
     linear);
   - whether a fullscreen game goes through direct scanout or is composited.
4. `glmark2-es2-wayland` and `vkcube` windowed and fullscreen, with screenshots.
5. GTK4 (gtk4-demo) over dmabuf:
   - `WAYLAND_DEBUG=1` shows it requests v3d's render node;
   - the window looks right (no stripes from misinterpreted tiling).
6. vc4's hardware cursor (`CURSOR2`, 64×64).
7. Two monitors: today LordOS uses the first connected connector (HDMI-A-1); the second stays off.
8. 4K monitor. On the CPU, the 3840×2160 canvas is 33 MB per buffer and copying it is costly. With
   the GPU it should work well. If not, `video=HDMI-A-1:1920x1080@60`.

## Video (for the `lord-media` helper)

This part is for the media agent: it's what the Pi 5 needs besides VA-API, which is the PC path. Its
files weren't touched.

- **The Pi 5 has no hardware H.264 decoder** (the Pi 4 does, and it's no longer used). It only has
  **hardware HEVC/H.265** (`rpi-hevc-dec`), up to 4Kp60 at 8 and 10 bits.
  - It's a **V4L2 stateless** decoder ("request" API), at `/dev/video19` and a `/dev/mediaN`.
  - There's no VA-API or VDPAU on the Pi.
- **FFmpeg:**
  - Debian 13's (7.1) **doesn't include** the V4L2-request hwaccel; upstream doesn't have it yet.
  - Raspberry Pi's (`archive.raspberrypi.com`, `+rpt` versions) does: `-hwaccel drm`, with the
    `hevc` decoder and `drm_prime` output. Some versions expose it as `hevc_v4l2request`.

  Options:
  1. pin `ffmpeg` and the Raspberry Pi `libav*` at 990, like the kernel, only in the Pi image;
  2. have `lord-media` load an alternative `libavcodec`;
  3. speak V4L2-request directly from C (a lot of work).

  Recommended: option 1. Add
  `ffmpeg libavcodec61 libavformat61 libavutil59 libswscale8 libswresample5 libavfilter10` to
  `/etc/apt/preferences.d/raspberrypi` (verify the names with `apt-cache policy` on the board).
- **Decoded frames** are DRM PRIME:
  - **NV12 or P030 with the `BROADCOM_SAND128` modifier**: the Pi's "column" format;
  - `av_hwframe_map` / `AVDRMFrameDescriptor` give the fd, offset, pitch and modifier.

  To display them:
  - **On the GPU (best):** import them via EGL with the SAND128 modifier and
    `GL_OES_EGL_image_external`. Mesa v3d has to accept SAND128 (it is believed to convert it with
    the TFU): **verify** with `LORD_GPU_DEBUG=1`. lord-server already imports multiplanar dmabufs
    with modifiers (`gpu_tex_dmabuf`) if the format is in the feedback. Today `gpu_formats()` only
    announces 32-bit RGB (`known[]` in `gpu.c`): for video NV12/P030 have to be added.
  - **Through a vc4 overlay plane:** the HVS displays SAND128 directly (that's what Kodi does).
    LordOS doesn't use overlay planes yet (GPU.md, Pending).
  - **On the CPU:** `av_hwframe_transfer_data` to linear NV12 and convert. Good for 1080p; for 4K
    it's expensive.
- **H.264, VP9 and AV1 in software,** on the 4 Cortex-A76 cores (FFmpeg and dav1d have NEON):
  - H.264 1080p30/60 should work with 2–3 threads;
  - 4K H.264 isn't realistic;
  - AV1 1080p with dav1d, at the limit.

  Use `thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE` and `threads = 4`. With the Active Cooler,
  so it doesn't throttle.
- **On the Pi 4:** hardware H.264 (`h264_v4l2m2m`, V4L2 stateful, `/dev/video10`, in Debian's FFmpeg)
  and HEVC via `rpi-hevc-dec` as on the Pi 5.
- **What the media agent has to measure on the board:**
  - CPU and dropped frames with 1080p H.264, 1080p HEVC and 4K HEVC;
  - temperature (`vcgencmd measure_temp`);
  - throttling (`vcgencmd get_throttled`, it has to give `0x0`).

## Developing on the board

There are two possible systems on the board:

- **Raspberry Pi OS Lite 64-bit (Trixie):** the kernel and firmware are already there, and Mesa
  comes from the Raspberry Pi archive. The simplest way to start.
- **The LordOS image**, built with `RPI_SSH=1`.

### Steps

1. **SSH:**
   - with Raspberry Pi OS, enable it in Imager;
   - with LordOS, put an empty `ssh` file on the boot partition (or build with `RPI_SSH=1`) and use
     the account created at first boot;
   - `ssh user@<ip>`; the IP shows up on the login screen or with `ip -4 addr`.
2. **Packages for compiling** (the same as in `VM-SETUP.md` plus Mesa):
   ```sh
   sudo apt-get install -y --no-install-recommends build-essential pkg-config libxkbcommon-dev libwayland-dev \
     wayland-protocols libgbm-dev libegl-dev libgles-dev libssl-dev libcrypt-dev libsrtp2-dev libusrsctp-dev xkb-data \
     libgl1-mesa-dri libegl1 libgles2 libgbm1 mesa-utils glmark2-es2-wayland
   ```
3. **The tree,** from the development machine (as for the VM; the themes are built there):
   ```sh
   npm run build
   COPYFILE_DISABLE=1 tar czf - --no-xattrs --exclude build --exclude node_modules \
     package.json README.md server third_party assets themes tools | \
     ssh user@<pi-ip> 'rm -rf ~/LordOS && mkdir ~/LordOS && cd ~/LordOS && tar xzf -'
   ```
4. **Compile on the board,** natively, no cross-compiling:
   ```sh
   cd ~/LordOS
   make -C server -j4           # ARCH=aarch64 is detected: NEON=1 and GPU=1 if the -dev packages are there
   make -C server test          # all headless tests, including build/canvas-test
   server/build/canvas-test     # NEON = C, and how long each one takes
   ```
5. **Test lord-server alone,** from a console (Ctrl+Alt+F2, with lordos.service stopped):
   ```sh
   cd ~/LordOS/server && sudo build/lord-server --user "$USER" 2> /tmp/lord.log
   ```
6. **Install it on the LordOS image:** `sh tools/iso/mkdeb.sh /tmp/lordos.deb` (on the board it takes
   `arm64` from `dpkg --print-architecture`) and `sudo apt install /tmp/lordos.deb`.
7. **Screenshots:**
   - LordOS's remote desktop (Settings → Sharing, port 5673; `REMOTE.md`) from the development machine's browser,
     only on the LAN and turning it off afterward;
   - or `grim` inside a Wayland session.

   There's no QEMU `screendump`: it's a board.

## Test checklist on the board

Each item, with a screenshot or the pasted output, in this document.

| # | Test | How | Expected |
|---|---|---|---|
| 1 | Boot | `systemd-analyze`, `systemd-analyze blame \| head` | SD: <25 s; NVMe: <12 s to the login screen |
| 2 | First boot | freshly written image, with and without `lordos-user.txt` | partition grown (`df -h /`); login screen or setup mode; the file deleted |
| 3 | RAM | `free -m`, lord-server RSS (`ps -o rss`) | idle desktop <400 MB total; lord-server ~35–60 MB (with 16K pages RSS goes up a bit) |
| 4 | Card and GPU | the log (see "What to verify") | `vc4`, `V3D 7.1`, `renderD128`, hardware cursor |
| 5 | GPU compositing FPS | `glmark2-es2-wayland` windowed and fullscreen; dragging windows; the app launcher | 60 FPS synced to vblank |
| 6 | CPU path | `lordos.gpu=0` | usable at 1080p; note ms per frame with `--profile` |
| 7 | Modifiers | `LORD_GPU_DEBUG=1`, `kms:` in the log | linear on the plane; UIF only for textures; no garbage on screen |
| 8 | Apps' dmabuf | gtk4-demo, foot, FeatherPad (Qt6), `vkcube` | they look right; `WAYLAND_DEBUG` shows renderD128 |
| 9 | 1080p video | H.264 and HEVC (once `lord-media` is there) | no dropped frames; CPU noted |
| 10 | 4K video | hardware HEVC 4Kp60 | smooth; temperature noted |
| 11 | Temperature | `vcgencmd measure_temp`, `vcgencmd get_throttled`, LordOS Monitor (`cpu_thermal` and the fan) | <75 °C with the Active Cooler under load; `throttled=0x0` |
| 12 | NEON | `server/build/canvas-test` | "NEON matches C"; NEON faster than C |
| 13 | Headless tests | `make -C server test` and each `build/*-test` | all pass, as on x86 (WASM: the full wg-2.0 suite) |
| 14 | App isolation | open a `.lpk`; `sandbox[...]:` in the log; `cat /sys/kernel/security/lsm`; `make -C server test-full` | `capability,landlock`; Landlock and seccomp=1; `doc_evil.sh` 25/25 and `sandbox-test` ok. Needs the LordOS kernel: Raspberry Pi's has no Landlock (`6.18.50+rpt`: seccomp only) |
| 15 | Remote desktop | Settings → Sharing, `https://<ip>:5673` from the development machine | visible and controllable; screenshots for this document |
| 16 | Network and sound | Wi-Fi (brcmfmac), Ethernet, HDMI audio via PipeWire | IP from our own DHCP; `wpctl status` shows HDMI |
| 17 | Pi 4 | the same image | boots with `kernel8.img`; note what fails |

## 0.8.0 Beta image (2026-10-18)

`lordos-0.8.0-rpi5-arm64.img.xz` from the `v0.8.0-beta` tag (46c972a), built natively on the Pi 5 in
about 10 minutes with the LordOS kernels: 532,884,784 bytes, SHA-256
`827f3c550eb01f639f3f93f9551b8051a8c6791398b00135f5624d4e5b16724f` (2693 MB uncompressed, system
1448 MB). `make -C server test` on the board: everything passes except `npverify-test` 35/36, because
the board runs Raspberry Pi's archive kernel, which has no `/sys/kernel/btf/vmlinux`.

Checked read-only (loop device, `ro,noload`): `config.txt` (vc4-kms-v3d, PCIe gen 3, `include
lordos-case.txt`), `cmdline.txt` (`quiet splash`, `lsm=landlock,lockdown,yama,integrity,bpf`),
`kernel_2712.img` is the LordOS vmlinuz, Plymouth and the `lordos` theme are in the initramfs,
`Theme=lordos`, `lordos`, `lord-netperm`, `lord-fand` and `lordos-rpi-firstboot` enabled, SSH off,
`kernel.yama.ptrace_scope = 1`, pipewire-pulse module loading off, `lordos-user.txt.example`, the
`remote-desktop` switch file handled by `firstboot`, and the build id `20261018.46c972ac`.

Found while checking: **the image didn't enable `lord-netperm`** (the ISO and `install.sh` do), so
the per-app firewall was off on the Pi; `build-img.sh` enables it now. **The LordOS kernel has no
BPF LSM** (`CONFIG_BPF_LSM` is not set, and no BTF), so "No Bluetooth" for Linux apps isn't enforced
on the Pi (`lord-netperm` reports `"bluetooth":false`); the network and device programs work. Turning
on `CONFIG_BPF_LSM` and `CONFIG_DEBUG_INFO_BTF` in `build-kernel.sh` is pending. **The image has not
been booted on a board yet.** (Later: both options are on since 0.8.1's `-2+lordos` kernel, see "The
LordOS kernel".)

## Results on the board (2026-10-18)

Raspberry Pi 5, 16 GB, 32 GB SD card, no fan load, **no monitor on HDMI** (both HDMI connectors
`disconnected`). Raspberry Pi OS-style Debian 13 with kernel `6.18.50+rpt-rpi-2712` (16K pages), GCC
14.2, Mesa 26.2.2 from the Raspberry Pi archive, FFmpeg 7.1.5+rpt. Tree copied with rsync; themes built
on the development machine.

| What | Result |
|---|---|
| `make -C server -j4` (GPU=1, NEON=1) | ✅ every binary: lord-server, lord-sessiond, lord-netd, lord-netperm (BPF with clang), lord-doc, lord-media, lord-vault, lord-maild. 1 min 02 s. One GCC 14 `-Wformat-truncation` warning (`apps.c`), fixed: now 0 warnings |
| `make -C server test` | ✅ `23 passed, 0 failed, 0 skipped` (with skkdic, rime-data-pinyin-simp, unicode-data and libthai-dev installed), plus engine-test 2642 checks, backup-test 220, vault-test 74, import-test 150 |
| NEON (`build/canvas-test`) | ✅ "NEON matches C". blit 9.90 → 4.33 ms/screen, mask 11.09 → 5.49 ms/screen (2.0–2.3×) |
| `make -C server test-full` | ⚠️ media-test probe/wave/sandbox ✅. `doc_evil.sh` 20/25: the 5 failures are the filesystem confinement checks (`~/.ssh`, home, `/tmp`), because **the Raspberry Pi kernel has no Landlock** (`# CONFIG_SECURITY_LANDLOCK is not set`, `/sys/kernel/security/lsm` = `capability`). seccomp works on aarch64 (`seccomp=1(tsync)`, no sockets, no fork). `sandbox-test` skipped for the same reason |
| arm64 `.deb` (`DEB_ARCH=arm64 tools/iso/mkdeb.sh`) | ✅ 5.0 MB; dependencies resolve on Raspberry Pi OS (its `+rpt` FFmpeg keeps the Debian package names) |
| lordos.service | ✅ `lord-sessiond: display: /dev/dri/card1` (vc4; v3d is card0, `drmcard.h` picks the right one). With nothing connected the login screen stays headless (`kms: no monitor connected`) instead of crash-looping. lord-server keeps the lent card and retries on the next DRM uevent (`main.c`, parked fds). Until 0.8.5 the login screen never opened the uevent socket (`hw_start` ran only in sessions), so a monitor plugged in after boot stayed black until a restart; the login screen now starts `hw` without the bus. Retry checked on the board with `echo change > /sys/class/drm/card1/uevent` (the greeter tries the card again) |
| Idle (login screen, headless) | lord-server 36 MB RSS, 0.6 % CPU; lord-sessiond 4.4 + 2.5 MB; system total 450 MB used of 16 GB; 38–43 °C, `throttled=0x0`, 2.4 GHz |
| EGL/GBM | `V3D 7.1.10.2`, OpenGL ES 3.1 (`eglinfo -p gbm`). LordOS's GPU path on screen not exercised (no monitor) |
| Video (FFmpeg CLI) | HEVC 1080p30 through `rpi-hevc-dec` (`/dev/video19`, `-hwaccel drm -hwaccel_output_format drm_prime`): 300 frames in 0.54 s with 0.12 s of CPU. Software, 4 threads: HEVC 1080p 251 fps, H.264 1080p ~420 fps (synthetic `testsrc2` clip: real content is heavier). lord-media only knows VA-API, so inside LordOS HEVC is software for now |
| `build-img.sh` natively on the Pi | ✅ after the key fix (above): 7 min 58 s from the SD card. `/var/tmp/lordos-rpi/out/lordos-0.7.0-rpi5-arm64.img.xz`, 500 MB (2651 MB uncompressed, system 1412 MB). Kernel 6.18.50+rpt, `kernel_2712.img`/`kernel8.img`, initramfs, 386 overlays. **The image hasn't been booted yet** (the board's only SD is the running system) |
| LordOS kernel (`build-kernel.sh all`, natively) | ✅ `6.18.57-lordos-rpi-2712` (50 min) and `6.18.57-lordos-rpi-v8` (49 min), Landlock built in; installed in a chroot: `raspi-firmware`'s hook copied `kernel_2712.img`, initramfs, device trees and 388 overlays. **Not booted yet** (the board's SD is the running system) |
| `build-img.sh` with the LordOS kernels, lord-fand and the switch files | ✅ 8 min 18 s. `lordos-0.7.0-rpi5-arm64.img.xz`, 503,485,468 bytes (2673 MB uncompressed, system 1431 MB), SHA-256 `d47778e6cb8a614b3bb104d14f808dcdf27a4a4f6f61dc50c055cccc9f5cb045`. Checked read-only: `kernel_2712.img` = our vmlinuz, `lsm=landlock,…` in `cmdline.txt`, the apt pin, SSH installed but off, `lord-fand` enabled, `verify-board` present |
| `firstboot` switch files (in a chroot of the image) | ✅ user + Wi-Fi from `lordos-user.txt` (SSID with quotes, `wifi_country` → `cmdline.txt`), `remote-desktop` with `port=5800` → `/etc/lordos/remote/config`, kept while the only account had no password, `allow=group` creates `lordos-remote`, a bad `port=99` falls back to 5673; the files are deleted |

Found and fixed on the board:

- **lord-netd vs NetworkManager.** Raspberry Pi OS uses NetworkManager (with netplan). lord-netd only
  knew systemd-networkd and ifupdown, so it saw `eth0` and `wlan0` as free and started its own DHCP
  client and a second `wpa_supplicant` on them, plus duplicate default routes. `managed_by_system()`
  (`net/state.c`) now also reads `/run/NetworkManager/devices/<ifindex>` (`managed=true`); `net-test
  state` shows both as `"managed":"system"`. The LordOS image has no NetworkManager, so there
  lord-netd still owns the network.
- **The Raspberry Pi archive key** (SHA-1 signatures rejected by apt; see "Building it").
- **Default language** of the image and first boot: now English, like the ISO (it was Spanish).

Notes for the image:

- Raspberry Pi OS boots with `cgroup_disable=memory`; the image's `cmdline.txt` already has
  `cgroup_enable=memory` (per-app memory limits need it).
- `lsm=landlock,…` in the image's `cmdline.txt` does nothing on Raspberry Pi's kernel: Landlock isn't
  compiled in, and isolation there is seccomp + cgroups + no-new-privs only. **Fixed:** the image now
  ships the LordOS kernel with `CONFIG_SECURITY_LANDLOCK=y` ("The LordOS kernel").
- The image takes Mesa (25.0.7) and FFmpeg (7.1.5) from Debian. Debian's FFmpeg has no V4L2-request
  decoding, so the hardware HEVC seen above needs the Raspberry Pi FFmpeg pinned (option 1 in
  "Video").

Still to do on the board: everything in the checklist that needs a monitor (items 1, 2, 4–8, 10, 15
with screenshots, 16 HDMI audio, 17), the desktop session, and booting the built image from a
second SD card or an NVMe. (Later: a 0.8.4 image was booted from a test SD card for the 0.8.4.1
release test, `ISO.md`, and the desktop shows on HDMI since 0.8.4.1, see the status at the top; the
measurements with a monitor are still to do.)

## Server portability audit (aarch64)

A review of `server/` on the development machine, without compiling for Linux ARM. The changed files were checked
with `clang -arch arm64 -fsyntax-only -Wall -Wextra` against the Linux 6.12 DRM headers and the
EGL/GLES/GBM ones. What follows is the result.

**There was nothing x86-specific:**
- no SSE/AVX intrinsics or asm in `server/src` (blending, blur and scaling are portable C, with
  32-bit SWAR);
- no `-march=native` in the Makefile.

**Fixed:**

| Where | What |
|---|---|
| `server/Makefile` | `ARCH` (from `$(CC) -dumpmachine`). On aarch64, `NEON=1` by default (`-DLORD_NEON`; `NEON=0` turns it off). QuickJS with `-ffp-contract=off`: GCC in GNU mode fuses `a*b+c` into FMA on ARM and would change JS numeric results; our code uses `-std=c11`, which already turns it off. `$(LD)` instead of `ld` for `pages.o` (cross-compiling). New test `build/canvas-test` |
| `server/src/canvas.c` | The rows of `canvas_blit` (without corners) and of `canvas_alpha_mask` (text, shadows) are separate functions, with a NEON version (4 pixels per iteration) under `LORD_NEON && __ARM_NEON`. Bit-for-bit identical to C with valid premultiplied sources: checked with `tests/canvas_test.c` under emulation (SIMDe) on the development machine, 4000 random rows. Still to do on the board |
| `server/src/ui/stb.c` | `STBI_NEON` on aarch64: stb's JPEG uses NEON (on x86 it already used SSE2) |
| `server/src/term/vt.c:890` | `src[i] >= 0 && …` with `char`: on ARM `char` is unsigned and `-Wextra` warns (`-Wtype-limits`). Now `(unsigned char)src[i] < 0x20`, the same on both architectures |
| `server/src/kms.c` `kms_dispatch` | the DRM event buffer aligned to 8 (it's read with a cast to `drm_event_vblank`, which has 64-bit fields) |
| `server/src/drmcard.h` (new), `main.c`, `remote/monitor.c` | pick the card with connectors instead of `card0` (see above) |
| `server/src/kms.c`/`kms.h`, `gpu/gpu.c` | the primary plane's `IN_FORMATS`, `kms_scanout_ok`, EGL's render node for the dmabuf feedback (see above) |
| `server/src/ui/js_sys.c` | `lord.system.info().cpu`: on ARM64 `/proc/cpuinfo` has no "model name". Now it comes from "CPU part" and `/proc/device-tree/model`: «Cortex-A76 · Raspberry Pi 5 Model B Rev 1.0» |
| `server/src/sec/sandbox.c` (fixed by the sandbox agent, on request) | `SYS_epoll_wait` without `#ifdef`: on aarch64 it doesn't exist and didn't compile. The repeated `SYS_read` was removed |

**Reviewed, no changes:**
- **Page size (16K on the Pi 5):** there's no hardcoded 4096 in `mmap`, shm or buffers.
  - Dumb buffers use `CREATE_DUMB`'s `size` and `pitch`.
  - WASM memory reserves and protects in multiples of 64 KiB (a multiple of 16K).
  - The Monitor uses `sysconf(_SC_PAGESIZE)` (`monitor/mon.c`).
  - The dmabuf feedback does `memfd` + `mmap` of an arbitrary size, which is valid.
- **Unaligned reads and endianness:**
  - the OpenType, MaxMind DB, xz, PNG/JPEG (stb), thbrk and IME readers read byte by byte in
    big-endian or with `memcpy`;
  - netlink attributes (`net/state.c`, `nl.c`, `wg.c`) are 4-aligned by the format.
  - Everything is little-endian, like x86.
- **Unsigned `char`:** the UTF-8 decoders and JSON escapes already use `unsigned char` (`mon.c`,
  `nl.c`, `snd.c`, `textlayout.c`, `json.c`, `wl.c`, `conn.c`); the other `char`s are only compared
  with ASCII.
- **Structures and ioctls:** `input_event` (24 bytes), `drm_mode_*` and `EVIOCG*` are the same on LP64
  (x86-64 and aarch64). There's no fbdev code.
- **`SYS_close_range` and `SYS_capset`** exist on aarch64.
- **WASM:** float→int conversions check the range before the cast, NaNs are handled by hand
  (`QNAN32/64`) and loads use `memcpy`. Run `build/wasm-test` on the board.
- **QuickJS 2025-04-26** supports aarch64.
- **`pkg/status.c`** already maps `aarch64` → `arm64` for dpkg and the Store.
- **Threads:** `PTHREAD_STACK_MIN` is 128 KiB on aarch64; the Monitor threads ask for 512 KiB, which
  is enough.

**Pending for the on-board stage** (risks, with file:line):

| Where | Risk |
|---|---|
| `server/src/main.c:488` (`mirror_frame(out.px…)` on the CPU path) | With remote desktop open, it reads from the mapped dumb buffer. On ARM that memory is write-combined/uncached and reading it is very slow; on x86 too, but less so. Better to mirror from `s->scene` + `wl_paint_fullscreen` to a buffer in RAM. Measure before touching |
| `server/src/main.c:484–498` (`canvas_copy`, line 486) | The CPU path copies the whole canvas to the dumb buffer (`canvas_copy`) every frame. At 4K that's 33 MB per frame, too much for the Pi's memory. Copy only the damage |
| `server/src/canvas.c` `box_pass`/`canvas_backdrop_blur` | The vertical pass walks by columns (`step = sw`), which is slow with the A76's cache. Candidate for NEON (4 columns at a time) or transposing; measure with `--profile` |
| `server/src/gpu/gpu.c` `gpu_formats` (`known[]`) | Only announces 32-bit RGB: the Pi's SAND128/NV12 video isn't announced (see Video) |
| `server/src/gpu/gpu.c` `gpu_device` | That EGL returns the render node with kmsro is unverified. If it returns nothing, it falls back to the first render node, which on the Pi is v3d |
| `server/src/kms.c` `read_scanout_formats` | `IN_FORMATS` with `UNIVERSAL_PLANES` untested on vc4. If it fails, it stays as before |
| `server/src/drmcard.h` | `GETCONNECTOR` forces an EDID read per card at startup (~50 ms). Hotplug: if HDMI is plugged in after boot, lord-sessiond has already chosen the card (vc4 anyway, because it has connectors) |
| `server/src/sec/sandbox.c` | seccomp: check on the board that no `.lpk` app dies from a syscall that arm64 glibc uses and x86 doesn't. Landlock: the Raspberry Pi kernel might not have it; `cmdline.txt` asks for `lsm=landlock,…`. The shared helper profile (`sandbox_apply_helper`) uses the same list and is fine on arm64, but has no `ioctl`: hardware video (V4L2 on the Pi, VA-API on the PC) inside the sandbox needs it (the sandbox agent was told) |
| `server/src/monitor/mon.c` | The Monitor's "model name" stayed empty on ARM. Fixed: `lord.monitor.info().cpuModel` now comes from `js_sys_cpu_name()`, the same one About uses (`js_monitor.c`) |
| `server/src/install/install.c:677` | The installer uses `grub-install --target=x86_64-efi`. On the Pi it doesn't apply (it's not live, the installer doesn't appear), but it's what would have to change for an ARM64/UEFI installer |
| page size | Some third-party program (not LordOS) assumes 4K: Widevine, box64, old jemalloc. If needed, `kernel=kernel8.img` in `config.txt` (4K, a bit slower) |
