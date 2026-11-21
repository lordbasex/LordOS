/** @file
  LordOS GVT-g GOP driver: EFI_GRAPHICS_OUTPUT_PROTOCOL over the linear
  framebuffer (UEFI 2.10, section 12.9).

  Copyright (c) 2026 iPERFEX and the LordOS contributors
  SPDX-License-Identifier: MIT
**/

#include "GvtGop.h"

STATIC
VOID
GvtFillInfo (
  IN  CONST GVT_MODE                        *Mode,
  OUT EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  *Info
  )
{
  ZeroMem (Info, sizeof (*Info));
  Info->Version              = 0;
  Info->HorizontalResolution = Mode->HActive;
  Info->VerticalResolution   = Mode->VActive;
  Info->PixelFormat          = PixelBlueGreenRedReserved8BitPerColor;
  Info->PixelsPerScanLine    = GvtStride (Mode->HActive) / 4;
}

STATIC
EFI_STATUS
EFIAPI
GvtGopQueryMode (
  IN  EFI_GRAPHICS_OUTPUT_PROTOCOL          *This,
  IN  UINT32                                ModeNumber,
  OUT UINTN                                 *SizeOfInfo,
  OUT EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  **Info
  )
{
  GVT_DEVICE  *Dev;

  if ((This == NULL) || (SizeOfInfo == NULL) || (Info == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Dev = GVT_DEVICE_FROM_GOP (This);
  if (ModeNumber >= Dev->ModeCount) {
    return EFI_INVALID_PARAMETER;
  }

  *Info = AllocatePool (sizeof (EFI_GRAPHICS_OUTPUT_MODE_INFORMATION));
  if (*Info == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  GvtFillInfo (&Dev->Modes[ModeNumber], *Info);
  *SizeOfInfo = sizeof (EFI_GRAPHICS_OUTPUT_MODE_INFORMATION);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
GvtGopSetMode (
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL  *This,
  IN UINT32                        ModeNumber
  )
{
  GVT_DEVICE                     *Dev;
  EFI_STATUS                     Status;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Black;

  if (This == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Dev = GVT_DEVICE_FROM_GOP (This);
  if (ModeNumber >= Dev->ModeCount) {
    return EFI_UNSUPPORTED;
  }

  Status = GvtSetMode (Dev, &Dev->Modes[ModeNumber]);
  if (EFI_ERROR (Status)) {
    return EFI_DEVICE_ERROR;
  }

  GvtFillInfo (&Dev->Modes[ModeNumber], &Dev->GopInfo);
  Dev->GopMode.Mode            = ModeNumber;
  Dev->GopMode.FrameBufferSize = (UINTN)Dev->GopInfo.PixelsPerScanLine * 4 * Dev->GopInfo.VerticalResolution;

  //
  // The Blt library keeps per-mode state; size it once for the largest need.
  //
  Status = FrameBufferBltConfigure (
             (VOID *)(UINTN)Dev->FbBase,
             &Dev->GopInfo,
             Dev->BltConfig,
             &Dev->BltConfigSize
             );
  if (Status == RETURN_BUFFER_TOO_SMALL) {
    if (Dev->BltConfig != NULL) {
      FreePool (Dev->BltConfig);
    }

    Dev->BltConfig = AllocatePool (Dev->BltConfigSize);
    if (Dev->BltConfig == NULL) {
      Dev->BltConfigSize = 0;
      return EFI_OUT_OF_RESOURCES;
    }

    Status = FrameBufferBltConfigure (
               (VOID *)(UINTN)Dev->FbBase,
               &Dev->GopInfo,
               Dev->BltConfig,
               &Dev->BltConfigSize
               );
  }

  if (EFI_ERROR (Status)) {
    return EFI_DEVICE_ERROR;
  }

  ZeroMem (&Black, sizeof (Black));
  return This->Blt (
                 This,
                 &Black,
                 EfiBltVideoFill,
                 0,
                 0,
                 0,
                 0,
                 Dev->GopInfo.HorizontalResolution,
                 Dev->GopInfo.VerticalResolution,
                 0
                 );
}

STATIC
EFI_STATUS
EFIAPI
GvtGopBlt (
  IN     EFI_GRAPHICS_OUTPUT_PROTOCOL       *This,
  IN OUT EFI_GRAPHICS_OUTPUT_BLT_PIXEL      *BltBuffer  OPTIONAL,
  IN     EFI_GRAPHICS_OUTPUT_BLT_OPERATION  BltOperation,
  IN     UINTN                              SourceX,
  IN     UINTN                              SourceY,
  IN     UINTN                              DestinationX,
  IN     UINTN                              DestinationY,
  IN     UINTN                              Width,
  IN     UINTN                              Height,
  IN     UINTN                              Delta         OPTIONAL
  )
{
  GVT_DEVICE  *Dev;
  EFI_STATUS  Status;
  EFI_TPL     OldTpl;
  UINTN       Line;
  UINTN       Stride;

  if (This == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Dev = GVT_DEVICE_FROM_GOP (This);
  if (Dev->BltConfig == NULL) {
    return EFI_NOT_READY;
  }

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  Status = FrameBufferBlt (
             Dev->BltConfig,
             BltBuffer,
             BltOperation,
             SourceX,
             SourceY,
             DestinationX,
             DestinationY,
             Width,
             Height,
             Delta
             );

  //
  // The host reads the framebuffer through the GPU, which does not snoop the
  // guest CPU's caches for scanout buffers: write the changed lines back.
  //
  if (!EFI_ERROR (Status) && (BltOperation != EfiBltVideoToBltBuffer)) {
    Stride = (UINTN)Dev->GopInfo.PixelsPerScanLine * 4;
    for (Line = 0; Line < Height; Line++) {
      WriteBackDataCacheRange (
        (UINT8 *)(UINTN)Dev->FbBase + (DestinationY + Line) * Stride + DestinationX * 4,
        Width * 4
        );
    }
  }

  gBS->RestoreTPL (OldTpl);
  return Status;
}

EFI_STATUS
GvtGopInit (
  IN GVT_DEVICE  *Dev
  )
{
  Dev->Gop.QueryMode = GvtGopQueryMode;
  Dev->Gop.SetMode   = GvtGopSetMode;
  Dev->Gop.Blt       = GvtGopBlt;
  Dev->Gop.Mode      = &Dev->GopMode;

  Dev->GopMode.MaxMode         = Dev->ModeCount;
  Dev->GopMode.Mode            = 0;
  Dev->GopMode.Info            = &Dev->GopInfo;
  Dev->GopMode.SizeOfInfo      = sizeof (EFI_GRAPHICS_OUTPUT_MODE_INFORMATION);
  Dev->GopMode.FrameBufferBase = Dev->FbBase;

  return GvtGopSetMode (&Dev->Gop, 0);
}

VOID
GvtGopFini (
  IN GVT_DEVICE  *Dev
  )
{
  if (Dev->BltConfig != NULL) {
    FreePool (Dev->BltConfig);
    Dev->BltConfig     = NULL;
    Dev->BltConfigSize = 0;
  }
}
