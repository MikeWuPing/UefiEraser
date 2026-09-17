/** @file
  Platform/AtaErase.c - see AtaErase.h.

  Copyright (c) 2026, Mike Wu. All rights reserved.
**/

#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/DebugLib.h>
#include <Library/LvglUefiPort.h>      /* LvglTickGetMs */

#include <Protocol/AtaPassThru.h>
#include <IndustryStandard/Atapi.h>

#include "AtaErase.h"

/** Temporary user password. 32 bytes as ATA requires; the erase clears it, so
    nothing is left on the device afterwards. */
STATIC CONST UINT8 mTempPassword[32] = {
  'U', 'E', 'F', 'I', 'E', 'R', 'A', 'S',
  'E', 'R', '-', 'T', 'M', 'P', '-', 'P',
  'W', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

/** Timeout for a single ATA command (100 ns units). 30 s is generous for
    IDENTIFY/SET PASSWORD; ERASE UNIT itself gets its own, longer wait. */
#define ATA_CMD_TIMEOUT  (30ULL * 1000ULL * 1000ULL * 10ULL)

/* ------------------------------------------------------- device location */

typedef struct {
  EFI_ATA_PASS_THRU_PROTOCOL  *Ata;
  UINT16                       Port;
  UINT16                       PmPort;
} ATA_LOCATION;

/** Find the pass-thru port whose device path matches `DeviceHandle`.

    ATA pass-thru exposes its topology through GetNextPort / GetNextDevice /
    GetDevicePath rather than by handing out a handle per device, so the device
    is located by comparing paths. */
STATIC
EFI_STATUS
FindAtaLocation (
  IN  EFI_HANDLE      DeviceHandle,
  OUT ATA_LOCATION   *Loc
  )
{
  EFI_STATUS                Status;
  EFI_HANDLE               *Handles = NULL;
  UINTN                     Count = 0;
  UINTN                     H;
  EFI_DEVICE_PATH_PROTOCOL *Want = NULL;

  Status = gBS->HandleProtocol (
                  DeviceHandle,
                  &gEfiDevicePathProtocolGuid,
                  (VOID **)&Want
                  );
  if (EFI_ERROR (Status) || Want == NULL) {
    return EFI_NOT_FOUND;
  }

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiAtaPassThruProtocolGuid,
                  NULL,
                  &Count,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "[AtaErase] no ATA pass-thru instance: %r\n", Status));
    return EFI_NOT_FOUND;
  }

  for (H = 0; H < Count; H++) {
    EFI_ATA_PASS_THRU_PROTOCOL  *Ata = NULL;
    UINT16                       Port = 0xFFFF;

    Status = gBS->HandleProtocol (Handles[H], &gEfiAtaPassThruProtocolGuid,
                                  (VOID **)&Ata);
    if (EFI_ERROR (Status) || Ata == NULL) {
      continue;
    }

    while (Ata->GetNextPort (Ata, &Port) == EFI_SUCCESS) {
      UINT16  Pm = 0xFFFF;

      while (Ata->GetNextDevice (Ata, Port, &Pm) == EFI_SUCCESS) {
        EFI_DEVICE_PATH_PROTOCOL  *Dp = NULL;

        if (EFI_ERROR (Ata->BuildDevicePath (Ata, Port, Pm, &Dp)) || Dp == NULL) {
          continue;
        }
        if (GetDevicePathSize (Dp) == GetDevicePathSize (Want) &&
            CompareMem (Dp, Want, GetDevicePathSize (Dp)) == 0)
        {
          Loc->Ata    = Ata;
          Loc->Port   = Port;
          Loc->PmPort = Pm;
          FreePool (Dp);
          FreePool (Handles);
          DEBUG ((
            DEBUG_INFO,
            "[AtaErase] located device at port=%u pmport=%u\n",
            (UINT32)Port,
            (UINT32)Pm
            ));
          return EFI_SUCCESS;
        }
        FreePool (Dp);
      }
    }
  }

  FreePool (Handles);
  return EFI_NOT_FOUND;
}

/* ------------------------------------------------------------- commands */

