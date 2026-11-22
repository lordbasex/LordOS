/** @file
  LordOS GVT-g GOP driver: hand the vGPU's OpRegion to the guest OS.

  With x-igd-opregion=on QEMU exposes the OpRegion (and its VBT) that GVT-g
  provides for the vGPU as the fw_cfg file "etc/igd-opregion" and makes the
  ASLS register (PCI config 0xFC) writable. Firmware is expected to copy the
  file into reserved memory below 4 GB and write that address to ASLS, where
  the OS graphics driver looks for it (QEMU docs/igd-assign.txt; Intel
  OpRegion specification).

  Copyright (c) 2026 iPERFEX and the LordOS contributors
  SPDX-License-Identifier: MIT
**/

#include "GvtGop.h"
#include <Library/IoLib.h>

#define FW_CFG_PORT_SEL     0x510
#define FW_CFG_PORT_DATA    0x511
#define FW_CFG_SIGNATURE    0x0000
#define FW_CFG_FILE_DIR     0x0019
#define FW_CFG_NAME_SIZE    56
#define IGD_ASLS            0xFC
#define OPREGION_MAX_SIZE   SIZE_64KB

STATIC CONST CHAR8  mOpRegionFile[]      = "etc/igd-opregion";
STATIC CONST CHAR8  mOpRegionSignature[] = "IntelGraphicsMem";

STATIC
VOID
FwCfgSelect (
  IN UINT16  Key
  )
{
  IoWrite16 (FW_CFG_PORT_SEL, Key);
}

STATIC
VOID
FwCfgRead (
  IN  UINTN  Size,
  OUT VOID   *Buffer
  )
{
  IoReadFifo8 (FW_CFG_PORT_DATA, Size, Buffer);
}

STATIC
UINT32
FwCfgReadBe32 (
  VOID
  )
{
  UINT32  Value;

  FwCfgRead (sizeof (Value), &Value);
  return SwapBytes32 (Value);
}

/**
  Find a fw_cfg file by name. Returns its selector key and size.
**/
STATIC
BOOLEAN
FwCfgFindFile (
  IN  CONST CHAR8  *Name,
  OUT UINT16       *Key,
  OUT UINT32       *Size
  )
{
  CHAR8   Signature[4];
  UINT32  Count;
  UINT32  Index;
  UINT32  FileSize;
  UINT16  Select;
  UINT16  Reserved;
  CHAR8   FileName[FW_CFG_NAME_SIZE];

  FwCfgSelect (FW_CFG_SIGNATURE);
  FwCfgRead (sizeof (Signature), Signature);
  if (CompareMem (Signature, "QEMU", sizeof (Signature)) != 0) {
    return FALSE;
  }

  FwCfgSelect (FW_CFG_FILE_DIR);
  Count = FwCfgReadBe32 ();
  for (Index = 0; Index < Count && Index < 4096; Index++) {
    FileSize = FwCfgReadBe32 ();
    FwCfgRead (sizeof (Select), &Select);
    FwCfgRead (sizeof (Reserved), &Reserved);
    FwCfgRead (sizeof (FileName), FileName);
    FileName[FW_CFG_NAME_SIZE - 1] = '\0';
    if (AsciiStrCmp (FileName, Name) == 0) {
      *Key  = SwapBytes16 (Select);
      *Size = FileSize;
      return TRUE;
    }
  }

  return FALSE;
}

VOID
GvtInstallOpRegion (
  IN GVT_DEVICE  *Dev
  )
{
  EFI_STATUS            Status;
  UINT32                Asls;
  UINT16                Key;
  UINT32                Size;
  EFI_PHYSICAL_ADDRESS  Address;
  UINTN                 Pages;

  Asls = 0;
  Dev->PciIo->Pci.Read (Dev->PciIo, EfiPciIoWidthUint32, IGD_ASLS, 1, &Asls);
  if (Asls != 0) {
    DEBUG ((DEBUG_INFO, "GvtGop: OpRegion already at 0x%x\n", Asls));
    return;
  }

  if (!FwCfgFindFile (mOpRegionFile, &Key, &Size)) {
    DEBUG ((DEBUG_INFO, "GvtGop: no %a in fw_cfg (x-igd-opregion off?)\n", mOpRegionFile));
    return;
  }

  if ((Size < SIZE_8KB) || (Size > OPREGION_MAX_SIZE)) {
    DEBUG ((DEBUG_WARN, "GvtGop: OpRegion size %u out of range\n", Size));
    return;
  }

  //
  // ASLS is a 32-bit register: the copy must live below 4 GB, in memory the
  // OS keeps reserved (ACPI NVS, as platform firmware does).
  //
  Pages   = EFI_SIZE_TO_PAGES (Size);
  Address = SIZE_4GB - 1;
  Status  = gBS->AllocatePages (AllocateMaxAddress, EfiACPIMemoryNVS, Pages, &Address);
  if (EFI_ERROR (Status)) {
    return;
  }

  ZeroMem ((VOID *)(UINTN)Address, EFI_PAGES_TO_SIZE (Pages));
  FwCfgSelect (Key);
  FwCfgRead (Size, (VOID *)(UINTN)Address);

  if (CompareMem ((VOID *)(UINTN)Address, mOpRegionSignature, sizeof (mOpRegionSignature) - 1) != 0) {
    DEBUG ((DEBUG_WARN, "GvtGop: %a has no OpRegion signature\n", mOpRegionFile));
    gBS->FreePages (Address, Pages);
    return;
  }

  Asls = (UINT32)Address;
  Dev->PciIo->Pci.Write (Dev->PciIo, EfiPciIoWidthUint32, IGD_ASLS, 1, &Asls);
  DEBUG ((DEBUG_INFO, "GvtGop: OpRegion (%u bytes) at 0x%x\n", Size, Asls));
}
