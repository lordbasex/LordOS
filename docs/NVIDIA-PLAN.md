# NVIDIA graphics cards — plan and designs for approval

**Status (2026-11-22).** The maintainer approved NVIDIA support as **"experimental, community-verified", with hardware
reports**. That first part is built on `feat/gpu-reports` (in development): an NVIDIA card is marked experimental in
Settings → Graphics cards and every computer can send a compatibility report. **Part A follows the maintainer's
decision of 2026-11-22** (the driver as an opt-in from the Store's suggestion card, NVIDIA's own repository through the
existing vendor-repository mechanism) and asks for one closed action only, `gpu-mok-enroll`. **Parts A and B are designs,
FOR REVIEW**: nothing in them is built, no admin action is added and no package is installed until the maintainer
approves (AGENTS.md: every root action needs the same review). Nothing here is claimed to work on any card until the
test matrix says so; facts marked *to verify* were not checked.

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

## Part A — design: NVIDIA's driver from the Store, CUDA as an option — FOR REVIEW

**The maintainer's decision (2026-11-22).** The default stays free: `nouveau` and Mesa, nothing proprietary installed
or enabled unless the person chooses it. The person opts in from the Store's existing **Suggested for your computer**
card for NVIDIA (rule `nvidiaDriver` in `assets/store/suggestions.json`, the same mechanism as the Raspberry Pi,
firmware and printer cards). For current cards the driver comes from **NVIDIA's own signed APT repository**, added with
the existing catalog-based `pkg-source-add <id>` (PACKAGES.md, "Vendor repositories") and removed with the last package
by `pkg-source-remove`. The card picks by generation; CUDA is a separate optional choice; Settings → Graphics cards only
opens that card. No new admin action for the repository or the install: of the four the earlier design proposed, only
`gpu-mok-enroll` is still requested (A.9).

### A.1 Facts (checked 2026-11-22)

