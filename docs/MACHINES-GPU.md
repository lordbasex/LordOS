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

**Order:** Part 1 steps 1–3 → Mode A's feasibility test → Mode A's review and integration → Part 1 steps 4–6 →
Mode B later.

## Sources

- virtio-win PR #990 (viogpudo HWCursor, closed), PR #943 (viogpu3d); virglrenderer MR 1185; Triton v0.6.
- Intel GVT support table; intel/gvt-linux "GVTg_Setup_Guide"; Arch wiki "Intel GVT-g"; NixOS wiki "IGVT-g"; Proxmox
  forum on GVT-g Windows drivers; Linux v6.12 `drivers/gpu/drm/i915/intel_gvt.c` (`is_supported_device` accepts
  Comet Lake).
