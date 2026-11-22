# Machines: a smoother picture, and a real GPU for a machine (plan)

Status: **plan, approved in principle by the maintainer on 2026-11-21**. Part 1 steps 1, 2, 3 and 6 are built (in
development, 2026-11-21: "Part 1 — done" below), and so are Part 2's Mode A (Intel GVT-g) and Settings → Graphics cards
("Mode A — built" below); the rest is not built yet. Each step that needs
root or widens a sandbox goes through the maintainer's review and an AGENTS.md entry before it is built. Design and
status of Machines as a whole: [VIRTUALIZATION.md](VIRTUALIZATION.md).

The evidence comes from measurements on a Windows 11 test machine (viogpudo 100.100.104.27100, QEMU 10.0.13) and
from the maintainer's compositor logs, 2026-11-21.

## Part 1 — a smoother picture for every machine (no new privilege)

Windows produced 60 frames a second and lord-vmdisplay forwarded them at 60 a second, using about 4 % of one CPU. The
slowness is on the host side:

| # | Cause | Measured | Fix | Files | Effort |
|---|---|---|---|---|---|
| 1 | lord-server reads frames only when the Machines app polls (`setInterval(…, 16)`), and acknowledges each frame (`VD_FRAME_DONE`) at the poll after the next | 60 → **31** frames/s; 0–16 ms added before each frame | Read the vmdisplay socket from lord-server's main loop (`js_vm_pollfds()` / `js_vm_service()` beside `media_pollfds`); acknowledge a frame right after the paint that showed it. The JS poll keeps only the state | `server/src/ui/js_vm.c`, `server/src/main.c` | 1–2 days |
| 2 | Windows draws the pointer into its picture, so every pointer move is a round trip through the guest | 32 ms median, 45 ms p90, 93 ms max, before lord-server's paint | Hardware pointer: the Windows guest tools set viogpudo's `HWCursor=1` and restart the device; lord-server draws the guest's shape (`VD_CURSOR`) and stops `cursor_guest_draws` once one arrives. A per-machine switch goes back to the guest-drawn pointer (upstream ships it off by default) | `server/src/ui/js_vm.c`, `tools/guest-win/`, AGENTS.md wording for the guest tools | 1–2 days + testing |
| 3 | The whole picture is damaged for any change (a 24×19 pointer, a clock) | 2–4 ms of CPU blit, alpha pass and upload per wasted repaint | Damage only the changed rectangle in the normal and full-screen view, as the apps-mode crop already does; `engine-test`'s `test_damage_fuzz` and `test_damage_pipeline` keep passing | `server/src/media/media.c` | small |
| 4 | lord-vmdisplay merges every change into one box (a clock and a moving square become 612×492) | — | Send up to 4–8 rectangles per frame, merged when they overlap; lord-server already checks each with `vd_rect_inside` | `server/src/vm/vmdisplay.c` (no new syscalls) | small–medium |
| 5 | A picture that isn't 1:1 with its box (a fixed resolution, a resize in progress, HiDPI) goes through a per-pixel float bilinear loop | **41 ms** a frame at 1600×900 → 1080p | Short term: scale only the damaged rows, banded. Later: draw media sources as GPU textures like Wayland windows (damage-only upload, GPU scaling; the CPU path stays for `LORD_GPU=0`). This is also the step that shows a 3D machine's dmabuf without the GPU → CPU → GPU readback | `server/src/gpu/gpu.c`, `server/src/ui/paint.c`, `server/src/media/mview.c` | medium–large |
| 6 | JS stalls anywhere freeze every machine (lord-server is one thread): the Monitor's `monitor.processes` takes ~20 ms per call and caused 2286 of 7009 stall lines | `__lordTick` stalls average 73 ms | Make the process list asynchronous or cheaper (outside Machines) | `apps/monitor`, its native call | small–medium |
| 7 | Guest settings | — | The guest tools may suggest "Adjust for best performance" (animations and transparency off), keep "Follow the window" (1:1), and use virtio-tablet instead of usb-tablet | `tools/guest-win/`, `server/src/vm/lordvm.c` | small |

Not useful (checked or known): virtio-gpu `blob=true` with udmabuf (viogpudo has no blob resources), std-VGA/bochs
(Basic Display Adapter, slower), QXL-DOD (deprecated). viogpudo stays the best 2D driver for Windows.

