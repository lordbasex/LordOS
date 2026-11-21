## @file
#  Builds the LordOS GVT-g GOP driver for X64 (see firmware/gvt-gop/README.md).
#
#  Copyright (c) 2026 iPERFEX and the LordOS contributors
#  SPDX-License-Identifier: MIT
##

[Defines]
  PLATFORM_NAME                  = GvtGopPkg
  PLATFORM_GUID                  = 08ff0b22-0179-4f43-8ff5-6c7f4310c6dc
  PLATFORM_VERSION               = 1.0
  DSC_SPECIFICATION              = 0x0001001C
  OUTPUT_DIRECTORY               = Build/GvtGopPkg
  SUPPORTED_ARCHITECTURES        = X64
  BUILD_TARGETS                  = DEBUG|RELEASE
  SKUID_IDENTIFIER               = DEFAULT

[LibraryClasses]
  !include MdePkg/MdeLibs.dsc.inc
  BaseLib|MdePkg/Library/BaseLib/BaseLib.inf
  BaseMemoryLib|MdePkg/Library/BaseMemoryLibRepStr/BaseMemoryLibRepStr.inf
  CacheMaintenanceLib|MdePkg/Library/BaseCacheMaintenanceLib/BaseCacheMaintenanceLib.inf
  DevicePathLib|MdePkg/Library/UefiDevicePathLib/UefiDevicePathLib.inf
  FrameBufferBltLib|MdeModulePkg/Library/FrameBufferBltLib/FrameBufferBltLib.inf
  IoLib|MdePkg/Library/BaseIoLibIntrinsic/BaseIoLibIntrinsic.inf
  MemoryAllocationLib|MdePkg/Library/UefiMemoryAllocationLib/UefiMemoryAllocationLib.inf
  PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf
  PrintLib|MdePkg/Library/BasePrintLib/BasePrintLib.inf
  UefiBootServicesTableLib|MdePkg/Library/UefiBootServicesTableLib/UefiBootServicesTableLib.inf
  UefiDriverEntryPoint|MdePkg/Library/UefiDriverEntryPoint/UefiDriverEntryPoint.inf
  UefiLib|MdePkg/Library/UefiLib/UefiLib.inf
  UefiRuntimeServicesTableLib|MdePkg/Library/UefiRuntimeServicesTableLib/UefiRuntimeServicesTableLib.inf
  DebugPrintErrorLevelLib|MdePkg/Library/BaseDebugPrintErrorLevelLib/BaseDebugPrintErrorLevelLib.inf
!if $(TARGET) == RELEASE
  DebugLib|MdePkg/Library/BaseDebugLibNull/BaseDebugLibNull.inf
!else
  # QEMU's debug console (I/O port 0x402): -debugcon file:debug.log -global isa-debugcon.iobase=0x402
  DebugLib|OvmfPkg/Library/PlatformDebugLibIoPort/PlatformDebugLibIoPort.inf
!endif

[PcdsFixedAtBuild]
  gEfiMdePkgTokenSpaceGuid.PcdDebugPropertyMask|0x07
  gEfiMdePkgTokenSpaceGuid.PcdDebugPrintErrorLevel|0x80000046
  gEfiMdePkgTokenSpaceGuid.PcdFixedDebugPrintErrorLevel|0x80000046

[Components]
  GvtGopPkg/GvtGopDxe/GvtGopDxe.inf

[BuildOptions]
  GCC:*_*_*_CC_FLAGS = -Werror