STATIC
EFI_STATUS
AtaCommand (
  IN ATA_LOCATION              *Loc,
  IN UINT8                      Command,
  IN UINT8                      Features,
  IN UINT8                      SectorCount,
  IN UINT8                      SectorNumber,
  IN UINT8                      CylLow,
  IN UINT8                      CylHigh,
  IN UINT8                      DeviceHead,
  IN UINT8                      Protocol,
  IN UINT8                      Length,
  IN OUT VOID                  *Buffer,
  IN UINT32                     BufferBytes
  )
{
  EFI_ATA_COMMAND_BLOCK              Acb;
  EFI_ATA_STATUS_BLOCK               Asb;
  EFI_ATA_PASS_THRU_COMMAND_PACKET   Packet;

  ZeroMem (&Acb, sizeof (Acb));
  ZeroMem (&Asb, sizeof (Asb));
  ZeroMem (&Packet, sizeof (Packet));

  Acb.AtaCommand     = Command;
  Acb.AtaFeatures    = Features;
  Acb.AtaSectorCount = SectorCount;
  Acb.AtaSectorNumber = SectorNumber;
  Acb.AtaCylinderLow = CylLow;
  Acb.AtaCylinderHigh = CylHigh;
  Acb.AtaDeviceHead  = DeviceHead;

  Packet.Asb     = &Asb;
  Packet.Acb     = &Acb;
  Packet.Timeout = ATA_CMD_TIMEOUT;
  Packet.Protocol = Protocol;
  Packet.Length   = Length;

  /* Direction follows the protocol byte: DATA_OUT buffers go in
     OutDataBuffer, DATA_IN buffers come back in InDataBuffer. */
  if (Protocol == EFI_ATA_PASS_THRU_PROTOCOL_PIO_DATA_OUT ||
      Protocol == EFI_ATA_PASS_THRU_PROTOCOL_UDMA_DATA_OUT)
  {
    Packet.OutDataBuffer   = Buffer;
    Packet.OutTransferLength = BufferBytes;
  } else if (Protocol == EFI_ATA_PASS_THRU_PROTOCOL_PIO_DATA_IN ||
             Protocol == EFI_ATA_PASS_THRU_PROTOCOL_UDMA_DATA_IN)
  {
    Packet.InDataBuffer    = Buffer;
    Packet.InTransferLength = BufferBytes;
  }

  return Loc->Ata->PassThru (Loc->Ata, Loc->Port, Loc->PmPort, &Packet, NULL);
}

STATIC
EFI_STATUS
AtaIdentify (
  IN  ATA_LOCATION  *Loc,
  OUT UINT16        *Identify
  )
{
  return AtaCommand (
           Loc,
           ATA_CMD_IDENTIFY_DRIVE,
           0, 0, 0, 0, 0, 0,
           EFI_ATA_PASS_THRU_PROTOCOL_PIO_DATA_IN,
           EFI_ATA_PASS_THRU_LENGTH_BYTES | EFI_ATA_PASS_THRU_LENGTH_SECTOR_COUNT,
           Identify,
           512
           );
}

/* --------------------------------------------------------------- public */

