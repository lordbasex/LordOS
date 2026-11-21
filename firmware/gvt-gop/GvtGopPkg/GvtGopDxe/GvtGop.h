/** @file
  LordOS GVT-g GOP driver: shared declarations.

  Copyright (c) 2026 iPERFEX and the LordOS contributors
  SPDX-License-Identifier: MIT
**/

#ifndef GVT_GOP_H_
#define GVT_GOP_H_

#include <Uefi.h>
#include <IndustryStandard/Pci.h>
#include <Protocol/PciIo.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/EdidDiscovered.h>
#include <Protocol/EdidActive.h>
#include <Protocol/DevicePath.h>
#include <Protocol/DriverBinding.h>
#include <Protocol/ComponentName2.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/FrameBufferBltLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#include "GvtRegs.h"

#define GVT_MAX_MODES        16
#define GVT_EDID_SIZE        128
#define GVT_MAX_WIDTH        4096
#define GVT_MAX_HEIGHT       2304
#define GVT_MIN_WIDTH        1024
#define GVT_MIN_HEIGHT       768

//
// One display mode. Timings are in pixels/lines (not minus one), the clock in kHz.
//
typedef struct {
  UINT32    HActive;
  UINT32    HSyncStart;
  UINT32    HSyncEnd;
  UINT32    HTotal;
  UINT32    VActive;
  UINT32    VSyncStart;
  UINT32    VSyncEnd;
  UINT32    VTotal;
  UINT32    ClockKhz;
  BOOLEAN   HSyncPositive;
  BOOLEAN   VSyncPositive;
} GVT_MODE;

#define GVT_GOP_SIGNATURE  SIGNATURE_32 ('L', 'G', 'V', 'T')

typedef struct {
  UINT32                                  Signature;
  EFI_HANDLE                              Controller;
  EFI_HANDLE                              Handle;          // child with GOP
  EFI_PCI_IO_PROTOCOL                     *PciIo;
  UINT64                                  OriginalAttributes;
  UINT64                                  MmioSize;        // size of BAR 0
  EFI_DEVICE_PATH_PROTOCOL                *DevicePath;     // child path

  //
  // The vGPU: which DDI GVT-g wired the virtual monitor to and the
  // graphics memory ranges PVINFO lets this guest use.
  //
  UINT32                                  Port;
  UINT32                                  GmBase;          // FB address in GGTT space
  UINT32                                  GmSize;

  //
  // Framebuffer: reserved guest RAM, mapped linearly into the GGTT at GmBase.
  //
  EFI_PHYSICAL_ADDRESS                    FbBase;
  UINTN                                   FbPages;

  UINT8                                   Edid[GVT_EDID_SIZE];
  BOOLEAN                                 EdidValid;
  GVT_MODE                                Modes[GVT_MAX_MODES];
  UINT32                                  ModeCount;

  EFI_GRAPHICS_OUTPUT_PROTOCOL            Gop;
  EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE       GopMode;
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION    GopInfo;
  FRAME_BUFFER_CONFIGURE                  *BltConfig;
  UINTN                                   BltConfigSize;

  EFI_EDID_DISCOVERED_PROTOCOL            EdidDiscovered;
  EFI_EDID_ACTIVE_PROTOCOL                EdidActive;
} GVT_DEVICE;

#define GVT_DEVICE_FROM_GOP(a)  CR (a, GVT_DEVICE, Gop, GVT_GOP_SIGNATURE)

//
// Display.c
//
UINT32
GvtRead (
  IN GVT_DEVICE  *Dev,
  IN UINT32      Offset
  );

VOID
GvtWrite (
  IN GVT_DEVICE  *Dev,
  IN UINT32      Offset,
  IN UINT32      Value
  );

EFI_STATUS
GvtProbe (
  IN GVT_DEVICE  *Dev
  );

EFI_STATUS
GvtMapFramebuffer (
  IN GVT_DEVICE  *Dev
  );

VOID
GvtUnmapFramebuffer (
  IN GVT_DEVICE  *Dev
  );

EFI_STATUS
GvtAuxTransfer (
  IN  GVT_DEVICE   *Dev,
  IN  CONST UINT8  *Send,
  IN  UINT32       SendSize,
  OUT UINT8        *Reply,
  IN  UINT32       ReplyMax,
  OUT UINT32       *ReplySize
  );

EFI_STATUS
GvtSetMode (
  IN GVT_DEVICE      *Dev,
  IN CONST GVT_MODE  *Mode
  );

VOID
GvtDisableDisplay (
  IN GVT_DEVICE  *Dev
  );

UINT32
GvtStride (
  IN UINT32  Width
  );

//
// Edid.c
//
EFI_STATUS
GvtReadEdid (
  IN GVT_DEVICE  *Dev
  );

VOID
GvtBuildModeList (
  IN GVT_DEVICE  *Dev
  );

//
// OpRegion.c
//
VOID
GvtInstallOpRegion (
  IN GVT_DEVICE  *Dev
  );

//
// Gop.c
//
EFI_STATUS
GvtGopInit (
  IN GVT_DEVICE  *Dev
  );

VOID
GvtGopFini (
  IN GVT_DEVICE  *Dev
  );

#endif
