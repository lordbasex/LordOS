/** @file
  LordOS GVT-g GOP driver: read the virtual monitor's EDID over I2C-over-AUX
  and turn it into the GOP mode list.

  EDID layout: VESA E-EDID 1.4. Generated timings: VESA CVT 1.2 reduced
  blanking (version 1). Fallback mode: VESA DMT 1024x768 at 60 Hz.

  Copyright (c) 2026 iPERFEX and the LordOS contributors
  SPDX-License-Identifier: MIT
**/

#include "GvtGop.h"

STATIC CONST UINT8  mEdidHeader[8] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };

//
// VESA DMT 1024x768 at 60 Hz, used when no EDID can be read.
//
STATIC CONST GVT_MODE  mFallbackMode = {
  1024, 1048, 1184, 1344, 768, 771, 777, 806, 65000, FALSE, FALSE
};

/**
  Send an I2C-over-AUX request with no data (start, restart or stop).
**/
STATIC
EFI_STATUS
GvtI2cAddressOnly (
  IN GVT_DEVICE  *Dev,
  IN UINT8       Request
  )
{
  UINT8   Msg[3];
  UINT8   Reply[2];
  UINT32  Size;

  Msg[0] = (UINT8)(Request << 4);
  Msg[1] = 0;
  Msg[2] = EDID_I2C_ADDR;
  return GvtAuxTransfer (Dev, Msg, sizeof (Msg), Reply, sizeof (Reply), &Size);
}

EFI_STATUS
GvtReadEdid (
  IN GVT_DEVICE  *Dev
  )
{
  EFI_STATUS  Status;
  UINT8       Msg[5];
  UINT8       Reply[2];
  UINT32      Size;
  UINT32      Index;
  UINT8       Sum;

  Dev->EdidValid = FALSE;

  //
  // Stop any earlier transaction, write the EDID offset (0) and read the
  // 128-byte base block one byte per transaction, then stop.
  //
  GvtI2cAddressOnly (Dev, AUX_I2C_READ);
  Status = GvtI2cAddressOnly (Dev, AUX_I2C_WRITE | AUX_I2C_MOT);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Msg[0] = (AUX_I2C_WRITE | AUX_I2C_MOT) << 4;
  Msg[1] = 0;
  Msg[2] = EDID_I2C_ADDR;
  Msg[3] = 0;                 // one byte
  Msg[4] = 0;                 // EDID offset
  Status = GvtAuxTransfer (Dev, Msg, sizeof (Msg), Reply, sizeof (Reply), &Size);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = GvtI2cAddressOnly (Dev, AUX_I2C_READ | AUX_I2C_MOT);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Msg[0] = (AUX_I2C_READ | AUX_I2C_MOT) << 4;
  Msg[3] = 0;                 // one byte per read
  for (Index = 0; Index < GVT_EDID_SIZE; Index++) {
    Status = GvtAuxTransfer (Dev, Msg, 4, Reply, sizeof (Reply), &Size);
    if (EFI_ERROR (Status) || (Size < 2) || ((Reply[0] & 0xF0) != 0)) {
      GvtI2cAddressOnly (Dev, AUX_I2C_READ);
      return EFI_DEVICE_ERROR;
    }

    Dev->Edid[Index] = Reply[1];
  }

  GvtI2cAddressOnly (Dev, AUX_I2C_READ);

  if (CompareMem (Dev->Edid, mEdidHeader, sizeof (mEdidHeader)) != 0) {
    return EFI_NOT_FOUND;
  }

  Sum = 0;
  for (Index = 0; Index < GVT_EDID_SIZE; Index++) {
    Sum = (UINT8)(Sum + Dev->Edid[Index]);
  }

  if (Sum != 0) {
    return EFI_CRC_ERROR;
  }

  Dev->EdidValid = TRUE;
  return EFI_SUCCESS;
}