EFI_STATUS
AtaSecurityQuery (
  IN  EFI_HANDLE          DeviceHandle,
  OUT ATA_SECURITY_INFO   *Info
  )
{
  EFI_STATUS     Status;
  ATA_LOCATION   Loc;
  UINT16        *Id = NULL;

  if (Info == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  ZeroMem (Info, sizeof (*Info));

  Status = FindAtaLocation (DeviceHandle, &Loc);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Id = AllocateZeroPool (512);
  if (Id == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  Status = AtaIdentify (&Loc, Id);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[AtaErase] IDENTIFY DEVICE failed: %r\n", Status));
    FreePool (Id);
    return Status;
  }

  /* Word 82 bit 1: the Security feature set is supported.
     Word 128: bit 1 enabled, bit 3 frozen, bit 4 locked, bit 5 enhanced erase. */
  Info->Supported = (BOOLEAN)((Id[82] & 0x0002) != 0);
  Info->Enabled   = (BOOLEAN)((Id[128] & 0x0002) != 0);
  Info->Frozen    = (BOOLEAN)((Id[128] & 0x0008) != 0);
  Info->Locked    = (BOOLEAN)((Id[128] & 0x0010) != 0);
  Info->EnhancedEraseSupported = (BOOLEAN)((Id[128] & 0x0020) != 0);
  Info->EraseTimeNormalMinutes   = Id[89];
  Info->EraseTimeEnhancedMinutes = Id[90];

  DEBUG ((
    DEBUG_INFO,
    "[AtaErase] security: supported=%d enabled=%d frozen=%d locked=%d "
    "enhanced=%d normal=%u min enhanced=%u min\n",
    Info->Supported,
    Info->Enabled,
    Info->Frozen,
    Info->Locked,
    Info->EnhancedEraseSupported,
    (UINT32)Info->EraseTimeNormalMinutes,
    (UINT32)Info->EraseTimeEnhancedMinutes
    ));

  FreePool (Id);
  return EFI_SUCCESS;
}

/** Wait for the device to stop being busy, by retrying IDENTIFY.

    A Secure Erase can take minutes, and the device stops answering while it
    runs, so the wait is bounded by the drive's own reported time plus a margin
    rather than by a fixed guess. */
STATIC
EFI_STATUS
WaitReady (
  IN ATA_LOCATION  *Loc,
  IN UINT32         MaxSeconds
  )
{
  UINT16   *Id = AllocateZeroPool (512);
  UINT64    Start = LvglTickGetMs ();
  EFI_STATUS Status = EFI_TIMEOUT;

  if (Id == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  for (;;) {
    Status = AtaIdentify (Loc, Id);
    if (!EFI_ERROR (Status)) {
      break;
    }
    if (LvglTickGetMs () - Start > (UINT64)MaxSeconds * 1000ULL) {
      DEBUG ((DEBUG_WARN, "[AtaErase] device still not ready after %u s\n",
              (UINT32)MaxSeconds));
      break;
    }
    gBS->Stall (500 * 1000);        /* 500 ms between probes */
  }

  FreePool (Id);
  return Status;
}

EFI_STATUS
AtaSecureErase (
  IN  EFI_HANDLE      DeviceHandle,
  IN  BOOLEAN         Enhanced,
  IN  ERASE_PUMP_CB   Pump,
  IN  VOID           *PumpCtx,
  OUT ERASE_REPORT   *Report
  )
{
  EFI_STATUS         Status;
  ATA_LOCATION       Loc;
  ATA_SECURITY_INFO  Info;
  UINT8             *Payload = NULL;
  UINT32             MaxSeconds;
  BOOLEAN            UseEnhanced = Enhanced;

  if (Report == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  ZeroMem (Report, sizeof (*Report));
  Report->AlgoId = "ata-secure-erase";
  Report->PassCount = 1;

  Status = FindAtaLocation (DeviceHandle, &Loc);
  if (EFI_ERROR (Status)) {
    Report->Status = ERASE_ERR_IO;
    Report->Reason = "not an ATA disk: no ATA Pass-Thru protocol covers it "
                     "(a virtual disk or a USB bridge looks like this)";
    return EFI_UNSUPPORTED;
  }

  Status = AtaSecurityQuery (DeviceHandle, &Info);
  if (EFI_ERROR (Status)) {
    Report->Status = ERASE_ERR_IO;
    Report->Reason = "the IDENTIFY DEVICE command failed - the disk did not "
                     "answer the ATA pass-through";
    return Status;
  }

  if (!Info.Supported) {
    DEBUG ((DEBUG_ERROR, "[AtaErase] device does not support the Security "
                         "feature set\n"));
    Report->Status = ERASE_ERR_IO;
    Report->Reason = "the disk reports no ATA Security feature set, so Secure "
                     "Erase/Erase Unit is not implemented by its firmware";
    return EFI_UNSUPPORTED;
  }
  if (Info.Frozen) {
    /* Deliberately NOT downgraded to an overwrite: the user asked for an erase
       that works on their media, and silently doing something else would be a
       false claim of success. */
    DEBUG ((DEBUG_ERROR, "[AtaErase] device is FROZEN - SECURITY ERASE UNIT "
                         "will be refused. Power-cycle the machine and retry.\n"));
    Report->Status = ERASE_ERR_IO;
    Report->Reason = "the disk is FROZEN: the firmware locked the Security "
                     "feature set. Power-cycle the machine and retry";
    return EFI_ACCESS_DENIED;
  }
  if (UseEnhanced && !Info.EnhancedEraseSupported) {
    DEBUG ((DEBUG_WARN, "[AtaErase] Enhanced erase not supported; using Normal\n"));
    UseEnhanced = FALSE;
  }

  Payload = AllocateZeroPool (512);
  if (Payload == NULL) {
    Report->Status = ERASE_ERR_IO;
    return EFI_OUT_OF_RESOURCES;
  }

  /* ---- 1. SECURITY SET PASSWORD (user, high) ---- */
  CopyMem (Payload, mTempPassword, 32);
  Status = AtaCommand (
             &Loc,
             ATA_CMD_SECURITY_SET_PASSWORD,
             0x01,                    /* Features bit 0: USER password */
             0, 0, 0, 0, 0,
             EFI_ATA_PASS_THRU_PROTOCOL_PIO_DATA_OUT,
             EFI_ATA_PASS_THRU_LENGTH_BYTES | EFI_ATA_PASS_THRU_LENGTH_SECTOR_COUNT,
             Payload,
             512
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[AtaErase] SECURITY SET PASSWORD failed: %r\n", Status));
    Report->Status = ERASE_ERR_IO;
    FreePool (Payload);
    return Status;
  }
  DEBUG ((DEBUG_INFO, "[AtaErase] SECURITY SET PASSWORD ok\n"));

  /* ---- 2. SECURITY ERASE PREPARE ---- */
  Status = AtaCommand (
             &Loc,
             ATA_CMD_SECURITY_ERASE_PREPARE,
             0, 0, 0, 0, 0, 0,
             EFI_ATA_PASS_THRU_PROTOCOL_ATA_NON_DATA,
             EFI_ATA_PASS_THRU_LENGTH_NO_DATA_TRANSFER,
             NULL,
             0
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[AtaErase] SECURITY ERASE PREPARE failed: %r\n", Status));
    Report->Status = ERASE_ERR_IO;
    FreePool (Payload);
    return Status;
  }
  DEBUG ((DEBUG_INFO, "[AtaErase] SECURITY ERASE PREPARE ok\n"));

  /* ---- 3. SECURITY ERASE UNIT ---- */
  CopyMem (Payload, mTempPassword, 32);
  if (UseEnhanced) {
    Payload[0] |= 0x02;               /* word 0 bit 1: Enhanced erase */
  }

  DEBUG ((
    DEBUG_INFO,
    "[AtaErase] SECURITY ERASE UNIT (%a) - this can take %u minutes; the "
    "device will be unresponsive until it finishes\n",
    UseEnhanced ? "enhanced" : "normal",
    (UINT32)(UseEnhanced ? Info.EraseTimeEnhancedMinutes
                         : Info.EraseTimeNormalMinutes)
    ));

  Status = AtaCommand (
             &Loc,
             ATA_CMD_SECURITY_ERASE_UNIT,
             0,
             0, 0, 0, 0, 0,
             EFI_ATA_PASS_THRU_PROTOCOL_PIO_DATA_OUT,
             EFI_ATA_PASS_THRU_LENGTH_BYTES | EFI_ATA_PASS_THRU_LENGTH_SECTOR_COUNT,
             Payload,
             512
             );
  FreePool (Payload);

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[AtaErase] SECURITY ERASE UNIT failed: %r\n", Status));
    Report->Status = ERASE_ERR_IO;
    return Status;
  }

  /* ---- 4. wait for the drive to come back ---- */
  MaxSeconds = (UINT32)(UseEnhanced ? Info.EraseTimeEnhancedMinutes
                                    : Info.EraseTimeNormalMinutes);
  if (MaxSeconds < 10) {
    MaxSeconds = 10;                  /* a device reporting 0 is just silent */
  }
  MaxSeconds += 60;                   /* margin */

  if (Pump != NULL) {
    Pump (PumpCtx);
  }
  Status = WaitReady (&Loc, MaxSeconds);
  if (EFI_ERROR (Status)) {
    Report->Status = ERASE_ERR_IO;
    return EFI_TIMEOUT;
  }

  DEBUG ((DEBUG_INFO, "[AtaErase] device ready; erase complete\n"));

  Report->PassesCompleted   = 1;
  Report->TotalBytesPlanned = 0;      /* no host-side writes: the drive did it */
  Report->Status            = ERASE_OK;
  return EFI_SUCCESS;
}
