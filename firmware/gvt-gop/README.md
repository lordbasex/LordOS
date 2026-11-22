# LordOS GVT-g GOP option ROM

A UEFI Graphics Output Protocol driver for Intel GVT-g virtual GPUs, shipped as a PCI option ROM. With it, a
machine that gets a GVT-g vGPU (Machines, Mode A in `docs/MACHINES-GPU.md`) shows its UEFI screen and its OS on
the vGPU's own display, and QEMU hands that display to LordOS as dmabufs. Written for LordOS from public
documentation; MIT, like the rest of LordOS. `NOTICE.md` lists every source consulted.

Status: tested with a Comet Lake GT2 vGPU (8086:9BC5, mdev type `i915-GVTg_V5_4`), QEMU 10.0, Debian's OVMF
2025.02 and Windows 11 with Intel's driver 27.20.100.8681. Machines gives it to QEMU for a machine with an Intel graphics card (lord-vm, in development).

## What it does

1. Binds to an Intel display controller of the Skylake family (gen9: Skylake, Kaby Lake, Amber Lake, Coffee
   Lake, Comet Lake) and checks the GVT-g PVINFO page at MMIO 0x78000 (`vGTvGTvG` magic, version 1). Anything
   else, including a physical Intel GPU, is left alone.
2. Reads the graphics memory ranges PVINFO gives this guest (the ballooning ranges) and the DDI GVT-g wired its
   virtual monitor to (port D on gen9).
3. Copies the vGPU's OpRegion (QEMU's fw_cfg file `etc/igd-opregion`, present with `x-igd-opregion=on`) into ACPI
   NVS memory below 4 GB and writes its address to ASLS (PCI config 0xFC), so the OS driver finds the VBT.
4. Reads the virtual monitor's EDID over I2C-over-AUX and builds the mode list: the preferred detailed timing
   first, then the other detailed, standard and established timings no larger than it and at least 1024x768
   (CVT reduced blanking for the ones without timings). No EDID: 1024x768 (VESA DMT).
5. Allocates the framebuffer for the largest mode in reserved memory and maps it linearly into the global GTT at
   the start of the guest's aperture range.
6. Sets the preferred mode: power wells, DPLL1 at HBR2, DDI clock, DP transport and DDI buffer, link training
   over AUX, transcoder A timings and M/N, transcoder enable, then primary plane A as linear XRGB8888.
7. Installs GOP (QueryMode, SetMode for every listed mode, Blt over the linear framebuffer) and EDID
   Discovered/Active on a child handle.

The OS driver later takes the display over and reprograms everything (Windows' Intel driver switches to a
Y-tiled plane by itself).

Not done, on purpose: watermarks and DDB allocation (GVT-g does not model them; the OS driver programs them),
panel fitting, a VGA BIOS (UEFI only), and other platforms (Broadwell and Broxton use other PLLs and ports).

## Build

```sh
tools/firmware/build-gvt-gop.sh                 # out: firmware/gvt-gop/out/
tools/firmware/build-gvt-gop.sh --device-id 0x9BC5 --work ~/.cache/lordos-firmware --out /tmp/gvt-gop
```

The script downloads `edk2-stable202502` from GitHub, checks the tarball's SHA-256 and the commit embedded in it
(`fbe0805b2091393406952e84724188f8c1941837`), builds only the BaseTools it needs (GenFw, EfiRom), builds the driver
X64 RELEASE and DEBUG with GCC (`GCC5` tool chain tag, `-Werror`), zeroes the debug directory and time stamps
(`GenFw -z`) and wraps each image with `EfiRom -f 0x8086 -i <device id> -l 0x030000 -ec`. The outputs and their
SHA-256 are in `out/SHA256SUMS`; two builds in different work folders give the same bytes.

Packages it needs (Debian 13): `build-essential nasm uuid-dev python3 curl git`. The DEBUG image prints to
QEMU's debug console: `-debugcon file:debug.log -global isa-debugcon.iobase=0x402`.

## Prebuilt

`prebuilt/lordos-gvt-gop.rom` is the RELEASE ROM this script builds from this folder (device ID 0x9BC5), committed with
its SHA-256 in `prebuilt/SHA256SUMS` (`aca1ff08…0759`). The package (`tools/iso/mkdeb.sh`) checks that hash and installs
it as `/usr/lib/lordos/firmware/lordos-gvt-gop.rom`, the fixed path lord-vm passes as `romfile=`: building EDK2 in every
package build would need a download and several minutes for an 8.7 KB file that never changes between builds. Anyone
can check it: `tools/firmware/build-gvt-gop.sh --out /tmp/g && cmp /tmp/g/lordos-gvt-gop-release.rom
firmware/gvt-gop/prebuilt/lordos-gvt-gop.rom` (two builds in different work folders gave these same bytes, 2026-11-21).
After a change to the driver, rebuild and replace both files in the same commit.

The ROM's PCI header names device 0x9BC5 (Comet Lake GT2); OVMF loads a `romfile=` image without comparing the device ID,
and the driver itself binds by the PVINFO page and the gen9 display, so other gen9 iGPUs take the same file (only 0x9BC5
is tested).

## Use with QEMU

```sh
-device vfio-pci-nohotplug,sysfsdev=/sys/bus/mdev/devices/<uuid>,display=on,x-igd-opregion=on,\
xres=1920,yres=1080,romfile=/path/lordos-gvt-gop-release.rom
```

The first boot after the vGPU is added to a machine with an existing NVRAM resets once: the framebuffer grows
OVMF's reserved memory bin (`MemoryTypeInformation`), and OVMF restarts to apply it. With `-action reboot=shutdown`
that restart is an exit, so the caller starts the machine again; later boots, and a fresh NVRAM, do not reset.

## Secure Boot

Debian's OVMF runs option ROMs without checking their signature (`PcdOptionRomImageVerificationPolicy` 0x00).
An OVMF built with policy 0x04 runs only signed ones: sign `GvtGopDxe-release.efi` with a key whose certificate
is in the machine's `db`, then wrap it again:

```sh
sbsign --key db.key --cert db.crt --output GvtGopDxe-signed.efi out/GvtGopDxe-release.efi
EfiRom -f 0x8086 -i 0x9BC5 -l 0x030000 -ec GvtGopDxe-signed.efi -o lordos-gvt-gop-signed.rom
```

## Layout

| File | What it holds |
|---|---|
| `GvtGopPkg/GvtGopDxe/Driver.c` | driver binding, child handle, component name |
| `GvtGopPkg/GvtGopDxe/Display.c` | MMIO, PVINFO probe, GGTT framebuffer, DP AUX, mode set |
| `GvtGopPkg/GvtGopDxe/Edid.c` | EDID over I2C-over-AUX, mode list, CVT timings |
| `GvtGopPkg/GvtGopDxe/OpRegion.c` | fw_cfg reader, OpRegion copy, ASLS |
| `GvtGopPkg/GvtGopDxe/Gop.c` | the Graphics Output Protocol |
| `GvtGopPkg/GvtGopDxe/GvtRegs.h` | register offsets and fields |
