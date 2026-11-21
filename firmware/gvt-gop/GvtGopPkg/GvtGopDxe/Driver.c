/** @file
  LordOS GVT-g GOP driver: UEFI driver model glue (UEFI 2.10, chapter 11).

  Binds to Intel display controllers of the Skylake family (gen9) that are
  GVT-g vGPUs, lights the vGPU's own virtual monitor and installs a Graphics
  Output Protocol on a child handle.

  Copyright (c) 2026 iPERFEX and the LordOS contributors
  SPDX-License-Identifier: MIT
**/

#include "GvtGop.h"

#define INTEL_VENDOR_ID  0x8086

STATIC EFI_DRIVER_BINDING_PROTOCOL  mGvtDriverBinding;

/**
  Gen9 display (Skylake, Kaby Lake, Amber Lake, Coffee Lake, Comet Lake) by
  PCI device ID prefix. Broxton/Gemini Lake (0x5Axx, 0x31xx) and Broadwell
  use other PLLs and are not handled.
**/
STATIC
BOOLEAN
GvtIsGen9 (
  IN UINT16  DeviceId
  )
{
  switch (DeviceId >> 8) {
    case 0x19:      // Skylake
    case 0x59:      // Kaby Lake
    case 0x87:      // Amber Lake / Kaby Lake
    case 0x3E:      // Coffee Lake
    case 0x9B:      // Comet Lake
      return TRUE;
    default:
      return FALSE;
  }
}

