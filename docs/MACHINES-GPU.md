# Machines: a smoother picture, and a real GPU for a machine (plan)

Status: **plan, approved in principle by the maintainer on 2026-11-21**. Nothing here is built yet. Each step that needs
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

- **Intel (Mode A):** the next step is our own GVT-g UEFI GOP option ROM, built from the open i915ovmfPkg source and
  signed with a LordOS key enrolled in each machine's Secure Boot db (Secure Boot stays on), so the vGPU's own display
  works and the picture reaches LordOS as a dmabuf. One mechanism for both uses: **exclusive** (the largest mdev type
  for one machine; a larger iGPU aperture in the firmware exposes larger types) and **shared** (smaller types, one per
  running machine — rare, but kept). The hybrid/render-only mode stays as the fallback.
- **AMD/NVIDIA (Mode B), first stage:** full passthrough with the card's own ROM and **its own monitor** — the best,
  near-native performance. Showing a dedicated card's picture inside LordOS (an IddCx virtual monitor + capture) is
  done only if people ask to use their usual monitors.

### Settings → Graphics cards (approved in principle 2026-11-21)

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