| Fact | Source |
|---|---|
| NVIDIA's repository has `debian13/` (x86_64 and `sbsa`), next to `debian10`–`debian12` | directory listing of `developer.download.nvidia.com/compute/cuda/repos/` |
| It is a **flat** repository: the keyring package's source line is `deb [signed-by=/usr/share/keyrings/cuda-archive-keyring.gpg] https://developer.download.nvidia.com/compute/cuda/repos/debian13/x86_64/ /`; `InRelease` and `Packages.gz` sit next to it (no `dists/`); `InRelease` has no `Suite`, `Codename` or `Valid-Until` | `cuda-keyring_1.1-1_all.deb` and `InRelease` from that directory |
| Keyring package `cuda-keyring` 1.1-1 ("Enrolls public key: 8793F200 (2025)"): ships the conffile `/etc/apt/sources.list.d/cuda-debian13-x86_64.list` and `/usr/share/keyrings/cuda-archive-keyring.gpg`; its postinst only warns about the old `7fa2af80` key | the `.deb` itself |
| Key: RSA 4096, 2025-08-15, `Kitmaker (Debian 13 Trixie) <kitmaker@nvidia.com>`, fingerprint **`0218 2E60 104F CDC2 6EAE 1B85 97A5 D4CB 8793 F200`**; the same key is in `debian13/x86_64/8793F200.pub` and in the keyring package, and verifies the repository's `InRelease` (signed 2026-11-18) | files served by NVIDIA, read with `gpg --show-keys`, `gpg --verify` |
| Driver branches there: 590.44.01 … **620.70.06** (590, 595, 610, 615, 620); no 580 | `debian13/x86_64/Packages.gz` |
| Packages: `nvidia-open` (driver + CUDA libs + `nvidia-settings`), `cuda-drivers` (→ `nvidia-open`), `nvidia-driver`, `nvidia-driver-cuda` (provides `nvidia-smi`, pulls `libcuda1`), `nvidia-kernel-open-dkms` (needs `dkms (>= 3.1.8)`, `g++`, `firmware-nvidia-gsp`), `nvidia-kernel-dkms`, `nvidia-driver-libs`, `libnvidia-egl-gbm1`, `libnvidia-egl-wayland21`, `nvidia-driver-pinning-<branch>` / `-<version>`, `nvidia-driver-assistant`, `cuda-toolkit-13-1` … `cuda-toolkit-13-5` | same |
| NVIDIA's guide (Debian 12 and 13): full `apt install nvidia-open`; compute-only `nvidia-driver-cuda nvidia-kernel-open-dkms`; desktop-only `nvidia-driver nvidia-kernel-open-dkms`; install `nvidia-driver-pinning-<branch>` first to stay on a branch; enable `contrib`. Only open-module commands are given; nothing about Secure Boot or Debian's own packages | [Driver Installation Guide, Debian](https://docs.nvidia.com/datacenter/tesla/driver-installation-guide/debian.html) |
| Open vs proprietary modules: open for Turing and newer (recommended), **required** for Grace Hopper and Blackwell; Maxwell, Pascal and Volta can't use them; mixed old + new GPUs stay proprietary. Open is the default from R560 | [NVIDIA blog, "NVIDIA transitions fully towards open-source GPU kernel modules"](https://developer.nvidia.com/blog/nvidia-transitions-fully-towards-open-source-gpu-kernel-modules/) |
| From 590 the proprietary flavour supports Turing, Ampere, Ada and Hopper only, Blackwell only the open modules; Maxwell, Pascal and Volta are "legacy GPUs no longer supported in the unified driver", kept on **580.xx** | 590.44.01 README, `kernel_open.html` and `supportedchips.html` on `download.nvidia.com` |
| In NVIDIA's `debian13` repository from 620, `nvidia-kernel-dkms` is only a transitional package that depends on `nvidia-kernel-open-dkms`, and `nvidia-driver` depends on the open modules | `Packages.gz` |
| **RTX 50 (Blackwell): branch 570 or newer** — the 570.86.16 README is the first of those checked to list the RTX 5090; 550.163.01's doesn't | `supportedchips.html` of 550.163.01, 570.86.16, 580.105.08, 590.44.01 |
| Debian 13 ships **`nvidia-driver` 550.163.01-2** (non-free; trixie-backports 550.163.01-4~bpo13+1), `nvidia-open-kernel-dkms` 550.163.01-2 (contrib), `nvidia-tesla-535-driver`; **no 470 or 390 legacy driver** | `apt-cache policy` on Debian 13, packages.debian.org |

So: NVIDIA's repository covers Turing to Blackwell; Debian's 550 covers Maxwell to Ada but **not RTX 50**; neither has a
driver for Kepler or older on Debian 13.

### A.2 What the card installs, by generation

The generation comes from the card's PCI device id, looked up in a data table next to the rules
(`assets/store/nvgen.json`, built from NVIDIA's `supportedchips.html` of the pinned branch, which lists each device id,
whether the open modules support it, and the 580 legacy list). It is unprivileged data like `suggestions.json`: a wrong
entry can only offer the wrong set, which still goes through the Store's plan, confirmation and `pkg-install`. NVIDIA's
`nvidia-driver-assistant` (its own picker) is not run.

| Card | Source | Driver set | CUDA option (off by default) |
|---|---|---|---|
| Turing (RTX 20, GTX 16) and newer, **including RTX 50** | NVIDIA's repository (`pkg-source-add nvidia-cuda`) | `nvidia-driver-pinning-<branch>`, `nvidia-driver`, `nvidia-kernel-open-dkms`, `linux-headers-amd64` | `nvidia-driver-cuda` (`libcuda1`, `nvidia-smi`); a second box for the toolkit, `cuda-toolkit-13-<n>` (compilers, for developers; size to verify) |
| Maxwell, Pascal, Volta (GTX 9xx, GTX 10, TITAN V), or any of these next to a newer card | Debian 13 `non-free` (what the card does today) | `nvidia-driver` 550, `nvidia-kernel-dkms` (proprietary), `firmware-misc-nonfree`, `linux-headers-amd64` | Debian's `libcuda1`, `nvidia-smi`; no toolkit offered (Debian's `nvidia-cuda-toolkit`: to verify) |
| Kepler and older | — | nothing: the card explains that Debian 13 has no driver for it and `nouveau` stays | — |
| Device id not in the table | — | nothing installable: the card says the model isn't known yet and links the compatibility report (Part 0) | — |

- Apps such as llama.cpp, Ollama, PyTorch wheels and Flatpak apps bring their own CUDA runtime and need only the
  driver's `libcuda`, which is why the toolkit is a separate box.
- Phase 1 is amd64 only (the rule's `arch` stays `x86_64`; NVIDIA's `sbsa` directory is for later).
- Removing: the card's **Remove** runs `pkg-remove` on the set; the last package of the repository's set makes
  `pkg-source-remove nvidia-cuda` run as for every vendor repository. `nouveau` is back after a restart.

### A.3 The repository entry (catalog) and the code it needs

A catalog entry in `apt/catalog.json`, key in `apt/keys/nvidia-cuda.asc`, read only by id, as for every vendor:

