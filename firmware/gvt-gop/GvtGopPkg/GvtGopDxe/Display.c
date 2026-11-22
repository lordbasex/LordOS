/** @file
  LordOS GVT-g GOP driver: MMIO access, the vGPU probe, the framebuffer in the
  global GTT, the DP AUX channel and the mode set.

  The mode set follows the Skylake-family DisplayPort enable order: power
  wells, DPLL, DDI clock, DP transport and DDI buffer, link training,
  transcoder timings and M/N, transcoder enable, then the plane (the order the
  Linux i915 driver uses on this hardware). On a GVT-g vGPU every register
  is emulated; the values still describe a real, consistent mode so the guest
  OS driver can read them back, and GVT-g derives the vblank rate from them.

  Copyright (c) 2026 iPERFEX and the LordOS contributors
  SPDX-License-Identifier: MIT
**/

#include "GvtGop.h"

#define POLL_STEP_US   10
#define POLL_LIMIT_US  20000

UINT32
GvtRead (
  IN GVT_DEVICE  *Dev,
  IN UINT32      Offset
  )
{
  UINT32  Value;

  if ((Offset >= GVT_MMIO_SIZE) || ((Offset & 3) != 0)) {
    ASSERT (FALSE);
    return 0;
  }

  Value = 0;
  Dev->PciIo->Mem.Read (Dev->PciIo, EfiPciIoWidthUint32, 0, Offset, 1, &Value);
  return Value;
}

VOID
GvtWrite (
  IN GVT_DEVICE  *Dev,
  IN UINT32      Offset,
  IN UINT32      Value
  )
{
  if ((Offset >= GVT_MMIO_SIZE) || ((Offset & 3) != 0)) {
    ASSERT (FALSE);
    return;
  }

  Dev->PciIo->Mem.Write (Dev->PciIo, EfiPciIoWidthUint32, 0, Offset, 1, &Value);
}

STATIC
BOOLEAN
GvtPoll (
  IN GVT_DEVICE  *Dev,
  IN UINT32      Offset,
  IN UINT32      Mask,
  IN UINT32      Expected
  )
{
  UINTN  Waited;

  for (Waited = 0; Waited < POLL_LIMIT_US; Waited += POLL_STEP_US) {
    if ((GvtRead (Dev, Offset) & Mask) == Expected) {
      return TRUE;
    }

    gBS->Stall (POLL_STEP_US);
  }

  DEBUG ((DEBUG_WARN, "GvtGop: timeout on 0x%x mask 0x%x\n", Offset, Mask));
  return FALSE;
}

STATIC
EFI_STATUS
GvtWritePte (
  IN GVT_DEVICE  *Dev,
  IN UINT32      PageIndex,
  IN UINT64      Pte
  )
{
  UINT64  Offset;

  Offset = GVT_GGTT_OFFSET + (UINT64)PageIndex * sizeof (UINT64);
  if (Offset + sizeof (UINT64) > Dev->MmioSize) {
    return EFI_INVALID_PARAMETER;
  }

  return Dev->PciIo->Mem.Write (Dev->PciIo, EfiPciIoWidthUint64, 0, Offset, 1, &Pte);
}

UINT32
GvtStride (
  IN UINT32  Width
  )
{
  return ALIGN_VALUE (Width * 4, PLANE_STRIDE_ALIGN);
}

