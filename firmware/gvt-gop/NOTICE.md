# NOTICE: sources and provenance

The LordOS GVT-g GOP driver (`firmware/gvt-gop`) was written for LordOS in November 2026 and is licensed under
the MIT license in the repository's `LICENSE` ("Copyright (c) 2026 iPERFEX and the LordOS contributors").

## Clean-room rule

It was written without looking at i915ovmfPkg or any of its forks (patmagauran/i915ovmfPkg, i915-ovmf and
others). Those projects carry no license, so their code cannot be used or adapted. The earlier LordOS test that
ran an i915ovmfPkg build contributed only its measurements (what worked, which QEMU options it needed, frame
rates), never code.

## What was consulted

### Linux kernel, `drivers/gpu/drm/i915` (MIT license), tag v6.12.57

Each file below carries the MIT permission notice (or `SPDX-License-Identifier: MIT`) in its header. They were
read to learn the register layout of the gen9 display engine and how the GVT-g mediator emulates it for a vGPU.
No code was copied; register offsets and bit positions are facts of the hardware interface.

| File | Used for |
|---|---|
| `i915_pvinfo.h` | PVINFO page layout, `VGT_MAGIC`, ballooning ranges |
| `i915_reg.h` | transcoder timings, PIPESRC, M/N, DDI, DP transport, DPLL_CTRL1/2, LCPLL, power wells, VGA control |
| `display/intel_dp_aux_regs.h` | DP AUX control and data registers |
| `display/skl_universal_plane_regs.h` | universal plane registers and PLANE_CTL fields |
| `gt/intel_gt_regs.h`, `gt/intel_ggtt.c`, `gt/intel_gtt.h` | GGTT location in BAR 0, PTE format, TLB flush |
| `display/intel_vga.c` | disabling the VGA plane |
| `gvt/display.c` | the virtual monitor's port (D on gen9), its reset-time state, vblank emulation |
| `gvt/handlers.c` | which display registers GVT-g emulates and how (pipe enable, DDI buffer, DP transport, DPLL status, power wells, AUX channel, PVINFO reads) |
| `gvt/edid.c`, `gvt/edid.h` | the EDID path GVT-g emulates: I2C-over-AUX for a DP monitor, one byte per read, 128 bytes |
| `gvt/fb_decoder.c`, `gvt/dmabuf.c` | what GVT-g needs to export the primary plane as a dmabuf (enabled pipe and plane, format, stride, PIPESRC, GGTT range checks) |
| `gvt/gtt.c`, `gvt/kvmgt.c`, `gvt/gvt.h`, `gvt/mmio.c`, `gvt/aperture_gm.c`, `gvt/vgpu.c` | GGTT write emulation, valid graphics memory ranges, vGPU types |
| `gvt/opregion.c`, `gvt/cfg_space.c` | the vGPU's virtual OpRegion |
| `Documentation/gpu/i915.rst` | GVT-g overview |

Source: `https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git`, tag `v6.12.57`.

### QEMU documentation

- `docs/igd-assign.txt` (QEMU v10.0.0): the firmware contract for `etc/igd-opregion` (reserved memory below
  4 GB, address written to ASLS at PCI config 0xFC) and `x-igd-opregion=on`.
- `docs/specs/fw_cfg.rst` (QEMU v10.0.0): the fw_cfg I/O ports (0x510 selector, 0x511 data), the signature
  item and the big-endian file directory.

Only the interfaces were taken from these documents; no QEMU code was read for this driver.

### UEFI and EDK2 (BSD-2-Clause-Patent)

- UEFI Specification 2.10: driver model (chapter 11), Graphics Output Protocol and EDID protocols (section 12.9),
  ACPI _ADR device path nodes.
- EDK2 `edk2-stable202502` headers and libraries the driver is built against and links: MdePkg (BaseLib,
  BaseMemoryLib, IoLib, UefiLib, DevicePathLib, CacheMaintenanceLib, entry point and table libraries) and
  MdeModulePkg `FrameBufferBltLib` (the Blt implementation). The DEBUG build also links OvmfPkg
  `PlatformDebugLibIoPort`. These are BSD-2-Clause-Patent; their notices travel with EDK2.
- MdeModulePkg `GraphicsConsoleDxe` and `UefiBootManagerLib` were read to understand how OVMF chooses a console
  mode and when it resets after memory-type changes.

### Standards

- VESA Enhanced EDID 1.4 (base block layout, detailed/standard/established timings).
- VESA Coordinated Video Timings 1.2 (reduced blanking, version 1) and VESA DMT (1024x768 at 60 Hz).
- VESA DisplayPort 1.2 (AUX request codes, DPCD link configuration and training registers).
- Intel OpRegion specification (the `IntelGraphicsMem` signature and the ASLS register).

## Binaries

Release images are built by `tools/firmware/build-gvt-gop.sh` from this folder and EDK2 `edk2-stable202502`
(commit `fbe0805b2091393406952e84724188f8c1941837`). An image contains this driver plus the EDK2 library code
named above (BSD-2-Clause-Patent).