```json
"source": { "type": "apt", "delivery": "direct",
  "url": "https://developer.download.nvidia.com/compute/cuda/repos/debian13/x86_64", "suite": "/", "components": [],
  "arches": ["amd64"], "key": "keys/nvidia-cuda.asc", "fingerprint": "02182E60104FCDC26EAE1B8597A5D4CB8793F200",
  "package": "nvidia-driver",
  "files": { "list": "cuda-debian13-x86_64.list", "keyring": "cuda-archive-keyring.gpg", "ownedBy": "cuda-keyring" } }
```

- LordOS doesn't install `cuda-keyring` (no driver package depends on it). `files`/`ownedBy` is the Steam pattern for a
  person who installs it by hand from NVIDIA's guide: its conffile and keyring then take over and
  `pkg_sources_handover()` removes LordOS's bootstrap files, so apt never sees the repository twice with different
  `Signed-By`.
- **Code, no new privilege** (normal review, Claude Security, tests):
  1. **Flat repositories in the engine.** `add_source()` skips them today (`pkg.c`, `pkg.flatRepo`). It must fetch
     `<url>/InRelease` and `<url>/Packages.gz`, with `Filename` relative to `<url>`. With no `Suite`, `Codename` or
     `Valid-Until` in NVIDIA's `InRelease`, only its `Date` can refuse an older copy (to design: never older than the
     one stored). This is a new parse path in a root helper: `pkg` tests and a fuzz case for it.
  2. **`sources_admin.c`:** a catalog `suite` of `/` with no components writes `Suites: /` and no `Components:` (apt's
     flat form); the handover's check of the vendor's `deb` line accepts the flat form for that entry only.
  3. **The catalog entry is a driver, not an app:** the Store's AI and Commercial lists must not show it (a `kind` they
     skip).
  4. **`pin.c`** must read NVIDIA's pinning file (`/etc/apt/preferences.d/nvidia-driver-pin`: `Package:
     src:nvidia-graphics-drivers:any …`, `Pin: version 595*`, `Pin-Priority: 1000`); the `:any` suffix: to verify.
  5. **The rule format:** a rule may name a catalog `source`, generation-dependent `variants` and optional `extras`
     (CUDA, toolkit); the Store adds the source, runs `pkg-update`, then shows the plan, and removes the source again if
     the person cancels (as Store → Commercial does). `sdk/suggestengine.js` stays pure, with cases in
     `tests/suggest_engine_test.mjs`.

### A.4 Mixing NVIDIA's packages with Debian's

- **Same names, higher versions.** `nvidia-driver`, `nvidia-kernel-dkms`, `libcuda1`, `nvidia-smi`,
  `firmware-nvidia-gsp`, `nvidia-settings`… exist in both; NVIDIA's (590+) beat Debian's (550) by version and their
  exact `(= x)` dependencies keep one set from one origin. The open modules have **different names**: Debian's
  `nvidia-open-kernel-dkms` (contrib), NVIDIA's `nvidia-kernel-open-dkms`.
- **Conflicts when switching.** NVIDIA's `nvidia-kernel-support` provides and conflicts with Debian's split packages
  (`nvidia-alternative`, `nvidia-support`, `nvidia-kernel-common`, `nvidia-detect`, …). A computer that already has
  Debian's 550 from today's card must have those removed: the plan shows the removals (`pkg-install --allow-remove`); the engine's handling of it is
  to verify on the matrix.
- **Pinning is needed.** Without it every `pkg-upgrade` moves to NVIDIA's newest branch (620.70.06 today). The set
  starts with `nvidia-driver-pinning-<branch>`; which branch LordOS pins (a production branch NVIDIA supports for
  long) is decided per LordOS release and is **to verify**; moving the pin is a LordOS update (a migration).
- **Mesa and glvnd stay Debian's.** NVIDIA's repository has no `libglvnd0`, `libegl1`, `libgl1`, `libgbm1` or Mesa;
  `nvidia-driver-libs` depends on Debian's `libglx0` and `libopengl0`. Mesa stays for other GPUs and `nouveau`.
- **Xorg server.** `nvidia-driver` (NVIDIA's and Debian's) pulls `xserver-xorg-video-nvidia` → `xserver-xorg-core`,
  unused by LordOS. NVIDIA's compute-only set avoids it but has no GL or Vulkan for apps, so the card installs the
  full driver.
- **`contrib`.** NVIDIA's guide enables it; which dependency needs it on Debian 13 is to verify (the plan says when a
  component is missing; the LordOS ISO enables `main contrib non-free non-free-firmware`).
- **DKMS.** `nvidia-kernel-open-dkms` builds with Debian's `dkms` and `g++` at install and again at each kernel
  upgrade (DKMS's kernel hooks run inside `dpkg`, so under our engine as under apt: to confirm on the matrix);
  `linux-headers-amd64` keeps the headers in step with the kernel. A failed build leaves the card without NVIDIA's
  module; that `nouveau`'s blacklist and the initramfs are handled by NVIDIA's packages as by Debian's is to verify.

### A.5 Secure Boot (MOK enrolment, guided)

With Secure Boot on, an unsigned module doesn't load. Debian's DKMS (used by both sources) signs what it builds with its
own key (`/var/lib/dkms/mok.key`, `mok.pub`, made at the first build); the key must be **enrolled in shim's MOK list**,
which only the person can confirm at the next boot in MokManager's blue screen.

Flow: after the install, if `SecureBoot` is on and `mokutil --test-key /var/lib/dkms/mok.pub` says it isn't enrolled,
the Store card and Settings show *"One more step at the next restart"*, then `gpu-mok-enroll import`: root makes a
**one-time password** (8 digits, from `getrandom`), hashes it (`mokutil --generate-hash`), runs `mokutil --import
/var/lib/dkms/mok.pub --hash-file <root's 0600 temp file>` and returns the digits once. They are shown large with the
steps and a picture of MokManager: *Enroll MOK → Continue → Yes → type the password → Reboot*. After the restart the
check runs again with `/sys/module/nvidia`; if it failed, **Try Again** (a new import) or **Cancel** (`gpu-mok-enroll
cancel`: `mokutil --revoke-import`). The password never touches disk outside root's temp file (removed at once), the
log or the clipboard. Until this action is approved, the card's `secureBoot` note explains the manual step.