/**
  Confirm this is a GVT-g vGPU we can drive and learn its resources.
**/
EFI_STATUS
GvtProbe (
  IN GVT_DEVICE  *Dev
  )
{
  UINT64  Magic;
  UINT32  Version;
  UINT32  Ddi;
  UINT32  MapBase;
  UINT32  MapSize;
  UINT32  HidBase;
  UINT32  HidSize;

  if (Dev->MmioSize < GVT_GGTT_OFFSET + SIZE_8MB) {
    DEBUG ((DEBUG_INFO, "GvtGop: BAR0 is 0x%lx bytes, not a gen8+ GTTMMADR\n", Dev->MmioSize));
    return EFI_UNSUPPORTED;
  }

  Magic = LShiftU64 (GvtRead (Dev, PVINFO_MAGIC_HI), 32) | GvtRead (Dev, PVINFO_MAGIC_LO);
  if (Magic != PVINFO_MAGIC) {
    DEBUG ((DEBUG_INFO, "GvtGop: no PVINFO magic (0x%lx), not a GVT-g vGPU\n", Magic));
    return EFI_UNSUPPORTED;
  }

  Version = GvtRead (Dev, PVINFO_VERSION);
  if ((Version & 0xFFFF) != PVINFO_VERSION_MAJOR) {
    DEBUG ((DEBUG_WARN, "GvtGop: PVINFO version %u.%u unsupported\n", Version & 0xFFFF, Version >> 16));
    return EFI_UNSUPPORTED;
  }

  MapBase = GvtRead (Dev, PVINFO_MAPPABLE_BASE);
  MapSize = GvtRead (Dev, PVINFO_MAPPABLE_SIZE);
  HidBase = GvtRead (Dev, PVINFO_NONMAPPABLE_BASE);
  HidSize = GvtRead (Dev, PVINFO_NONMAPPABLE_SIZE);
  DEBUG ((
    DEBUG_INFO,
    "GvtGop: vGPU %u, aperture GM 0x%x+0x%x, hidden GM 0x%x+0x%x\n",
    GvtRead (Dev, PVINFO_VGT_ID),
    MapBase,
    MapSize,
    HidBase,
    HidSize
    ));

  //
  // Prefer the mappable (aperture) range, as firmware on real hardware does;
  // use the hidden range only when the aperture slice is too small.
  //
  if (((MapBase & EFI_PAGE_MASK) == 0) && (MapSize >= SIZE_16MB)) {
    Dev->GmBase = MapBase;
    Dev->GmSize = MapSize & ~(UINT32)EFI_PAGE_MASK;
  } else if (((HidBase & EFI_PAGE_MASK) == 0) && (HidSize >= SIZE_16MB)) {
    Dev->GmBase = HidBase;
    Dev->GmSize = HidSize & ~(UINT32)EFI_PAGE_MASK;
  } else {
    DEBUG ((DEBUG_ERROR, "GvtGop: no usable graphics memory range\n"));
    return EFI_UNSUPPORTED;
  }

  //
  // GVT-g wires its virtual monitor to one DDI and reports it in transcoder
  // A's DDI function control at vGPU reset (port D on the Skylake family).
  //
  Ddi = GvtRead (Dev, TRANS_DDI_FUNC_CTL_A);
  Dev->Port = (Ddi & TRANS_DDI_PORT_MASK) >> TRANS_DDI_PORT_SHIFT;
  if (((Ddi & TRANS_DDI_FUNC_ENABLE) == 0) || (Dev->Port < PORT_B) || (Dev->Port > PORT_D)) {
    if ((GvtRead (Dev, DDI_BUF_CTL (PORT_D)) & DDI_BUF_CTL_ENABLE) != 0) {
      Dev->Port = PORT_D;
    } else if ((GvtRead (Dev, DDI_BUF_CTL (PORT_B)) & DDI_BUF_CTL_ENABLE) != 0) {
      Dev->Port = PORT_B;
    } else {
      DEBUG ((DEBUG_ERROR, "GvtGop: no virtual monitor on any DDI\n"));
      return EFI_UNSUPPORTED;
    }
  }

  DEBUG ((DEBUG_INFO, "GvtGop: virtual monitor on DDI %c\n", 'A' + Dev->Port));
  return EFI_SUCCESS;
}