**Order:** 1, 2, 3 first (about a week; most of the felt smoothness), then 4 and 6, then 5.

### Part 1 — done (2026-11-21, in development; the latest release is 0.8.9.4)

- **Step 1, frames in lord-server's loop.** Each machine's lord-vmdisplay socket (and a 3D frame's dmabuf while its
  fence is pending) is in the main `poll()` (`js_vm_pollfds` / `js_vm_dispatch`, `server/src/ui/js_vm.c`, `main.c`);
  `VD_FRAME_DONE` goes right after the render that painted the frame (`js_vm_painted`). `lord.vm.poll` only reports state.
- **Step 3, only the changed rectangle.** `media_src_damage_rect` maps the frame's rectangle into the MediaView through
  where its last paint put the picture (`map_*` in `MediaSrc`, fractions of the content box, one pixel wider for the
  filter; whole node when rotated, mirrored or of another size). A change outside the shown part counts as painted at
  once. `engine-test` (damage fuzz and pipeline) and `mview-test` pass.
- **Step 2, the hardware pointer.** The Windows tools (w5) say `pointer` in their hello; lord-vm then runs one fixed
  script as SYSTEM through qemu-guest-agent (`PTR_PS`) that sets `HWCursor` on each present `VioGpuDod` adapter's
  driver key and restarts only an adapter whose value changed (`pnputil /restart-device`). The tools run as the person
  and can't write that key, so the write is lord-vm's (AGENTS.md's guest-tools entry records it). lord-server draws the
  handed-over shape and drops `cursor_guest_draws` once one arrives. Configure → Display → *Pointer drawn by*: LordOS
  (faster, default) / The machine (machine.json schema 10 `"pointer": "guest"`, HWCursor 0, applied at the next start).
- **Step 6, Monitor.** `monitor.processes` no longer reads `/proc/<pid>/cgroup` on the screen's thread (the monitor's
  thread reads each process's scope once) and no longer parses JSON: the thread hands over rows (`MonProcs`) and the
  call builds the objects with pre-made atoms.

**Measured** (Windows 11 clone `win-tools`, viogpudo 100.100.104.27100, QEMU 10.0.13; a WinForms window moving a bar,
61 frames a second at the guest; picture 1400 × 1050 at 1:1; headless lord-server, CPU path, 60 Hz software vblank;
same method before and after, two runs each; pointer with `lordvm-dev lat`, 100 moves 200 ms apart; Monitor with the
perf driver's `monitorProcs` plan):

| | Before | After |
|---|---|---|
| Frames a second shown | 29.4 / 29.6 | **60.0 / 60.0** |
| Paint per frame | 1.76 / 1.65 ms | **0.11 / 0.14 ms** |
| lord-server CPU while it plays | 54 % of a core | **16–18 %** |
| Pointer move → visible | guest round trip 35–39 ms median, 45–47 p90, 75–97 max, then up to one app poll (16 ms) | **drawn by lord-server on the next frame** (≤ 16.7 ms at 60 Hz); moves make 0 frames, the guest's position answer in 1 ms |
| `monitor.processes`, 382 processes | 4.0 ms | **1.3 ms** |
| `monitor.processes`, 1,900 processes | 19.6 ms (a slow-call stall line every second) | **4.5 ms** (under the 20 ms line) |
| Stall lines during the picture runs | 0 | 0 |

Still open from step 6: the Processes tab's own React update (150 rows re-rendered each second) costs 50–120 ms of JS
per second here (`__lordTick` lines), independent of the native call; memoized or virtualized rows in the SDK's `Table`
would fix it. A real-display (KMS) screenshot with the handed-over pointer is still to do on the dev VM.

## Part 2 — a real GPU for a machine

Linux guests already have 3D through virgl. Windows has no stable virgl driver: the experimental viogpu3d (virtio-win
PR #943, a Mesa branch) gives OpenGL only, is test-signed (Secure Boot off) and unstable, so it is not offered. Triton
(D3D11 + Vulkan through Venus) targets Windows on ARM64 under UTM's forked QEMU and is watched for upstreaming.

A real GPU is the way. Two modes, both in the same "GPU for the machine" setting, which only lists GPUs that are not
showing this desktop:

### Mode A — shared (Intel GVT-g), the picture inside LordOS

For computers with an Intel iGPU of the generations GVT-g supports (Broadwell to Comet Lake) that the desktop doesn't
use, like the maintainer's (i9-10900, UHD 630, RX 580 as the main card, Z590).

- **What the guest gets:** Intel's own driver: D3D11, D3D12 (feature level 12_1), OpenGL 4.6 and Vulkan, computed by
  the real iGPU (about 85–95 % of it; the mediation costs a little). The picture comes as a dmabuf through
  `-display dbus,gl=on`, so it shows in the machine's window with apps mode, full screen, the clipboard and the rest.
- **Size:** the mdev type sets the share. A machine that is the only user gets the largest type (`i915-GVTg_V5_1` or
  the largest the host lists), close to the whole iGPU.
- **Caveats:** Intel archived GVT-g (it works, it won't improve). Windows needs an older Intel driver (community reports
  name 27.20.100.8681 and 30.0.101.1002; newer DCH installers fail or give code 43) and Windows Update must not
  replace it. Windows 11 needs UEFI, so OVMF with `ramfb=on` (and possibly a VBIOS ROM). A GVT-g machine can't be
  suspended or moved (Suspend becomes Shut Down, as for 3D today). A host i915/GVT bug can hang the iGPU.
- **What the person does:** enable the iGPU in the firmware while keeping the add-in card as the main display ("iGPU
  Multi-Monitor" / "Integrated Graphics" / "IGD"; primary display PEG/PCIe), and a large aperture/DVMT if offered.
- **What LordOS needs (each item for review):**
  - a root migration: `i915.enable_gvt=1` (and `enable_guc=0`) in modprobe.d plus initramfs, `kvmgt` in
    modules-load;
  - closed `admin.c` actions: create an mdev of one of the listed types for a machine's UUID
    (`/sys/bus/pci/devices/0000:00:02.0/mdev_supported_types/<type>/create`), give `/dev/vfio/<group>` to the session's
    person, remove the mdev when the machine stops; probably a higher memlock limit for QEMU;
  - lord-vm: `-device vfio-pci,sysfsdev=/sys/bus/mdev/devices/<uuid>,display=on,x-igd-opregion=on,ramfb=on,driver=vfio-pci-nohotplug,xres=…,yres=…`
    and `-display dbus,gl=on` on the Intel render node; Landlock and seccomp allow `/dev/vfio/*`, that sysfs path and
    the VFIO ioctls;
  - lord-vmdisplay: nothing new (it passes the dmabuf), but its fourcc/modifier checks accept Intel's tiled modifiers;
  - lord-server: Intel-tiled dmabufs can't be imported by the AMD card, so a second EGL context on the Intel render
    node imports and reads them back (as `db_read` does today), or Part 1's step 5 draws them as textures;
  - Machines: "Intel GPU (experimental)" under Display → Graphics, offered only when the mdev types exist, with a
    driver note for Windows.
- **First, a feasibility test by hand (about one day), outside LordOS:** iGPU on in the firmware → `enable_gvt=1` and
  `kvmgt` → create the largest mdev by hand → start a copy of the Windows test machine with QEMU by hand → install the
  older Intel driver → `dxdiag` shows D3D12, a day of use is stable, and the RX 580 stays the desktop's main card. Only
  if it passes are the admin actions and AGENTS.md entries proposed. Integration after that: about 2–3 weeks with
  testing.

### Mode B — dedicated (full passthrough of a second GPU)

For computers with a second AMD or NVIDIA card kept only for machines.

- **What the guest gets:** the card itself: native D3D12, OpenGL and Vulkan at full speed; the most stable option for
  games.
- **The picture** comes out of that card's own monitor outputs. Showing it inside LordOS needs a Looking-Glass-style
  shared-memory client (IVSHMEM + a guest capture program), a large piece of work kept for later.
- **What it needs:** the card in its own IOMMU group (on the maintainer's board the free bridges are 00:1b.0, alone,
  and 00:1c.0, which shares a group with 1c.4), `vfio-pci` bound at boot (root: kernel parameters or a modprobe rule),
  memlock equal to the guest's memory, and for some AMD cards a reset quirk (`vendor-reset`).
- A variant is passing the whole Intel iGPU (GVT-d, legacy IGD passthrough) with its picture on the motherboard's
  output; with OVMF it is fragile, so Mode A is preferred for an iGPU.

### Mode A — results of the hands-on test (2026-11-21, i9-10900 UHD 630, V5_4, Windows 11, driver 27.20.100.8681)

- **The vGPU works as a GPU:** Intel's driver loads with no code 43 (`PCI\VEN_8086&DEV_9BC5`), `dxdiag` shows D3D12
  feature level 12_1, a WebGL load ran 20 minutes at 25–46 % 3D use with no hang and no host i915/GVT error. The
  driver came from the Microsoft Update Catalog (Intel-signed kernel driver, Microsoft WHCP catalog). Windows Update
  installs 30.0.101.1273 by itself unless driver updates are blocked.
- **Its own display does not work with stock OVMF:** pipe and DDI come up but the primary plane never does, so QEMU
  gets no dmabuf. The known fix is a GVT option ROM (VBT/OpRegion for UEFI), today only as unsigned third-party
  binaries (Secure Boot off). Not used.
- **A guest reboot crashes the VM** (KVM emulation failure on a GVT-protected page, 3 of 3); a shutdown is clean.
  `-action reboot=shutdown` plus lord-vm starting the machine again works.
- **Hybrid / render-only works** with Secure Boot on and no option ROM: the machine keeps its `virtio-vga` (viogpudo)
  as the visible display and gets the mdev with `display=off`; apps that prefer the high-performance GPU
  (`HKCU\Software\Microsoft\DirectX\UserGpuPreferences`, `GpuPreference=2;`) render on the Intel GPU and are shown on
  viogpudo. WebGL aquarium, 1200×700: **17–20 fps on WARP → 41–54 fps on the UHD 630** (about 2.5×). Limits: an app
  that presents through DirectComposition (Chromium browsers by default) still goes through WARP unless started with
  `--disable-direct-composition`; dwm itself composes on WARP (viogpudo is display-only), which caps the whole desktop
  at about 50 fps at that size. The Intel adapter lights an invisible monitor; the guest tools must keep the desktop on
  viogpudo only (`DisplaySwitch /internal` or SetDisplayConfig). The host sees an i915 hotplug uevent each time that
  invisible monitor changes; lord-server must ignore hotplugs from a card that isn't driving the desktop.
- **Next step, the "virtual HDMI cable":** an Indirect Display (IddCx) virtual monitor rendered by the Intel adapter
  makes dwm and every app compose on the real GPU; the guest tools capture it (Desktop Duplication) and hand frames to
  the host through shared memory (IVSHMEM), like Looking Glass. That removes the WARP ceiling and works the same for a
  dedicated card with no monitor attached (Mode B). The IddCx driver must be signed (attestation-signed by Microsoft).
- Memlock grew to about 200 MB under 3D (47 MB idle); budget a few hundred MB.

### Decisions of 2026-11-21 (maintainer)

- **Intel (Mode A):** the next step is our own GVT-g UEFI GOP option ROM (now built: LordOS's own driver, below, as
  i915ovmfPkg has no license), signed with a LordOS key enrolled in each machine's Secure Boot db (Secure Boot stays
  on), so the vGPU's own display works and the picture reaches LordOS as a dmabuf. One mechanism for both uses:
  **exclusive** (the largest mdev type for one machine; a larger iGPU aperture in the firmware exposes larger types) and
  **shared** (smaller types, one per running machine — rare, but kept). The hybrid/render-only mode stays as the
  fallback.
- **AMD/NVIDIA (Mode B), first stage:** full passthrough with the card's own ROM and **its own monitor** — the best,
  near-native performance. Showing a dedicated card's picture inside LordOS (an IddCx virtual monitor + capture) is
  done only if people ask to use their usual monitors.

### Mode A — our own option ROM, hands-on (2026-11-21)

- **It works.** The open i915ovmfPkg GOP driver (patmagauran/i915ovmfPkg 2841e8e, built against edk2-stable202502
  with a small build patch for gcc 14) wrapped with `EfiRom`, given as `romfile=` to
  `vfio-pci-nohotplug,sysfsdev=…,display=on,x-igd-opregion=on,xres=1920,yres=1080`: the UEFI screen and the Windows
  desktop come out of the vGPU itself, Windows reads the EDID (1920×1080; a resize to 1280×800 follows at once), dwm
  and the apps compose on the Intel GPU (the only adapter), and QEMU hands lord-vmdisplay `ScanoutDMABUF` frames:
  `XR24`, stride 7680, modifier `I915_FORMAT_MOD_Y_TILED` (the ROM's own boot screen is linear `XR24`). EGL import on
  the Intel render node worked every frame.
- **WebGL aquarium (500 fish, 1200×700):** 17–20 fps on WARP → 41–54 hybrid → **75 fps held at the 74 Hz refresh**,
  **356–410 fps uncapped** (iGPU 800–1150 MHz). One first session stayed at 11–16 fps with the iGPU stuck at 350 MHz
  (i915 RPS up-threshold 95 %, the GPU at ~74 %): a minimum iGPU frequency while a GVT machine runs is worth a closed
  admin action.
- **Stability:** about 23 minutes at 96–100 % guest 3D, one guest restart (with `-action reboot=shutdown`, a clean exit
  and a fresh start), about 20 000 frames per run with 0 failures, no host i915/GVT/VFIO/KVM messages, two i915 hotplug
  uevents per machine start (no storm).
- **Secure Boot:** stayed on (`Confirm-SecureBootUEFI` → True). Debian's stock OVMF runs option ROMs without checking
  signatures (`PcdOptionRomImageVerificationPolicy` 0x00), so signing only matters with an OVMF built with policy 0x04:
  with it our signed ROM (sbsign + a test db certificate enrolled with `virt-fw-vars --add-db`) runs and an unsigned
  one is refused — and QEMU's unsigned iPXE ROMs would be refused too. Enrolling a db certificate on an existing
  machine changes PCR7 (BitLocker recovery), so it belongs at machine creation.
- **License:** i915ovmfPkg and the project it was forked from carry **no license** (the upstream is archived, 2023), so
  it can't ship. It was used only for this measurement; LordOS's own driver (next section) replaces it with the same
  results.
- **What LordOS needs besides Mode A's list above:** accept Y-tiled modifiers and cache dmabuf imports (GVT hands a
  new dmabuf on each flip), expect refused messages during resizes, QEMU's 30 ms display poll caps the picture at
  ~33 fps (raise it), ignore i915 hotplugs from a machine start, and choose the signing model (stock OVMF + the
  ROM's integrity from our signed package, or our own OVMF with policy 0x04 plus signed NIC ROMs).

### Our own GVT-g GOP driver (2026-11-21)

LordOS's own UEFI GOP option ROM for GVT-g vGPUs, `firmware/gvt-gop` (MIT), written from public, licensed sources
only: the MIT-licensed Linux i915 and GVT-g files, QEMU's `igd-assign.txt` and `fw_cfg.rst`, the UEFI specification and
EDK2, VESA EDID/CVT/DisplayPort (the list, file by file, is in `firmware/gvt-gop/NOTICE.md`). No i915ovmfPkg code was
read. Built by `tools/firmware/build-gvt-gop.sh` against `edk2-stable202502` (tarball hash and commit checked), X64
RELEASE and DEBUG, reproducible (the same bytes from two work folders). Not wired into lord-vm yet.

- **What it does:** binds only to a gen9 Intel display controller whose PVINFO page says GVT-g; copies the vGPU's
  OpRegion from fw_cfg `etc/igd-opregion` to ACPI NVS below 4 GB and writes ASLS (0xFC); reads the EDID over
  I2C-over-AUX on the DDI GVT-g wired the monitor to (D); lists the preferred detailed timing plus the EDID's other
  modes no larger than it and at least 1024×768 (QEMU's EDID gives 1920×1080, 1680×1050, 1440×900, 1280×1024,
  1280×960, 1024×768); maps a reserved-memory framebuffer into the GGTT at the start of the guest's aperture range;
  programs power wells, DPLL1 (HBR2), the DDI, DP link training, transcoder A and plane A (linear XRGB8888); installs
  GOP and EDID Discovered/Active. 2,197 lines with the build files (about 1,600 of C without blank and comment
  lines); the RELEASE driver is 13,952 bytes, the compressed ROM 8,704 bytes.
- **Results** (same computer, Windows image and driver as above, the old ROM measured again the same afternoon):

| | i915ovmfPkg (test only) | LordOS GVT-g GOP |
|---|---|---|
| License | none | MIT |
| UEFI screen | 1280×720, fixed | the EDID's preferred mode, 1920×1080; SetMode works for all 6 listed modes (each one a linear `XR24` dmabuf of the right size and stride) |
| Plane lit after QEMU starts | 0.81–0.84 s | 0.84–0.86 s |
| Time the ROM spends on the display | 46–49 ms | 38–40 ms |
| ROM size | 52,224 bytes | 8,704 bytes |
| Windows desktop on the vGPU | 1920×1080 at 74 Hz | 1920×1080 at 74 Hz; resizes to 1280×800 and back follow the EDID |
| WebGL aquarium, vsync | 75 fps | 75 fps |
| WebGL aquarium, uncapped | 333–410 fps | 314–432 fps (iGPU 767–1033 MHz) |
| Long run at 96–100 % guest 3D | ~23 min, ~20,000 frames per run, 0 failures | 21 min, 53,392 frames read by lord-vmdisplay, 0 failures, no host kernel message, no hotplug uevent |
| Guest restart, `-action reboot=shutdown` | clean exit | clean exit in ~8 s, the next start boots (75 fps) |
| Secure Boot | on; policy 0x04: signed runs, unsigned refused | on (`Confirm-SecureBootUEFI` True); with the policy-0x04 OVMF the signed ROM runs, the unsigned one is refused |

- **Same as with the old ROM:** dmabufs are `XR24` stride 7680 linear (UEFI) then `I915_FORMAT_MOD_Y_TILED` (Windows);
  two i915 `add` uevents on card0 per machine start; the only host kernel line is `gvt: vgpu 1: fail to find ppgtt
  instance` when Windows hibernates or resets (also seen in a run where the ROM was refused); the desktop stays on the
  RX 580.
- **Two behaviours to plan for in lord-vm** (the first comes with any GOP ROM that reserves a framebuffer, the second
  happens with either ROM):
  - The first boot after adding the vGPU to an existing NVRAM resets once: the framebuffer grows OVMF's reserved
    memory bin (`MemoryTypeInformation`, 0x401 → 0xA24 pages) and OVMF restarts; with `-action reboot=shutdown` that
    is an exit, so lord-vm starts the machine again. No reset with a fresh OVMF NVRAM or on later boots.
  - The Windows image hibernates on an ACPI power-off (QMP `SUSPEND_DISK`). A session resumed from hibernation ran
    the aquarium at 13–17 fps with the iGPU held at 350 MHz — with either ROM; cold boots and restarts ran at 75 fps.
    lord-vm should turn off hibernation/Fast Startup in the guest tools or shut down without it, and the minimum iGPU
    frequency action above still matters.

### Mode A — built (2026-11-21, in development; the latest release is 0.8.9.4)

Approved by the maintainer on 2026-11-21 with its four closed admin actions (AGENTS.md "Machines' graphics cards").

| Piece | What it does | Where |
|---|---|---|
| GPU list | Every PCI class 0x03 device: ids, driver, IOMMU group, `boot_vga`, card and render node, GVT-g family (Broadwell, Skylake, Kaby/Amber Lake, Coffee/Whiskey Lake, Comet Lake, Broxton by device id), GVT on (it lists `mdev_supported_types`), each type's sizes, largest resolution, weight and free instances. The desktop's GPU is the card lord-server holds open (root reads it from lord-server's descriptors); with none held, the firmware's primary display | `server/src/vm/vmgpu.c`, `lord.vm.gpus()` |
| `gpu-role` | Machines (shared): writes `/etc/modprobe.d/lordos-gpu.conf` and `/etc/modules-load.d/lordos-gpu.conf` (fixed text), adopts the hand-written `i915-gvt.conf` and `kvmgt.conf` of the feasibility test when they say exactly the same (removed, so no option is said twice), runs `update-initramfs -u`. Unused removes them. Dedicated and AI answer `gpu.roleNotYet` | `server/src/vm/vmgpu_admin.c` |
| `vm-gvt-create` / `vm-gvt-remove` | The mdev named after the machine's UUID, of a listed type with an instance free; root notes the caller in `/run/lordos/vm-gvt/<id>`; `/dev/vfio/<group>` to the caller 0600. Removal: only the caller's, never while a process holds the group. The helper sweeps the caller's leftovers when it starts and ends | same |
| `gpu-min-freq` | `rps_min_freq_mhz` within RPn..RP0 while the caller has a vGPU; the first value is kept in `/run/lordos/vm-gvt/minfreq-<address>` and comes back with "default" once no vGPU is left. lord-server asks for two thirds of RP0 (800 MHz on the UHD 630) while a vGPU machine runs | same, `js_vm.c` (`gvt_freq`) |
| Settings → Graphics cards | Shown with more than one GPU. The desktop's card fixed; Unused / Machines (shared) for a GVT-g iGPU; a suggestion when such an iGPU is unused; "Machines: dedicated — coming in a later version" as a note for other cards; restart notice | `apps/settings/Gpus.jsx` |
| Configure → Display → Graphics card | None / Whole (the largest type) / Shared (the smallest type that shows the machine's resolution; 1920×1080 when it follows the window), free instances shown, a choice with none free disabled; a card replaces 3D graphics. machine.json schema 11 `"gpu"` | `apps/machines/configure.jsx`, `model.js` (`gvtType`) |
| Start | Machines asks `vm-gvt-create` before the start (`startPrep`, with the bridged network's `vm-net-bridge`) | `apps/machines/ui.jsx` |
| lord-vm | `vfio-pci-nohotplug,id=video0,sysfsdev=…,display=on,x-igd-opregion=on,xres,yres,romfile=/usr/lib/lordos/firmware/lordos-gvt-gop.rom,addr=0x4` as the only display; `-display dbus,gl=on,rendernode=<the iGPU's>`; `-action reboot=shutdown`, and every reset (the guest's, Restart's) ends with reason `reset`, which the machine's window starts again (the first boot's OVMF reset included); no suspend, no snapshot with memory. QEMU's soft and hard memlock: the machine's memory + 512 MB, within the session's hard limit, which lord-server raises to the computer's memory before it drops root, only when an iGPU runs GVT-g | `server/src/vm/lordvm.c` (`decide_vgpu`), `server/src/main.c` |
| QEMU's sandbox | Landlock adds `/dev/vfio/vfio` and that vGPU's `/dev/vfio/<group>` (read, write, ioctl), the mdev's real sysfs folder (read) and the ROM (read; under `/usr` in the package). Our seccomp is a denylist that already allows the VFIO ioctls. A vGPU machine doesn't start unless those Landlock rules applied | `server/src/vm/vmsandbox.c` |
| lord-server's picture | Intel-tiled dmabufs are imported on the iGPU's render node in a second EGL context (`dmaread.c`, surfaceless GLES2, its own GBM device) and read back as BGRX, one import per buffer kept; it works with the desktop on another GPU or on the CPU path. drm hotplug uevents from another card than the desktop's are ignored | `server/src/gpu/dmaread.c`, `js_vm.c`, `server/src/hw/uevent.c` |
| Guest tools w6 | With a vGPU, one fixed SYSTEM script (`GVTWIN_PS`): hibernation and Fast Startup off; Intel's 27.20.100.8681 from the vendor catalog (fixed URL, SHA-256 `b5db0c71…21a0`, INF version, the catalog's WHCP signature and `igdkmd64.sys`'s Intel signature checked) only when the device lacks it; then a device installation restriction (DenyDeviceIDs, not retroactive) for that one device id | `server/src/vm/vmgvtwin.c` |
| Option ROM | The reproducible build committed in `firmware/gvt-gop/prebuilt` (SHA-256 `aca1ff08…0759`), checked and installed by `mkdeb.sh`: building EDK2 in each package build was too heavy for an 8.7 KB file that doesn't change | `tools/iso/mkdeb.sh` |

**Checked live** (the maintainer's computer: i9-10900 UHD 630 with GVT-g on, RX 580 as the desktop's card; a fresh copy
of the Windows 11 test machine, `win-gvt`; QEMU 10.0.13; `lordvm-dev` in lord-server's place, the test build of lord-vm
with the committed ROM):

- `vm-gvt-create` (run as root through `build/vm-gvt-cli`, the action's own code): `i915-GVTg_V5_4` made with the
  machine's UUID, `/dev/vfio/16` given to the user 0600; `vm-gvt-remove` refused while QEMU held the group
  (`vm.gvtInUse`) and removed it after the shutdown; `gpu-min-freq 800` set 800 MHz, 5000 was refused (`gpu.freqRange`),
  "default" kept 800 while the vGPU was there and put back 350 once it was gone.
- First start: Landlock, our seccomp and no_new_privs on with `vgpu=1`; the ROM refused at first (outside Landlock's
  paths in the test build: a read rule for the ROM was added); then the first boot's OVMF reset ended QEMU with reason
  `reset`, as planned; the next start booted to the desktop at 1920×1080 on the vGPU, 0 dmabuf failures.
- The guest tools w6 then ran the driver script: "27.20.100.8681 installed (PCI\VEN_8086&DEV_9BC5) … hibernation and
  Fast Startup turned off"; the block failed on the first run because PowerShell's `$k` and `$K` are one variable (fixed;
  `vm-test` now refuses two names that differ only in case). Restart from the host ended QEMU with
  `host-qmp-system-reset` (now also a restart); the next start: "already 27.20.100.8681 … its updates are blocked now;
  hibernation and Fast Startup stay off". The machine's own shutdown was clean.
- `vmgpu-test` (the four actions under a fake root), `dmaread-test` (a Y-tiled XRGB8888 buffer made with GBM on the
  Intel node read back exactly, with the other EGL context current on the AMD node), `uevent-test`.

**Not checked yet:** the whole path inside a running lord-server (Machines' window, Settings → Graphics cards) with a
screenshot on the dev VM's real display; `gpu-role` for real (it writes `/etc` and runs `update-initramfs`; only under
the fake root); a guest restart from inside Windows with this build (the QMP reset and the first-boot reset were); the
WebGL numbers with this build (the earlier hands-on numbers stand); other gen9 iGPUs than 0x9BC5; a restart when the
machine has no window open (only the machine's window starts it again).

### Settings → Graphics cards (approved in principle 2026-11-21; built, "Mode A — built" above)

The person never runs a command: a computer with more than one GPU is managed from **Settings → Graphics cards**.

- Every GPU is listed with what uses it. The one driving the desktop is fixed and never offered for anything else.
- A GPU nobody uses is suggested: "You have a graphics card that isn't used. Use it for Machines or for AI?"
- Roles for a spare GPU: **Unused** · **Machines: shared** (Mode A, Intel GVT-g where the hardware allows it) ·
  **Machines: dedicated** (Mode B, the whole card for one machine) · **AI** (local models use this card instead of the
  desktop's; second stage: which stack each vendor needs — Vulkan, ROCm, CUDA — and which LordOS AI features use it).
  A role the hardware can't take is shown disabled with the reason. Each role says what it gains and what it costs
  (for example: a machine with a GPU can't be suspended).
- Changing a role needs a restart, said plainly; "Unused" always undoes it.
- Underneath: closed `admin.c` actions (for review and an AGENTS.md entry before they're built), e.g.
  `gpu-role <pci address> <unused|machines-shared|machines-dedicated|ai>`. Root validates the address against the GPUs
  it detects itself, refuses the desktop's GPU, writes only fixed files (`/etc/modprobe.d/lordos-gpu.conf`,
  `/etc/modules-load.d/lordos-gpu.conf`, the vfio-pci binding for a dedicated card) and runs `update-initramfs`; no
  free-form options. The mdev create/remove and `/dev/vfio` access for a running machine are separate closed actions
  (Mode A above).
- Order: Mode A's feasibility test by hand on the maintainer's computer first (the three fixed lines this action will
  write, run once by the maintainer), then this Settings page, which takes over that configuration.

**Order:** Part 1 steps 1–3 → Mode A's feasibility test → Settings → Graphics cards and Mode A's review and integration → Part 1 steps 4–6 →
Mode B later.

## Sources

- virtio-win PR #990 (viogpudo HWCursor, closed), PR #943 (viogpu3d); virglrenderer MR 1185; Triton v0.6.
- Intel GVT support table; intel/gvt-linux "GVTg_Setup_Guide"; Arch wiki "Intel GVT-g"; NixOS wiki "IGVT-g"; Proxmox
  forum on GVT-g Windows drivers; Linux v6.12 `drivers/gpu/drm/i915/intel_gvt.c` (`is_supported_device` accepts
  Comet Lake).
- For LordOS's own GVT-g GOP driver: `firmware/gvt-gop/NOTICE.md` (Linux v6.12.57 i915/GVT-g files, MIT; QEMU
  `docs/igd-assign.txt` and `docs/specs/fw_cfg.rst`; UEFI 2.10; EDK2 edk2-stable202502; VESA E-EDID 1.4, CVT 1.2, DMT,
  DisplayPort 1.2).