### A.6 Flatpak's GL.nvidia extension

Flatpak apps see NVIDIA's userspace only through `org.freedesktop.Platform.GL.nvidia-<version>` (and `GL32.nvidia-…`
for 32-bit, Steam), which must match the host driver exactly (`550.163.01` → `GL.nvidia-550-163-01`); otherwise they
fall back to software rendering. Design: after the card's install and after every `pkg-upgrade` that changes
`nvidia-driver-libs`, lord-server reads the installed version and, when Flatpak is installed, installs the matching
extension through the existing Flatpak path (`lord.flatpak`, Flathub's verified signature) and removes the previous one
once nothing uses it. No new admin action (per-user installation, or the existing Flatpak install action for the
system one). Settings → Flatpak shows it as part of the driver.

### A.7 Settings → Graphics cards

An NVIDIA card gets **Driver Options in the Store** (any user; the Store asks for an administrator as usual), which runs
`wm.open('store', { args: { page: 'suggested', card: 'nvidiaDriver' } })` (the `card` argument, opening that card
expanded, is new). Settings installs nothing itself. After the install it shows, from the compatibility report's facts
(Part 0, read-only): the driver and version, open or proprietary modules, *CUDA: Ready* when `libcuda.so.1`,
`nvidia-smi` and `/dev/nvidia-uvm` are there, and the Secure Boot step when it is pending. *AI and compute (CUDA)* is a
status read from those facts, so `gpu-role … ai` (Machines' action) doesn't change for NVIDIA. *Desktop* stays
*Planned* (Part B).

### A.8 How the card's notes change

| Note | Today | New |
|---|---|---|
| `nonfree` | non-free, from Debian | Says where it comes from: *NVIDIA's own repository* (Turing and newer) or *Debian's non-free archive* (older cards), and that it isn't free software |
| `nouveau` | the free driver is in use and stays the default | Unchanged; adds that **Remove** brings it back after a restart |
| `secureBoot` | the key must be enrolled at the next restart | Points to the guided step (A.5) once approved; until then the manual MokManager steps |
| `older` | cards older than about 2014 need the free driver | *GTX 9xx, GTX 10 and Volta get Debian's driver 550; Kepler and older keep `nouveau` (Debian 13 has no driver for them)* |
| new `rtx50` | — | RTX 50 needs NVIDIA's repository: Debian 13's 550 doesn't support it |
| new `branch` | — | Driver updates come from NVIDIA through the Store's updates, held to the branch LordOS tested |
| desktop | the LordOS desktop hasn't been tested with this driver | Unchanged (Part B) |

Every text through `t()`, in en, es and pt.

### A.9 Admin actions: what was dropped, what is still asked

| Proposed | Now | Why |
|---|---|---|
| `gpu-nvidia-sources "<on\|off>"` | **dropped** | NVIDIA's repository goes through `pkg-source-add nvidia-cuda` / `pkg-source-remove` (catalog, pinned fingerprint). Debian's `non-free`/`contrib` for older cards: the ISO already writes them, and on an `install.sh` system without them the card already explains instead of offering (`ifUnavailable: "explain"`) |
| `gpu-nvidia-install "<pci address>"` | **dropped** | The set is installed by the Store's existing `pkg-install` after the plan and confirmation. The flavour is data in the card: an administrator who could pick a wrong set can already install any package through the same action, so a root-side choice added nothing. The packages handle `nouveau`'s blacklist and the initramfs (to verify); `nvidia-drm.modeset=1` is only needed to draw the desktop and moves to Part B. The role isn't recorded (A.7) |
| `gpu-nvidia-remove` | **dropped** | `pkg-remove` on the set plus `pkg-source-remove` with the last package |
| `gpu-mok-enroll "<import\|cancel>"` | **still requested, awaiting the maintainer's approval** | No existing action can run `mokutil --import` with a one-time password made and hashed root-side. `import` only when Secure Boot is on, the key pair exists at DKMS's fixed path, root-owned, not a link, and isn't enrolled; fixed `/usr/bin/mokutil` arguments; returns the password once. `cancel`: `mokutil --revoke-import`. Administrators only; `mokutil` becomes a dependency of the `lordos` package |

## Part B — design: NVIDIA drawing the desktop — FOR REVIEW

Today an NVIDIA card shows the desktop with `nouveau` (Mesa) or, on NVIDIA's driver, with the screen composed on the
CPU (#171: the GPU start ends before a GL context; the report's `GPU start:` reason will tell which step per card).

### B.1 What lord-server needs from NVIDIA's driver

- **KMS:** `nvidia-drm.modeset=1` so `/dev/dri/cardN` does atomic modesetting; lord-server drives
  KMS itself (`kms.c`, no libdrm). `fbdev=1` keeps a console and a clean hand-off to the greeter (to verify per
  generation). NVIDIA's 590 README still calls DRM KMS "disabled by default"; whether its or Debian's packages turn it
  on is to verify. If they don't, a fixed `/etc/modprobe.d/lordos-nvidia.conf` (`options nvidia-drm modeset=1 fbdev=1`,
  then `update-initramfs -u`) is part of Part B's request, written only when the desktop role is chosen.
- **GBM:** `gpu.c` creates a GBM device on the card's fd. Mesa's `libgbm` loads NVIDIA's backend
  (`gbm/nvidia-drm_gbm.so`, package `libnvidia-egl-gbm1`): `nvidia-driver-libs` already depends on it in NVIDIA's repository (A.1).
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
  capability for `lord-sessiond`, and a closed action of its own since `gpu-nvidia-install` was dropped): when the
  desktop role is chosen, root adds a GRUB entry
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
| Driver from the Store card (A.2) | Debian 550, proprietary | NVIDIA's repository, open | NVIDIA's repository, open | NVIDIA's repository, open | NVIDIA's repository, open (needs 570+; Debian's 550 lacks it) |
| Desktop on `nouveau` (CPU / GPU composition) | | | | | |
| A: install from the Store card, DKMS build, `nvidia-smi`, CUDA (llama.cpp / Ollama) | | | | | |
| A: switch from Debian's 550 (today's card) to NVIDIA's repository | | | | | |
| A: Secure Boot on: MOK enrolment end to end | | | | | |
| A: kernel upgrade → DKMS rebuild; driver upgrade within the pinned branch → GL.nvidia extension | | | | | |
| A: remove → back on `nouveau`, NVIDIA's source gone | | | | | |
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

1. **`gpu-mok-enroll "<import|cancel>"`** (A.5, A.9), the only new closed action Part A still asks for.
2. The pinned branch of NVIDIA's driver (A.4) and the CUDA toolkit box (A.2), once verified.
3. The Flatpak GL.nvidia handling (A.6).
4. Part B, after the explicit-sync design: the desktop role, `nvidia-drm.modeset=1` if the packages don't set it, and
   the boot fallback (a GRUB entry and `lord-sessiond`'s counter).

Already decided (2026-11-22) and built after normal code review, with no new privilege: the `nvidia-cuda` catalog entry
and key, flat repositories in the engine and `sources_admin.c`, the rule's variants and options, and Settings' button
(A.3, A.7).