/**
  Allocate the framebuffer for the largest mode and map it linearly into the
  global GTT at the start of the guest's graphics memory range.
**/
EFI_STATUS
GvtMapFramebuffer (
  IN GVT_DEVICE  *Dev
  )
{
  EFI_STATUS  Status;
  UINTN       Bytes;
  UINTN       Index;
  UINT32      First;

  Bytes = 0;
  for (Index = 0; Index < Dev->ModeCount; Index++) {
    Bytes = MAX (Bytes, (UINTN)GvtStride (Dev->Modes[Index].HActive) * Dev->Modes[Index].VActive);
  }

  Dev->FbPages = EFI_SIZE_TO_PAGES (Bytes);
  if ((Dev->FbPages == 0) || (EFI_PAGES_TO_SIZE (Dev->FbPages) > Dev->GmSize)) {
    return EFI_OUT_OF_RESOURCES;
  }

  //
  // Reserved memory: the OS must never reuse these pages, because the plane
  // keeps scanning them (and OS loaders draw into them) until the OS driver
  // reprograms the display.
  //
  Status = gBS->AllocatePages (AllocateAnyPages, EfiReservedMemoryType, Dev->FbPages, &Dev->FbBase);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  ZeroMem ((VOID *)(UINTN)Dev->FbBase, EFI_PAGES_TO_SIZE (Dev->FbPages));
  WriteBackDataCacheRange ((VOID *)(UINTN)Dev->FbBase, EFI_PAGES_TO_SIZE (Dev->FbPages));

  First = Dev->GmBase >> EFI_PAGE_SHIFT;
  for (Index = 0; Index < Dev->FbPages; Index++) {
    Status = GvtWritePte (
               Dev,
               First + (UINT32)Index,
               ((Dev->FbBase + EFI_PAGES_TO_SIZE (Index)) & GVT_GGTT_ADDR_MASK) | GVT_GGTT_PTE_PRESENT
               );
    if (EFI_ERROR (Status)) {
      GvtUnmapFramebuffer (Dev);
      return Status;
    }
  }

  GvtWrite (Dev, GFX_FLSH_CNTL_GEN6, GFX_FLSH_CNTL_EN);
  GvtRead (Dev, GFX_FLSH_CNTL_GEN6);

  DEBUG ((
    DEBUG_INFO,
    "GvtGop: framebuffer 0x%lx (%u pages) at GM 0x%x\n",
    Dev->FbBase,
    (UINT32)Dev->FbPages,
    Dev->GmBase
    ));
  return EFI_SUCCESS;
}

VOID
GvtUnmapFramebuffer (
  IN GVT_DEVICE  *Dev
  )
{
  UINTN   Index;
  UINT32  First;

  if (Dev->FbPages == 0) {
    return;
  }

  First = Dev->GmBase >> EFI_PAGE_SHIFT;
  for (Index = 0; Index < Dev->FbPages; Index++) {
    GvtWritePte (Dev, First + (UINT32)Index, 0);
  }

  GvtWrite (Dev, GFX_FLSH_CNTL_GEN6, GFX_FLSH_CNTL_EN);
  gBS->FreePages (Dev->FbBase, Dev->FbPages);
  Dev->FbBase  = 0;
  Dev->FbPages = 0;
}