STATIC
EFI_STATUS
EFIAPI
GvtSupported (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   Controller,
  IN EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath
  )
{
  EFI_STATUS           Status;
  EFI_PCI_IO_PROTOCOL  *PciIo;
  PCI_TYPE00           Pci;

  Status = gBS->OpenProtocol (
                  Controller,
                  &gEfiPciIoProtocolGuid,
                  (VOID **)&PciIo,
                  This->DriverBindingHandle,
                  Controller,
                  EFI_OPEN_PROTOCOL_BY_DRIVER
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = PciIo->Pci.Read (PciIo, EfiPciIoWidthUint32, 0, sizeof (Pci) / sizeof (UINT32), &Pci);
  if (!EFI_ERROR (Status)) {
    if ((Pci.Hdr.VendorId != INTEL_VENDOR_ID) ||
        (Pci.Hdr.ClassCode[2] != PCI_CLASS_DISPLAY) ||
        !GvtIsGen9 (Pci.Hdr.DeviceId))
    {
      Status = EFI_UNSUPPORTED;
    }
  }

  //
  // A child device path, if given, must be our single ACPI _ADR output.
  //
  if (!EFI_ERROR (Status) && (RemainingDevicePath != NULL) && !IsDevicePathEnd (RemainingDevicePath)) {
    if ((DevicePathType (RemainingDevicePath) != ACPI_DEVICE_PATH) ||
        (DevicePathSubType (RemainingDevicePath) != ACPI_ADR_DP))
    {
      Status = EFI_UNSUPPORTED;
    }
  }

  gBS->CloseProtocol (Controller, &gEfiPciIoProtocolGuid, This->DriverBindingHandle, Controller);
  return Status;
}

STATIC
EFI_STATUS
GvtCreateChild (
  IN GVT_DEVICE  *Dev
  )
{
  EFI_STATUS                Status;
  EFI_DEVICE_PATH_PROTOCOL  *ParentPath;
  ACPI_ADR_DEVICE_PATH      AdrNode;
  EFI_PCI_IO_PROTOCOL       *ChildPciIo;

  Status = gBS->HandleProtocol (Dev->Controller, &gEfiDevicePathProtocolGuid, (VOID **)&ParentPath);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  ZeroMem (&AdrNode, sizeof (AdrNode));
  AdrNode.Header.Type    = ACPI_DEVICE_PATH;
  AdrNode.Header.SubType = ACPI_ADR_DP;
  AdrNode.ADR            = ACPI_DISPLAY_ADR (1, 0, 0, 1, 0, ACPI_ADR_DISPLAY_TYPE_EXTERNAL_DIGITAL, 0, 0);
  SetDevicePathNodeLength (&AdrNode.Header, sizeof (AdrNode));

  Dev->DevicePath = AppendDevicePathNode (ParentPath, &AdrNode.Header);
  if (Dev->DevicePath == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Dev->EdidDiscovered.SizeOfEdid = Dev->EdidValid ? GVT_EDID_SIZE : 0;
  Dev->EdidDiscovered.Edid       = Dev->EdidValid ? Dev->Edid : NULL;
  Dev->EdidActive.SizeOfEdid     = Dev->EdidDiscovered.SizeOfEdid;
  Dev->EdidActive.Edid           = Dev->EdidDiscovered.Edid;

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Dev->Handle,
                  &gEfiDevicePathProtocolGuid,
                  Dev->DevicePath,
                  &gEfiGraphicsOutputProtocolGuid,
                  &Dev->Gop,
                  &gEfiEdidDiscoveredProtocolGuid,
                  &Dev->EdidDiscovered,
                  &gEfiEdidActiveProtocolGuid,
                  &Dev->EdidActive,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    FreePool (Dev->DevicePath);
    Dev->DevicePath = NULL;
    return Status;
  }

  //
  // Tie the child to the controller's PCI I/O.
  //
  Status = gBS->OpenProtocol (
                  Dev->Controller,
                  &gEfiPciIoProtocolGuid,
                  (VOID **)&ChildPciIo,
                  mGvtDriverBinding.DriverBindingHandle,
                  Dev->Handle,
                  EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER
                  );
  if (EFI_ERROR (Status)) {
    gBS->UninstallMultipleProtocolInterfaces (
           Dev->Handle,
           &gEfiDevicePathProtocolGuid,
           Dev->DevicePath,
           &gEfiGraphicsOutputProtocolGuid,
           &Dev->Gop,
           &gEfiEdidDiscoveredProtocolGuid,
           &Dev->EdidDiscovered,
           &gEfiEdidActiveProtocolGuid,
           &Dev->EdidActive,
           NULL
           );
    FreePool (Dev->DevicePath);
    Dev->DevicePath = NULL;
    Dev->Handle     = NULL;
  }

  return Status;
}

STATIC
EFI_STATUS
EFIAPI
GvtStart (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   Controller,
  IN EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath
  )
{
  EFI_STATUS                         Status;
  GVT_DEVICE                         *Dev;
  EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR  *Bar;
  UINT64                             Supports;

  Dev = AllocateZeroPool (sizeof (*Dev));
  if (Dev == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Dev->Signature  = GVT_GOP_SIGNATURE;
  Dev->Controller = Controller;

  Status = gBS->OpenProtocol (
                  Controller,
                  &gEfiPciIoProtocolGuid,
                  (VOID **)&Dev->PciIo,
                  This->DriverBindingHandle,
                  Controller,
                  EFI_OPEN_PROTOCOL_BY_DRIVER
                  );
  if (EFI_ERROR (Status)) {
    FreePool (Dev);
    return Status;
  }

  Status = Dev->PciIo->Attributes (Dev->PciIo, EfiPciIoAttributeOperationGet, 0, &Dev->OriginalAttributes);
  if (!EFI_ERROR (Status)) {
    Status = Dev->PciIo->Attributes (Dev->PciIo, EfiPciIoAttributeOperationSupported, 0, &Supports);
  }

  if (!EFI_ERROR (Status)) {
    Supports &= EFI_PCI_IO_ATTRIBUTE_MEMORY | EFI_PCI_IO_ATTRIBUTE_BUS_MASTER;
    Status    = Dev->PciIo->Attributes (Dev->PciIo, EfiPciIoAttributeOperationEnable, Supports, NULL);
  }

  if (EFI_ERROR (Status)) {
    goto CloseIo;
  }

  //
  // BAR 0 must be a memory BAR large enough for MMIO and the global GTT;
  // every access is bounded by its size.
  //
  Status = Dev->PciIo->GetBarAttributes (Dev->PciIo, 0, NULL, (VOID **)&Bar);
  if (EFI_ERROR (Status)) {
    goto RestoreAttributes;
  }

  if ((Bar->Desc == ACPI_ADDRESS_SPACE_DESCRIPTOR) && (Bar->ResType == ACPI_ADDRESS_SPACE_TYPE_MEM)) {
    Dev->MmioSize = Bar->AddrLen;
  }

  FreePool (Bar);

  Status = GvtProbe (Dev);
  if (EFI_ERROR (Status)) {
    goto RestoreAttributes;
  }

  GvtInstallOpRegion (Dev);

  Status = GvtReadEdid (Dev);
  DEBUG ((DEBUG_INFO, "GvtGop: EDID %r\n", Status));
  GvtBuildModeList (Dev);

  Status = GvtMapFramebuffer (Dev);
  if (EFI_ERROR (Status)) {
    goto RestoreAttributes;
  }

  Status = GvtGopInit (Dev);
  if (EFI_ERROR (Status)) {
    goto Unmap;
  }

  //
  // An end node asks us to start the controller without producing a child.
  //
  if ((RemainingDevicePath != NULL) && IsDevicePathEnd (RemainingDevicePath)) {
    return EFI_SUCCESS;
  }

  Status = GvtCreateChild (Dev);
  if (EFI_ERROR (Status)) {
    goto Disable;
  }

  return EFI_SUCCESS;

Disable:
  GvtDisableDisplay (Dev);
  GvtGopFini (Dev);
Unmap:
  GvtUnmapFramebuffer (Dev);
RestoreAttributes:
  Dev->PciIo->Attributes (Dev->PciIo, EfiPciIoAttributeOperationSet, Dev->OriginalAttributes, NULL);
CloseIo:
  gBS->CloseProtocol (Controller, &gEfiPciIoProtocolGuid, This->DriverBindingHandle, Controller);
  FreePool (Dev);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
GvtStop (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   Controller,
  IN UINTN                        NumberOfChildren,
  IN EFI_HANDLE                   *ChildHandleBuffer
  )
{
  EFI_STATUS                    Status;
  EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop;
  GVT_DEVICE                    *Dev;

  if (NumberOfChildren > 1) {
    return EFI_INVALID_PARAMETER;
  }

  if (NumberOfChildren == 0) {
    //
    // Started without a child: nothing left on the controller to find our
    // state from, so the display stays lit until the OS takes over.
    //
    return gBS->CloseProtocol (Controller, &gEfiPciIoProtocolGuid, This->DriverBindingHandle, Controller);
  }

  Status = gBS->OpenProtocol (
                  ChildHandleBuffer[0],
                  &gEfiGraphicsOutputProtocolGuid,
                  (VOID **)&Gop,
                  This->DriverBindingHandle,
                  ChildHandleBuffer[0],
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Dev = GVT_DEVICE_FROM_GOP (Gop);

  gBS->CloseProtocol (Controller, &gEfiPciIoProtocolGuid, This->DriverBindingHandle, Dev->Handle);
  Status = gBS->UninstallMultipleProtocolInterfaces (
                  Dev->Handle,
                  &gEfiDevicePathProtocolGuid,
                  Dev->DevicePath,
                  &gEfiGraphicsOutputProtocolGuid,
                  &Dev->Gop,
                  &gEfiEdidDiscoveredProtocolGuid,
                  &Dev->EdidDiscovered,
                  &gEfiEdidActiveProtocolGuid,
                  &Dev->EdidActive,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    gBS->OpenProtocol (
           Controller,
           &gEfiPciIoProtocolGuid,
           (VOID **)&Dev->PciIo,
           This->DriverBindingHandle,
           Dev->Handle,
           EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER
           );
    return Status;
  }

  GvtDisableDisplay (Dev);
  GvtGopFini (Dev);
  GvtUnmapFramebuffer (Dev);
  Dev->PciIo->Attributes (Dev->PciIo, EfiPciIoAttributeOperationSet, Dev->OriginalAttributes, NULL);
  gBS->CloseProtocol (Controller, &gEfiPciIoProtocolGuid, This->DriverBindingHandle, Controller);
  FreePool (Dev->DevicePath);
  FreePool (Dev);
  return EFI_SUCCESS;
}

STATIC EFI_DRIVER_BINDING_PROTOCOL  mGvtDriverBinding = {
  GvtSupported,
  GvtStart,
  GvtStop,
  0x10,
  NULL,
  NULL
};

//
// Component name (UEFI 2.10, section 11.5), English only.
//
STATIC EFI_UNICODE_STRING_TABLE  mDriverName[] = {
  { "en", L"LordOS GVT-g Graphics Output Driver" },
  { NULL, NULL }
};

STATIC EFI_UNICODE_STRING_TABLE  mControllerName[] = {
  { "en", L"Intel GVT-g virtual GPU" },
  { NULL, NULL }
};

STATIC
EFI_STATUS
EFIAPI
GvtGetDriverName (
  IN  EFI_COMPONENT_NAME2_PROTOCOL  *This,
  IN  CHAR8                         *Language,
  OUT CHAR16                        **DriverName
  )
{
  return LookupUnicodeString2 (Language, This->SupportedLanguages, mDriverName, DriverName, FALSE);
}

STATIC
EFI_STATUS
EFIAPI
GvtGetControllerName (
  IN  EFI_COMPONENT_NAME2_PROTOCOL  *This,
  IN  EFI_HANDLE                    ControllerHandle,
  IN  EFI_HANDLE                    ChildHandle        OPTIONAL,
  IN  CHAR8                         *Language,
  OUT CHAR16                        **ControllerName
  )
{
  EFI_STATUS  Status;

  Status = EfiTestManagedDevice (ControllerHandle, mGvtDriverBinding.DriverBindingHandle, &gEfiPciIoProtocolGuid);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return LookupUnicodeString2 (Language, This->SupportedLanguages, mControllerName, ControllerName, FALSE);
}

STATIC EFI_COMPONENT_NAME2_PROTOCOL  mComponentName2 = {
  GvtGetDriverName,
  GvtGetControllerName,
  "en"
};

EFI_STATUS
EFIAPI
GvtGopEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  return EfiLibInstallDriverBindingComponentName2 (
           ImageHandle,
           SystemTable,
           &mGvtDriverBinding,
           ImageHandle,
           NULL,
           &mComponentName2
           );
}