/**
  Fill Mode from an 18-byte detailed timing descriptor. Returns FALSE for
  display descriptors and interlaced timings.
**/
STATIC
BOOLEAN
GvtParseDtd (
  IN  CONST UINT8  *D,
  OUT GVT_MODE     *Mode
  )
{
  UINT32  HBlank;
  UINT32  VBlank;
  UINT32  HOff;
  UINT32  HWidth;
  UINT32  VOff;
  UINT32  VWidth;

  Mode->ClockKhz = ((UINT32)D[0] | ((UINT32)D[1] << 8)) * 10;
  if ((Mode->ClockKhz == 0) || ((D[17] & BIT7) != 0)) {
    return FALSE;
  }

  Mode->HActive = D[2] | ((UINT32)(D[4] & 0xF0) << 4);
  HBlank        = D[3] | ((UINT32)(D[4] & 0x0F) << 8);
  Mode->VActive = D[5] | ((UINT32)(D[7] & 0xF0) << 4);
  VBlank        = D[6] | ((UINT32)(D[7] & 0x0F) << 8);
  HOff          = D[8] | ((UINT32)(D[11] & 0xC0) << 2);
  HWidth        = D[9] | ((UINT32)(D[11] & 0x30) << 4);
  VOff          = (D[10] >> 4) | ((UINT32)(D[11] & 0x0C) << 2);
  VWidth        = (D[10] & 0x0F) | ((UINT32)(D[11] & 0x03) << 4);

  if ((Mode->HActive == 0) || (Mode->VActive == 0) || (HBlank == 0) || (VBlank == 0) ||
      (HOff + HWidth > HBlank) || (VOff + VWidth > VBlank))
  {
    return FALSE;
  }

  Mode->HSyncStart = Mode->HActive + HOff;
  Mode->HSyncEnd   = Mode->HSyncStart + HWidth;
  Mode->HTotal     = Mode->HActive + HBlank;
  Mode->VSyncStart = Mode->VActive + VOff;
  Mode->VSyncEnd   = Mode->VSyncStart + VWidth;
  Mode->VTotal     = Mode->VActive + VBlank;

  //
  // Digital separate sync (bits 4:3 = 11) carries both polarities.
  //
  Mode->HSyncPositive = ((D[17] & 0x18) == 0x18) && ((D[17] & BIT1) != 0);
  Mode->VSyncPositive = ((D[17] & 0x18) == 0x18) && ((D[17] & BIT2) != 0);
  return TRUE;
}

/**
  CVT 1.2 reduced-blanking (v1) timings for Width x Height at 60 Hz.
**/
STATIC
VOID
GvtCvtReducedBlanking (
  IN  UINT32    Width,
  IN  UINT32    Height,
  OUT GVT_MODE  *Mode
  )
{
  UINT32  VSync;
  UINT32  Vbi;
  UINT32  MinVbi;
  UINT64  Clock;

  Width = Width & ~7U;      // 8-pixel character cell

  //
  // Vertical sync width encodes the aspect ratio.
  //
  if (Width * 3 == Height * 4) {
    VSync = 4;
  } else if (Width * 9 == Height * 16) {
    VSync = 5;
  } else if (Width * 10 == Height * 16) {
    VSync = 6;
  } else if ((Width * 4 == Height * 5) || (Width * 9 == Height * 15)) {
    VSync = 7;
  } else {
    VSync = 10;
  }

  //
  // At least 460 us of vertical blanking: lines = 460 us / line period,
  // line period = (1/60 s - 460 us) / height (all scaled by 3 to stay exact).
  //
  Vbi    = (460 * 3 * Height) / (50000 - 460 * 3) + 1;
  MinVbi = 3 + VSync + 6;
  Vbi    = MAX (Vbi, MinVbi);

  Mode->HActive       = Width;
  Mode->HSyncStart    = Width + 48;
  Mode->HSyncEnd      = Width + 48 + 32;
  Mode->HTotal        = Width + 160;
  Mode->VActive       = Height;
  Mode->VSyncStart    = Height + 3;
  Mode->VSyncEnd      = Height + 3 + VSync;
  Mode->VTotal        = Height + Vbi;
  Mode->HSyncPositive = TRUE;
  Mode->VSyncPositive = FALSE;

  //
  // Pixel clock in kHz, rounded down to the 250 kHz CVT clock step.
  //
  Clock          = DivU64x32 (MultU64x32 ((UINT64)Mode->HTotal * Mode->VTotal, 60), 1000);
  Mode->ClockKhz = (UINT32)(Clock - ModU64x32 (Clock, 250));
}