/**
  One DP AUX transaction on the monitor's port. Send holds the request header
  and data (at most 20 bytes); Reply receives the reply (code byte first).
**/
EFI_STATUS
GvtAuxTransfer (
  IN  GVT_DEVICE   *Dev,
  IN  CONST UINT8  *Send,
  IN  UINT32       SendSize,
  OUT UINT8        *Reply,
  IN  UINT32       ReplyMax,
  OUT UINT32       *ReplySize
  )
{
  UINT32  Ctl;
  UINT32  Word;
  UINT32  Index;
  UINT32  Size;

  if ((SendSize == 0) || (SendSize > 20) || (ReplyMax > 20)) {
    return EFI_INVALID_PARAMETER;
  }

  for (Index = 0; Index < 5; Index++) {
    Word = 0;
    if (Index * 4 < SendSize) {
      for (Size = 0; Size < 4; Size++) {
        Word <<= 8;
        if (Index * 4 + Size < SendSize) {
          Word |= Send[Index * 4 + Size];
        }
      }
    }

    GvtWrite (Dev, DP_AUX_CH_DATA (Dev->Port, Index), Word);
  }

  Ctl = DP_AUX_CTL_SEND_BUSY | DP_AUX_CTL_DONE | DP_AUX_CTL_TIME_OUT_ERROR |
        DP_AUX_CTL_TIME_OUT_MAX | DP_AUX_CTL_RECEIVE_ERROR |
        (SendSize << DP_AUX_CTL_MSG_SIZE_SHIFT) |
        DP_AUX_CTL_FW_SYNC_PULSE (32) | DP_AUX_CTL_SYNC_PULSE (32);
  GvtWrite (Dev, DP_AUX_CH_CTL (Dev->Port), Ctl);

  if (!GvtPoll (Dev, DP_AUX_CH_CTL (Dev->Port), DP_AUX_CTL_SEND_BUSY, 0)) {
    return EFI_TIMEOUT;
  }

  Ctl = GvtRead (Dev, DP_AUX_CH_CTL (Dev->Port));
  //
  // Clear the sticky status bits (write one to clear, no SEND_BUSY).
  //
  GvtWrite (Dev, DP_AUX_CH_CTL (Dev->Port), DP_AUX_CTL_DONE | DP_AUX_CTL_TIME_OUT_ERROR | DP_AUX_CTL_RECEIVE_ERROR);
  if ((Ctl & (DP_AUX_CTL_TIME_OUT_ERROR | DP_AUX_CTL_RECEIVE_ERROR)) != 0) {
    return EFI_DEVICE_ERROR;
  }

  Size = (Ctl & DP_AUX_CTL_MSG_SIZE_MASK) >> DP_AUX_CTL_MSG_SIZE_SHIFT;
  Size = MIN (Size, ReplyMax);
  for (Index = 0; Index < Size; Index++) {
    if ((Index & 3) == 0) {
      Word = GvtRead (Dev, DP_AUX_CH_DATA (Dev->Port, Index / 4));
    }

    Reply[Index] = (UINT8)(Word >> (24 - 8 * (Index & 3)));
  }

  *ReplySize = Size;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
GvtDpcdWrite (
  IN GVT_DEVICE  *Dev,
  IN UINT32      Address,
  IN UINT8       Value
  )
{
  UINT8       Msg[5];
  UINT8       Reply[2];
  UINT32      Size;
  EFI_STATUS  Status;

  Msg[0] = (UINT8)((AUX_NATIVE_WRITE << 4) | ((Address >> 16) & 0xF));
  Msg[1] = (UINT8)(Address >> 8);
  Msg[2] = (UINT8)Address;
  Msg[3] = 0;                 // one byte
  Msg[4] = Value;
  Status = GvtAuxTransfer (Dev, Msg, sizeof (Msg), Reply, sizeof (Reply), &Size);
  if (!EFI_ERROR (Status) && ((Size == 0) || ((Reply[0] & 0x30) != 0))) {
    Status = EFI_DEVICE_ERROR;  // NAK or DEFER
  }

  return Status;
}

/**
  Clamp a ratio to the 24-bit M/N fields: M/N = Num/Den with a fixed N.
**/
STATIC
UINT32
GvtMValue (
  IN UINT64  Num,
  IN UINT64  Den,
  IN UINT32  N
  )
{
  UINT64  M;

  M = DivU64x64Remainder (MultU64x32 (Num, N), Den, NULL);
  return (UINT32)MIN (M, 0xFFFFFF);
}

/**
  Turn the primary plane, transcoder A and the monitor's DDI off.
**/
VOID
GvtDisableDisplay (
  IN GVT_DEVICE  *Dev
  )
{
  GvtWrite (Dev, PLANE_CTL_1_A, 0);
  GvtWrite (Dev, PLANE_SURF_1_A, 0);
  GvtWrite (Dev, TRANSCONF_A, 0);
  GvtPoll (Dev, TRANSCONF_A, TRANSCONF_STATE_ENABLE, 0);
  GvtWrite (Dev, TRANS_DDI_FUNC_CTL_A, 0);
  GvtWrite (Dev, DP_TP_CTL (Dev->Port), 0);
  GvtWrite (Dev, DDI_BUF_CTL (Dev->Port), 0);
  GvtPoll (Dev, DDI_BUF_CTL (Dev->Port), DDI_BUF_IS_IDLE, DDI_BUF_IS_IDLE);
}

/**
  Program pipe A, transcoder A and the monitor's DDI for Mode and scan out
  the framebuffer as linear XRGB8888.
**/
EFI_STATUS
GvtSetMode (
  IN GVT_DEVICE      *Dev,
  IN CONST GVT_MODE  *Mode
  )
{
  UINT32  Port;
  UINT32  Value;
  UINT32  Wells;
  UINT32  Stride;

  Port   = Dev->Port;
  Stride = GvtStride (Mode->HActive);
  if ((UINT64)Stride * Mode->VActive > EFI_PAGES_TO_SIZE (Dev->FbPages)) {
    return EFI_UNSUPPORTED;
  }

  GvtDisableDisplay (Dev);

  //
  // The VGA plane must be off before a pipe drives the port.
  //
  GvtWrite (Dev, VGACNTRL, VGACNTRL_DISP_DISABLE);

  //
  // Power wells: PW1, PW2 and the DDI's IO.
  //
  Wells = PWR_WELL_REQ (PW_IDX_PW_1) | PWR_WELL_REQ (PW_IDX_PW_2) |
          PWR_WELL_REQ (PW_IDX_DDI_B + Port - PORT_B);
  GvtWrite (Dev, PWR_WELL_CTL2, GvtRead (Dev, PWR_WELL_CTL2) | Wells);
  GvtPoll (Dev, PWR_WELL_CTL2, Wells >> 1, Wells >> 1);

  //
  // DPLL1 in DP mode at the HBR2 link rate, then route it to the DDI.
  //
  Value  = GvtRead (Dev, DPLL_CTRL1) & ~DPLL_CTRL1_ID_MASK (GVT_PORT_DPLL);
  Value |= DPLL_CTRL1_OVERRIDE (GVT_PORT_DPLL) | DPLL_CTRL1_LINK_RATE (DPLL_CTRL1_LINK_RATE_2700, GVT_PORT_DPLL);
  GvtWrite (Dev, DPLL_CTRL1, Value);
  GvtRead (Dev, DPLL_CTRL1);
  GvtWrite (Dev, LCPLL2_CTL, LCPLL_PLL_ENABLE);
  GvtPoll (Dev, DPLL_STATUS, DPLL_LOCK (GVT_PORT_DPLL), DPLL_LOCK (GVT_PORT_DPLL));

  Value  = GvtRead (Dev, DPLL_CTRL2);
  Value &= ~(DPLL_CTRL2_DDI_CLK_OFF (Port) | DPLL_CTRL2_DDI_CLK_SEL_MASK (Port));
  Value |= DPLL_CTRL2_DDI_CLK_SEL (GVT_PORT_DPLL, Port) | DPLL_CTRL2_DDI_SEL_OVERRIDE (Port);
  GvtWrite (Dev, DPLL_CTRL2, Value);
  GvtRead (Dev, DPLL_CTRL2);

  //
  // DP transport with training pattern 1, DDI buffer on, train the sink.
  //
  GvtWrite (Dev, DP_TP_CTL (Port), DP_TP_CTL_ENABLE | DP_TP_CTL_MODE_SST | DP_TP_CTL_ENHANCED_FRAME | DP_TP_CTL_TRAIN_PAT1);
  GvtWrite (Dev, DDI_BUF_CTL (Port), DDI_BUF_CTL_ENABLE | DDI_PORT_WIDTH (GVT_LINK_LANES));
  GvtPoll (Dev, DDI_BUF_CTL (Port), DDI_BUF_IS_IDLE, 0);

  GvtDpcdWrite (Dev, DPCD_SET_POWER, 1);
  GvtDpcdWrite (Dev, DPCD_LINK_BW_SET, DPCD_LINK_BW_5_4);
  GvtDpcdWrite (Dev, DPCD_LANE_COUNT_SET, DPCD_LANE_COUNT_ENHANCED | GVT_LINK_LANES);
  GvtDpcdWrite (Dev, DPCD_TRAINING_PATTERN_SET, DPCD_TRAINING_PAT1_NOSCR);
  GvtWrite (Dev, DP_TP_CTL (Port), DP_TP_CTL_ENABLE | DP_TP_CTL_MODE_SST | DP_TP_CTL_ENHANCED_FRAME | DP_TP_CTL_TRAIN_PAT2);
  GvtDpcdWrite (Dev, DPCD_TRAINING_PATTERN_SET, DPCD_TRAINING_PAT2_NOSCR);
  GvtWrite (Dev, DP_TP_CTL (Port), DP_TP_CTL_ENABLE | DP_TP_CTL_MODE_SST | DP_TP_CTL_ENHANCED_FRAME | DP_TP_CTL_TRAIN_IDLE);
  GvtPoll (Dev, DP_TP_STATUS (Port), DP_TP_STATUS_IDLE_DONE, DP_TP_STATUS_IDLE_DONE);
  GvtDpcdWrite (Dev, DPCD_TRAINING_PATTERN_SET, 0);
  GvtWrite (Dev, DP_TP_CTL (Port), DP_TP_CTL_ENABLE | DP_TP_CTL_MODE_SST | DP_TP_CTL_ENHANCED_FRAME | DP_TP_CTL_TRAIN_NORMAL);

  //
  // Transcoder A timings (each field minus one) and the pipe source size.
  //
  GvtWrite (Dev, TRANS_HTOTAL_A, ((Mode->HTotal - 1) << 16) | (Mode->HActive - 1));
  GvtWrite (Dev, TRANS_HBLANK_A, ((Mode->HTotal - 1) << 16) | (Mode->HActive - 1));
  GvtWrite (Dev, TRANS_HSYNC_A, ((Mode->HSyncEnd - 1) << 16) | (Mode->HSyncStart - 1));
  GvtWrite (Dev, TRANS_VTOTAL_A, ((Mode->VTotal - 1) << 16) | (Mode->VActive - 1));
  GvtWrite (Dev, TRANS_VBLANK_A, ((Mode->VTotal - 1) << 16) | (Mode->VActive - 1));
  GvtWrite (Dev, TRANS_VSYNC_A, ((Mode->VSyncEnd - 1) << 16) | (Mode->VSyncStart - 1));
  GvtWrite (Dev, PIPESRC_A, ((Mode->HActive - 1) << 16) | (Mode->VActive - 1));

  //
  // Data M/N = stream bits / link bits; link M/N = pixel clock / link clock.
  //
  GvtWrite (
    Dev,
    PIPE_DATA_M1_A,
    PIPE_TU_SIZE (64) | GvtMValue ((UINT64)Mode->ClockKhz * 24, (UINT64)GVT_LINK_CLOCK_KHZ * GVT_LINK_LANES * 8, 0x800000)
    );
  GvtWrite (Dev, PIPE_DATA_N1_A, 0x800000);
  GvtWrite (Dev, PIPE_LINK_M1_A, GvtMValue (Mode->ClockKhz, GVT_LINK_CLOCK_KHZ, 0x80000));
  GvtWrite (Dev, PIPE_LINK_N1_A, 0x80000);

  Value = TRANS_DDI_FUNC_ENABLE | (Port << TRANS_DDI_PORT_SHIFT) | TRANS_DDI_MODE_DP_SST |
          TRANS_DDI_BPC_8 | TRANS_DDI_PORT_WIDTH (GVT_LINK_LANES);
  if (Mode->HSyncPositive) {
    Value |= TRANS_DDI_PHSYNC;
  }

  if (Mode->VSyncPositive) {
    Value |= TRANS_DDI_PVSYNC;
  }

  GvtWrite (Dev, TRANS_DDI_FUNC_CTL_A, Value);
  GvtWrite (Dev, TRANSCONF_A, TRANSCONF_ENABLE);
  GvtPoll (Dev, TRANSCONF_A, TRANSCONF_STATE_ENABLE, TRANSCONF_STATE_ENABLE);

  //
  // Primary plane: linear XRGB8888, the surface write arms the update.
  //
  GvtWrite (Dev, PLANE_STRIDE_1_A, Stride / PLANE_STRIDE_ALIGN);
  GvtWrite (Dev, PLANE_POS_1_A, 0);
  GvtWrite (Dev, PLANE_SIZE_1_A, ((Mode->VActive - 1) << 16) | (Mode->HActive - 1));
  GvtWrite (Dev, PLANE_OFFSET_1_A, 0);
  GvtWrite (Dev, PLANE_CTL_1_A, PLANE_CTL_ENABLE | PLANE_CTL_FORMAT_XRGB_8888 | PLANE_CTL_TILED_LINEAR | PLANE_CTL_ALPHA_DISABLE);
  GvtWrite (Dev, PLANE_SURF_1_A, Dev->GmBase);

  DEBUG ((
    DEBUG_INFO,
    "GvtGop: mode %ux%u, %u kHz, totals %ux%u\n",
    Mode->HActive,
    Mode->VActive,
    Mode->ClockKhz,
    Mode->HTotal,
    Mode->VTotal
    ));
  return EFI_SUCCESS;
}
