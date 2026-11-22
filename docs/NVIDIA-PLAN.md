# NVIDIA graphics cards — plan and designs for approval

**Status (2026-11-22).** The maintainer approved NVIDIA support as **"experimental, community-verified", with hardware
reports**. That first part is built on `feat/gpu-reports` (in development): an NVIDIA card is marked experimental in
Settings → Graphics cards and every computer can send a compatibility report. **Parts A and B below are designs only,
FOR REVIEW**: nothing in them is built, no admin action is added and no package is installed until the maintainer
approves each closed action listed here (AGENTS.md: every root action needs the same review). Nothing here is claimed
to work on any card until the test matrix says so.

Related: `docs/GPU.md` (lord-server's GPU path), `docs/MACHINES-GPU.md` (Settings → Graphics cards, `gpu-role`),
`docs/HARDWARE-REPORT.md` (one NVIDIA report so far, #171: an RTX 3060 on NVIDIA's proprietary driver, desktop composed
on the CPU), `docs/DESKTOP-PARITY.md` (`linux-drm-syncobj-v1`).

## Part 0 — built now: the experimental status and the compatibility report

- **Settings → Graphics cards** shows on every computer with a GPU (before: only with two or more). An NVIDIA card
  (PCI vendor `10de`) says *"Experimental — not verified on your card model yet"*, has **How to Help Test It** (the Help
  article `graphics-cards`) and lists its roles **AI and compute (CUDA)** and **Desktop** as *Planned*, with a note
  saying why; there is no button that pretends to turn them on. Each card shows its driver with its version (`nvidia
  560.35.03 (open)`, `nouveau (Mesa 25.0.7)`, `amdgpu (Mesa …)`).
- **Send a compatibility report** (any card): `lord.sysinfo.gpuReport()` builds a plain-text report in C
  (`server/src/sysreport/gpureport.c`) from world-readable files only: the cards (PCI ids, pci.ids names, kernel driver,
  boot display, render node), **which card draws the desktop** (the DRM card lord-server holds open), NVIDIA's module
  (loaded, version, open or proprietary kernel modules), `nvidia-drm` `modeset` and `fbdev`, `nouveau` loaded, CUDA's
  userspace (`libcuda.so.1`, `nvidia-smi`, `/dev/nvidia-uvm`), Mesa's version (from `libgallium-<version>.so`), the
  Vulkan ICDs with their API version, VA-API and VDPAU drivers, OpenCL vendors, Secure Boot, the kernel's lockdown
  mode, only the graphics options of the kernel command line, and this session's composition (`gpu`/`cpu`/…, the GPU
  start's reason) and GL renderer. No probe program is run (no `glxinfo`, `vulkaninfo`, `nvidia-smi`): the GL facts are
  the desktop's own context's.
- **Privacy:** the report is made of fixed fields with no place for names, serials, MACs or home paths, then scrubbed
  again (C: `gpureport_redacted`, JS: `scrubReport`): the home → `~`, `/home/<name>` → `/home/<user>`, the user's and
  host's names as whole words, MAC addresses, UUIDs, e-mail addresses and serial values. Tests:
  `build/gpureport-test` (a fake machine full of such values, ASan/UBSan, random input) and `tests/gpu_report_test.mjs`.
- **Sending:** the whole text is shown first; **Open the Form on GitHub** copies it to the clipboard and opens
  `.github/ISSUE_TEMPLATE/gpu-report.yml` prefilled (version, card, driver, report; under `ISSUE_URL_MAX`, cut on whole
  lines and marked). Nothing is sent by LordOS. Without a browser: `lordos-issue --gpu` (or `lordos-sysreport gpu`)
  prints the same report without the session's part.

## Part A — design: the AI / compute role (CUDA) — FOR REVIEW

Goal: on a computer with an NVIDIA card, an administrator can choose **AI and compute (CUDA)** in Settings → Graphics
cards, and LordOS installs NVIDIA's driver and CUDA's driver library from **Debian 13's own non-free archive** with its
own package engine (`server/src/pkg`, then `dpkg`; never apt or libapt), signs nothing itself, and guides the Secure Boot
key enrolment. The desktop stays where it is (Part B is separate).

### A.1 What is installed

Fixed package sets, named in the signed `lordos` package (never from JS):

| Flavour | Packages | For |
|---|---|---|
| `open` | `nvidia-open-kernel-dkms`, `nvidia-driver`, `libcuda1`, `nvidia-smi`, `linux-headers-<flavour>` | Turing (RTX 20 / GTX 16) and newer, NVIDIA's recommended modules there |
| `proprietary` | `nvidia-kernel-dkms`, `nvidia-driver`, `libcuda1`, `nvidia-smi`, `linux-headers-<flavour>` | Maxwell and Pascal (GTX 9xx, GTX 10) |

- `linux-headers-<flavour>` is `linux-headers-amd64` (or `-cloud-amd64` for that kernel), chosen root-side from the
  running kernel's release; DKMS builds the module for it, and again at each kernel upgrade (its trigger runs inside
  `dpkg`, so it works under our engine as under apt; to be confirmed on the matrix).