STATIC
VOID
GvtAddMode (
  IN GVT_DEVICE      *Dev,
  IN CONST GVT_MODE  *Mode
  )
{
  UINT32  Index;

  if ((Dev->ModeCount >= GVT_MAX_MODES) ||
      (Mode->HActive > GVT_MAX_WIDTH) || (Mode->VActive > GVT_MAX_HEIGHT))
  {
    return;
  }

  //
  // Every mode but the first (preferred) one must fit inside the preferred
  // one, and be at least 1024x768: GraphicsConsole then keeps the preferred
  // mode instead of switching to its 800x600 default.
  //
  if (Dev->ModeCount > 0) {
    if ((Mode->HActive > Dev->Modes[0].HActive) || (Mode->VActive > Dev->Modes[0].VActive) ||
        (Mode->HActive < GVT_MIN_WIDTH) || (Mode->VActive < GVT_MIN_HEIGHT))
    {
      return;
    }
  }

  for (Index = 0; Index < Dev->ModeCount; Index++) {
    if ((Dev->Modes[Index].HActive == Mode->HActive) && (Dev->Modes[Index].VActive == Mode->VActive)) {
      return;
    }
  }

  CopyMem (&Dev->Modes[Dev->ModeCount], Mode, sizeof (*Mode));
  Dev->ModeCount++;
}

STATIC
VOID
GvtAddCvtMode (
  IN GVT_DEVICE  *Dev,
  IN UINT32      Width,
  IN UINT32      Height
  )
{
  GVT_MODE  Mode;

  GvtCvtReducedBlanking (Width, Height, &Mode);
  GvtAddMode (Dev, &Mode);
}

/**
  Mode 0 is the EDID's preferred timing (the first detailed descriptor), then
  the other detailed timings, standard timings and established timings, all
  no larger than the preferred mode. With no EDID, only 1024x768.
**/
VOID
GvtBuildModeList (
  IN GVT_DEVICE  *Dev
  )
{
  GVT_MODE     Mode;
  UINT32       Index;
  CONST UINT8  *E;
  UINT32       Width;
  UINT32       Height;

  Dev->ModeCount = 0;
  E              = Dev->Edid;

  if (Dev->EdidValid) {
    for (Index = 0; Index < 4; Index++) {
      if (GvtParseDtd (&E[0x36 + Index * 18], &Mode)) {
        GvtAddMode (Dev, &Mode);
      }
    }
  }

  if (Dev->ModeCount == 0) {
    GvtAddMode (Dev, &mFallbackMode);
    return;
  }

  //
  // Standard timings: (width / 8 - 31), then aspect ratio in bits 7:6.
  //
  for (Index = 0; Index < 8; Index++) {
    if ((E[0x26 + Index * 2] <= 1) && (E[0x27 + Index * 2] <= 1)) {
      continue;
    }

    Width = ((UINT32)E[0x26 + Index * 2] + 31) * 8;
    switch (E[0x27 + Index * 2] >> 6) {
      case 0:  Height = Width * 10 / 16;
        break;
      case 1:  Height = Width * 3 / 4;
        break;
      case 2:  Height = Width * 4 / 5;
        break;
      default: Height = Width * 9 / 16;
        break;
    }

    GvtAddCvtMode (Dev, Width, Height);
  }

  //
  // Established timings of at least 1024x768.
  //
  if ((E[0x24] & 0x0E) != 0) {
    GvtAddCvtMode (Dev, 1024, 768);
  }

  if ((E[0x24] & 0x01) != 0) {
    GvtAddCvtMode (Dev, 1280, 1024);
  }

  if (Dev->ModeCount == 1) {
    GvtAddMode (Dev, &mFallbackMode);
  }

  for (Index = 0; Index < Dev->ModeCount; Index++) {
    DEBUG ((
      DEBUG_INFO,
      "GvtGop: mode %u: %ux%u, %u kHz\n",
      Index,
      Dev->Modes[Index].HActive,
      Dev->Modes[Index].VActive,
      Dev->Modes[Index].ClockKhz
      ));
  }
}
