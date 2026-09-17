## @file
# EraserPkg platform description.
#
# Builds UefiEraser.efi standalone against the stock edk2 MdePkg and the
# sibling LvglPkg; the edk2 tree itself is not modified. X64 only.
#
# DebugLib is mapped to BaseDebugLibSerialPort so QEMU "-serial file:"
# captures DEBUG() output directly (the version/assertion channel).
# DebugPrintErrorLevelLib is the local fixed-mask instance: PcdLib here is
# BasePcdLibNull, whose stock DebugPrintErrorLevelLib would return 0 and
# silently kill the serial log.
# No TimerLib mapping on purpose: the LVGL tick source is TSC-based.
#
# Copyright (c) 2026, Mike Wu. All rights reserved.
#
##

[Defines]
  PLATFORM_NAME                  = EraserPkg
  PLATFORM_GUID                  = 7C1A5E93-4B68-42D7-9F03-5A8E2C6B1D74
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010005
  OUTPUT_DIRECTORY               = Build/EraserPkg
  SUPPORTED_ARCHITECTURES        = X64
  BUILD_TARGETS                  = DEBUG|RELEASE|NOOPT
  SKUID_IDENTIFIER               = DEFAULT

[LibraryClasses]
  BaseLib|MdePkg/Library/BaseLib/BaseLib.inf
  BaseMemoryLib|MdePkg/Library/BaseMemoryLib/BaseMemoryLib.inf
  UefiApplicationEntryPoint|MdePkg/Library/UefiApplicationEntryPoint/UefiApplicationEntryPoint.inf
  UefiLib|MdePkg/Library/UefiLib/UefiLib.inf
  UefiBootServicesTableLib|MdePkg/Library/UefiBootServicesTableLib/UefiBootServicesTableLib.inf
  UefiRuntimeServicesTableLib|MdePkg/Library/UefiRuntimeServicesTableLib/UefiRuntimeServicesTableLib.inf
  MemoryAllocationLib|MdePkg/Library/UefiMemoryAllocationLib/UefiMemoryAllocationLib.inf
  DebugLib|MdePkg/Library/BaseDebugLibSerialPort/BaseDebugLibSerialPort.inf
  DebugPrintErrorLevelLib|EraserPkg/Library/FixedDebugPrintErrorLevelLib/FixedDebugPrintErrorLevelLib.inf
  SerialPortLib|PcAtChipsetPkg/Library/SerialIoLib/SerialIoLib.inf
  IoLib|MdePkg/Library/BaseIoLibIntrinsic/BaseIoLibIntrinsic.inf
  RegisterFilterLib|MdePkg/Library/RegisterFilterLibNull/RegisterFilterLibNull.inf
  StackCheckLib|MdePkg/Library/StackCheckLibNull/StackCheckLibNull.inf
  CompilerIntrinsicsLib|MdePkg/Library/CompilerIntrinsicsLib/CompilerIntrinsicsLib.inf
  PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf
  PrintLib|MdePkg/Library/BasePrintLib/BasePrintLib.inf
  DevicePathLib|MdePkg/Library/UefiDevicePathLib/UefiDevicePathLib.inf
  # LvglUefiPort.inf declares LvglLib as its own dependency, so the consumer
  # platform DSC must carry the LvglLib mapping too - without it the final
  # link of this application fails with unresolved lv_* symbols.
  LvglLib|LvglPkg/Library/LvglLib/LvglLib.inf
  LvglUefiPort|LvglPkg/Library/LvglUefiPort/LvglUefiPort.inf
  # LvglUefiPort's UsbHidMouse.c issues HID class requests, so UefiUsbLib is
  # consumed by that library's INF and must be mapped here (MdePkg instance:
  # no new package dependency; its PcdUsbTransferTimeoutValue is FixedAtBuild
  # and resolves through the BasePcdLibNull mapping above).
  UefiUsbLib|MdePkg/Library/UefiUsbLib/UefiUsbLib.inf

[PcdsFixedAtBuild]
  gEfiMdePkgTokenSpaceGuid.PcdDebugPropertyMask|0x03
  gEfiMdePkgTokenSpaceGuid.PcdMaximumAsciiStringLength|1000000
  gEfiMdePkgTokenSpaceGuid.PcdMaximumUnicodeStringLength|1000000
  gEfiMdePkgTokenSpaceGuid.PcdDebugClearMemoryValue|0xAF
  gEfiMdePkgTokenSpaceGuid.PcdFixedDebugPrintErrorLevel|0xFFFFFFFF

[Components]
  EraserPkg/Application/UefiEraser/UefiEraser.inf

[BuildOptions]
  # lv_conf_internal.h defaults the non-stock simsun fonts to 0 (they are
  # only enabled via CONFIG_LV_FONT_SIMSUN_*_CJK). Defining the macro here
  # makes LvglLib compile lv_font_simsun_16_cjk.c (the Chinese UI font).
  # The macro is inert for every other module: only lvgl sources read it in
  # `#ifndef LV_FONT_SIMSUN_16_CJK`.
  MSFT:*_*_*_CC_FLAGS = /DLV_FONT_SIMSUN_16_CJK=1