- The flavour is chosen **root-side** from a fixed table of NVIDIA device-id ranges by generation in the signed package
  (`nvgen.json`), never by the caller. A card older than Maxwell, or newer than what Debian 13's driver supports
  (Blackwell, RTX 50, needs a newer driver than Debian 13's at the time of writing: to verify), is refused with a
  message, never "tried".
- CUDA's **toolkit** (compilers) is not installed: apps (llama.cpp, Ollama, PyTorch wheels, Flatpak apps) bring their
  own CUDA runtime and need only the driver's `libcuda`.
- phase 1 is amd64 only.

### A.2 Debian's non-free component

LordOS's ISO already writes `main contrib non-free non-free-firmware`; a LordOS installed over Debian with
`install.sh` may not have `non-free`. The engine refuses packages from components the sources don't list.

### A.3 Secure Boot (MOK enrolment, guided)

With Secure Boot on, an unsigned module doesn't load. Debian's DKMS signs the modules it builds with its own key
(`/var/lib/dkms/mok.key`, `mok.pub`, made at the first build); the key must be **enrolled in shim's MOK list**, which
only the person can confirm, at the next boot, in the firmware's blue MokManager screen.

Flow: after the install, if `SecureBoot` is on and `mokutil --test-key /var/lib/dkms/mok.pub` says it isn't enrolled,
Settings shows *"One more step at the next restart"*, then `gpu-mok-enroll import`: root makes a **one-time password**
(8 digits, from `getrandom`), hashes it (`mokutil --generate-hash`), runs `mokutil --import /var/lib/dkms/mok.pub
--hash-file <root's 0600 temp file>` and returns the digits once. Settings shows them large with step-by-step text and
a picture of MokManager: *Enroll MOK → Continue → Yes → type the password → Reboot*. After the restart Settings checks
`mokutil --test-key` again and that the module loaded (`/sys/module/nvidia`); if not, it says what happened and offers
**Try Again** (a new import) or **Cancel** (`gpu-mok-enroll cancel`: `mokutil --revoke-import`). The password never
touches disk outside root's temp file (removed at once), the log or the clipboard.

### A.4 Flatpak's GL.nvidia extension

Flatpak apps see NVIDIA's userspace only through `org.freedesktop.Platform.GL.nvidia-<version>` (and `GL32.nvidia-…` for
32-bit, Steam), whose version must match the host driver's exactly (`550.163.01` → `GL.nvidia-550-163-01`). A driver
upgrade without the matching extension leaves Flatpak apps on software rendering. Design: after `gpu-nvidia-install` and
after every `pkg-upgrade` that changes `nvidia-driver`, lord-server reads `/proc/driver/nvidia/version` (or the
package's version before the restart) and, when Flatpak is installed, installs the matching extension through the
existing Flatpak path (`lord.flatpak`, the same installation as the apps, Flathub's verified signature) and removes the
previous one once nothing uses it. No new admin action if the apps' installation is per user; with the system
installation it rides on the existing Flatpak install action. Settings → Flatpak shows it as part of the driver.

### A.5 The closed admin actions (new, need approval)

Every argument is checked again on the root side; the GPU set is what root reads from `/sys/bus/pci/devices` itself;
programs run from fixed paths with fixed arguments; packages only through the engine.

1. **`gpu-nvidia-sources "<on|off>"`** (administrators). `on` writes `/etc/apt/sources.list.d/lordos-debian-nonfree.sources`
   with fixed text: `deb.debian.org/debian` `<suite> <suite>-updates` and `security.debian.org/debian-security
   <suite>-security`, component `non-free` only, `Signed-By: /usr/share/keyrings/debian-archive-keyring.gpg`; `<suite>`
   is read root-side from `/etc/os-release` (`VERSION_CODENAME`, must be `trixie`), then runs `pkg-update`. Skipped
   (answers ok) when the existing sources already list `non-free` for that archive. `off` removes only that file, and
   only when no package from `non-free` is installed (`status.c`).
2. **`gpu-nvidia-install "<pci address>"`** (administrators). The address must be a PCI device root lists, vendor
   `0x10de`, class `0x03xxxx`. Root picks the flavour (A.1), checks the non-free source is there (else answers
   `gpu.needNonFree`), resolves and installs the fixed package set through the engine (progress lines as `pkg-install`),
   writes `/etc/modprobe.d/lordos-nvidia.conf` with fixed text (`options nvidia-drm modeset=1 fbdev=1`, needed for KMS
   on a computer where this card drives a display), runs `update-initramfs -u`, and records the role
   (`gpu-role <address> ai` stops answering `gpu.roleNotYet` only for an NVIDIA card with the driver installed).
   Debian's packages blacklist `nouveau` themselves; root verifies that file is there. Takes effect after a restart.
3. **`gpu-nvidia-remove`** (administrators, no argument). Removes the fixed package set through the engine (purge),
   `lordos-nvidia.conf` and the role, runs `update-initramfs -u`; `nouveau` is back after a restart. Never removes
   anything outside the set.
4. **`gpu-mok-enroll "<import|cancel>"`** (administrators). `import` only when Secure Boot is on, the key pair exists at
   DKMS's fixed path, owned by root, not a link, and isn't enrolled; runs `/usr/bin/mokutil` as in A.3 and returns the
   one-time password. `cancel` runs `mokutil --revoke-import`. `mokutil` is a dependency of the `lordos` package.

### A.6 What Settings shows

*AI and compute (CUDA)* gets a **Set Up** button (administrators) that explains first: the size (headers + DKMS build,
several hundred MB), that the driver is NVIDIA's non-free software, that the screen may flicker during the build, that a
restart is needed and, with Secure Boot on, the one-time password. A progress view follows the engine's lines. After the
restart: the driver's version, `nvidia-smi`'s presence, CUDA *Ready*, and **Remove**. Until approved, the role stays
*Planned* as built in Part 0.

## Part B — design: NVIDIA drawing the desktop — FOR REVIEW

Today an NVIDIA card shows the desktop with `nouveau` (Mesa) or, on NVIDIA's driver, with the screen composed on the
CPU (#171: the GPU start ends before a GL context; the report's `GPU start:` reason will tell which step per card).

### B.1 What lord-server needs from NVIDIA's driver

- **KMS:** `nvidia-drm.modeset=1` (A.5's fixed file) so `/dev/dri/cardN` does atomic modesetting; lord-server drives
  KMS itself (`kms.c`, no libdrm). `fbdev=1` keeps a console and a clean hand-off to the greeter (to verify per
  generation).
- **GBM:** `gpu.c` creates a GBM device on the card's fd. Mesa's `libgbm` loads NVIDIA's backend
  (`gbm/nvidia-drm_gbm.so`, package `libnvidia-egl-gbm1`) — it must be in A.1's set for the desktop role.
- **EGL/GLES:** through glvnd (`libEGL_nvidia.so.0`); `EGL_PLATFORM_GBM_KHR`, an XRGB8888 config, GLES2. The renderer
  check that refuses software renderers must accept NVIDIA's string.
- **Buffers and modifiers:** NVIDIA's scanout buffers use its block-linear modifiers: framebuffers must be added with
  `ADDFB2` + modifiers from the plane's `IN_FORMATS` (`kms.c` already reads them); the cursor plane wants linear
  buffers. dmabuf import of clients' buffers needs `EGL_EXT_image_dma_buf_import_modifiers`; on hybrid laptops, buffers
  crossing GPUs (PRIME) must be linear.
- **Synchronisation:** NVIDIA's driver doesn't do implicit sync on dmabufs. Without **explicit sync
  (`linux-drm-syncobj-v1`)** GL and Vulkan clients and Xwayland flicker or show old frames. This is the hard
  dependency: **the desktop on NVIDIA's driver waits for the explicit-sync design the Wayland work is writing**
  (timeline syncobj import, waiting on each surface's acquire point before sampling it, signalling its release point
  after the frame's page flip, `DRM_CAP_SYNCOBJ_TIMELINE`). Until that design is approved and built, the desktop on
  NVIDIA's driver stays composed on the CPU and Settings keeps *Desktop: Planned*.

### B.2 Turning it on, and getting back

- **Opt-in:** *Desktop* becomes available only for a card whose generation passed the matrix (B.3), after A's install,
  and only with explicit sync built. Choosing it writes nothing new: lord-server simply composes on the card once
  `gpu_init` succeeds on it; `LORD_GPU=0` and the CPU path stay as they are.
- **If the display stays dark after a restart:** a **boot fallback**. Design (needs its own approval, a root
  capability for `lord-sessiond`): before the first restart with NVIDIA's driver, `gpu-nvidia-install` adds a GRUB entry
  *LordOS (safe graphics)* with `modprobe.blacklist=nvidia,nvidia_drm,nvidia_modeset nouveau.modeset=1` (a fixed file
  in `/etc/grub.d`, then `update-grub`), and arms a counter in `/var/lib/lordos/gpu-boot`; `lord-sessiond` clears it when
  the greeter has shown a frame. Two boots in a row that don't reach the greeter make the next boot use the safe entry
  (`grub-reboot`), and Settings then explains what happened and offers **Remove the Driver**. The entry stays in the
  boot menu while the driver is installed, so a person can always choose it by hand.
- **Hybrid laptops** (Intel/AMD + NVIDIA): the integrated GPU keeps the desktop; NVIDIA is for compute (A) and, later,
  per-app offload (out of scope here).

### B.3 Test matrix (by generation)

No NVIDIA card is owned by the team today: every cell is filled by community reports (Part 0) or a lent card, and
nothing is claimed before its cell says so.

| Check | GTX 10 (Pascal) | RTX 20 / GTX 16 (Turing) | RTX 30 (Ampere) | RTX 40 (Ada) | RTX 50 (Blackwell) |
|---|---|---|---|---|---|
| Kernel modules to use | proprietary | open | open | open | open (newer driver than Debian 13's: to verify) |
| Desktop on `nouveau` (CPU / GPU composition) | | | | | |
| A: install, DKMS build, `nvidia-smi`, CUDA (llama.cpp / Ollama) | | | | | |
| A: Secure Boot on: MOK enrolment end to end | | | | | |
| A: kernel upgrade → DKMS rebuild; driver upgrade → GL.nvidia extension | | | | | |
| A: remove → back on `nouveau` | | | | | |
| B: KMS with `modeset=1`, greeter hand-off, VT switch | | | | | |
| B: GBM/EGL start, GPU composition, direct scanout | | | | | |
| B: explicit-sync clients (GTK4, Chromium, Firefox, Xwayland, Vulkan games) | | | | | |
| B: two monitors, mixed refresh rates, VRR | | | | | |
| B: suspend and resume (video memory kept) | | | | | |
| B: boot fallback after a forced failure | | | | | |
| Hybrid laptop (iGPU desktop, NVIDIA compute) | | | | | |

A cell is filled with the report's issue number and ✓ / partly / ✗. A generation moves to *community-verified* on the
site's "Tested graphics cards" page (draft: `web/content/hardware/gpus.*.md`) with two matching reports.

## What needs the maintainer's OK

1. Part A's four closed actions (`gpu-nvidia-sources`, `gpu-nvidia-install`, `gpu-nvidia-remove`, `gpu-mok-enroll`),
   the package sets and the device-id table, and `gpu-role … ai` becoming real for NVIDIA.
2. The Flatpak GL.nvidia handling (A.4).
3. Part B, after the explicit-sync design: the desktop role, and the boot fallback (a GRUB entry and `lord-sessiond`'s
   counter).
