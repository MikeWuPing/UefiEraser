/** @file
  Platform/NvmeErase.c - see NvmeErase.h.

  Copyright (c) 2026, Mike Wu. All rights reserved.
**/

#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/DebugLib.h>
#include <Library/LvglUefiPort.h>

#include <Protocol/NvmExpressPassthru.h>

#include "NvmeErase.h"

/* Admin opcodes (NVMe base spec). */
#define NVME_ADMIN_IDENTIFY      0x06u
#define NVME_ADMIN_GET_LOG_PAGE  0x02u
#define NVME_ADMIN_FORMAT_NVM    0x80u
#define NVME_ADMIN_SANITIZE      0x84u

/* Identify CNS values. */
#define NVME_CNS_CONTROLLER      0x01u

/* Log page identifiers. */
#define NVME_LOG_SANITIZE_STATUS 0x81u

/** Identify Controller is 4096 bytes. */
#define NVME_IDENTIFY_BYTES      4096u

/** Offset of SANICAP inside Identify Controller. */
#define NVME_IDENTIFY_SANICAP_OFF  328u
#define NVME_IDENTIFY_MODEL_OFF     24u    /* 40 bytes ASCII */
#define NVME_IDENTIFY_SERIAL_OFF     4u    /* 20 bytes ASCII */

/** Timeout for one admin command (100 ns units) - 30 s. Format and Sanitize
    are ASYNCHRONOUS: they return immediately and the controller works in the
    background, which is why progress comes from the log page. */
#define NVME_CMD_TIMEOUT  (30ULL * 1000ULL * 1000ULL * 10ULL)

typedef struct {
  EFI_NVM_EXPRESS_PASS_THRU_PROTOCOL  *Nvme;
  UINT32                               Nsid;
} NVME_LOCATION;

/* ------------------------------------------------------- device location */

STATIC
EFI_STATUS
FindNvmeLocation (
  IN  EFI_HANDLE     DeviceHandle,
  OUT NVME_LOCATION *Loc
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
                  &gEfiNvmExpressPassThruProtocolGuid,
                  NULL,
                  &Count,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "[NvmeErase] no NVMe pass-thru instance: %r\n", Status));
    return EFI_NOT_FOUND;
  }

  for (H = 0; H < Count; H++) {
    EFI_NVM_EXPRESS_PASS_THRU_PROTOCOL  *Nvme = NULL;
    UINT32                               Nsid = 0xFFFFFFFFu;

    Status = gBS->HandleProtocol (Handles[H], &gEfiNvmExpressPassThruProtocolGuid,
                                  (VOID **)&Nvme);
    if (EFI_ERROR (Status) || Nvme == NULL) {
      continue;
    }

    while (Nvme->GetNextNamespace (Nvme, &Nsid) == EFI_SUCCESS) {
      EFI_DEVICE_PATH_PROTOCOL  *Dp = NULL;

      if (EFI_ERROR (Nvme->BuildDevicePath (Nvme, Nsid, &Dp)) || Dp == NULL) {
        continue;
      }
      if (GetDevicePathSize (Dp) == GetDevicePathSize (Want) &&
          CompareMem (Dp, Want, GetDevicePathSize (Dp)) == 0)
      {
        Loc->Nvme = Nvme;
        Loc->Nsid = Nsid;
        FreePool (Dp);
        FreePool (Handles);
        DEBUG ((DEBUG_INFO, "[NvmeErase] located namespace %u\n", (UINT32)Nsid));
        return EFI_SUCCESS;
      }
      FreePool (Dp);
    }
  }

  FreePool (Handles);
  return EFI_NOT_FOUND;
}

/* ------------------------------------------------------------- commands */

STATIC
EFI_STATUS
NvmeAdmin (
  IN NVME_LOCATION             *Loc,
  IN UINT32                     NamespaceId,
  IN UINT8                      Opcode,
  IN UINT32                     Cdw10,
  IN UINT32                     Cdw11,
  IN OUT VOID                  *Buffer,
  IN UINT32                     BufferBytes,
  OUT EFI_NVM_EXPRESS_COMPLETION *Comp
  )
{
  EFI_NVM_EXPRESS_COMMAND             Cmd;
  EFI_NVM_EXPRESS_COMPLETION          Local;
  EFI_NVM_EXPRESS_PASS_THRU_COMMAND_PACKET  Packet;

  ZeroMem (&Cmd, sizeof (Cmd));
  ZeroMem (&Local, sizeof (Local));
  ZeroMem (&Packet, sizeof (Packet));

  Cmd.Cdw0.Opcode = Opcode;
  Cmd.Cdw10       = Cdw10;
  Cmd.Cdw11       = Cdw11;

  Packet.CommandTimeout = NVME_CMD_TIMEOUT;
  Packet.QueueType      = NVME_ADMIN_QUEUE;
  Packet.NvmeCmd        = &Cmd;
  Packet.NvmeCompletion = &Local;

  if (Buffer != NULL && BufferBytes != 0) {
    Packet.TransferBuffer = Buffer;
    Packet.TransferLength = BufferBytes;
  }

  {
    EFI_STATUS Status = Loc->Nvme->PassThru (Loc->Nvme, NamespaceId, &Packet, NULL);
    if (Comp != NULL) {
      *Comp = Local;
    }
    return Status;
  }
}

/** Identify Controller (CNS 1) - whole device, so NamespaceId is the
    controller-wide value. */
STATIC
EFI_STATUS
NvmeIdentifyController (
  IN  NVME_LOCATION  *Loc,
  OUT UINT8          *Buf
  )
{
  return NvmeAdmin (
           Loc,
           0xFFFFFFFFu,               /* controller-wide */
           NVME_ADMIN_IDENTIFY,
           (UINT32)(NVME_CNS_CONTROLLER << 16),
           0,
           Buf,
           NVME_IDENTIFY_BYTES,
           NULL
           );
}

STATIC
VOID
CopyAsciiTrimmed (
  IN  CONST UINT8  *Src,
  IN  UINTN         SrcLen,
  OUT CHAR8        *Dst,
  IN  UINTN         DstBytes
  )
{
  UINTN Len = SrcLen;
  UINTN I;

  while (Len > 0 && Src[Len - 1] == ' ') {
    Len--;
  }
  if (Len > DstBytes - 1) {
    Len = DstBytes - 1;
  }
  for (I = 0; I < Len; I++) {
    CHAR8 C = (CHAR8)Src[I];
    Dst[I] = (C >= 0x20 && C < 0x7F) ? C : '?';
  }
  Dst[Len] = 0;
}

/* --------------------------------------------------------------- public */

EFI_STATUS
NvmeEraseQuery (
  IN  EFI_HANDLE          DeviceHandle,
  OUT NVME_ERASE_CAPS     *Caps
  )
{
  EFI_STATUS     Status;
  NVME_LOCATION  Loc;
  UINT8         *Id = NULL;

  if (Caps == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  ZeroMem (Caps, sizeof (*Caps));

  Status = FindNvmeLocation (DeviceHandle, &Loc);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Id = AllocateZeroPool (NVME_IDENTIFY_BYTES);
  if (Id == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  Status = NvmeIdentifyController (&Loc, Id);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[NvmeErase] Identify Controller failed: %r\n", Status));
    FreePool (Id);
    return Status;
  }

  /* SANICAP: bit 0 = crypto erase, bit 1 = block erase, bit 2 = overwrite,
     bit 3 = no-deallocate behaviour. */
  Caps->Sanicap              = *(UINT32 *)(Id + NVME_IDENTIFY_SANICAP_OFF);
  Caps->SanitizeCryptoErase  = (BOOLEAN)((Caps->Sanicap & 0x1) != 0);
  Caps->SanitizeBlockErase   = (BOOLEAN)((Caps->Sanicap & 0x2) != 0);
  Caps->SanitizeOverwrite    = (BOOLEAN)((Caps->Sanicap & 0x4) != 0);
  Caps->NoDeallocateInSanitize = (BOOLEAN)((Caps->Sanicap & 0x8) != 0);

  /* Format NVM's SES support is advertised in the OACS field (Identify
     Controller byte 256, bit 1 = Format NVM supported). Crypto Erase support
     for Format is implied by the FNA field's crypto-erase bit. */
  {
    UINT16 Oacs = *(UINT16 *)(Id + 256);
    UINT8  Fna  = Id[260];

    Caps->FormatSesSupported = 0;
    if ((Oacs & 0x0002) != 0) {
      Caps->FormatSesSupported |= (1u << NVME_SES_USER_DATA_ERASE);
      if ((Fna & 0x01) != 0) {
        Caps->FormatSesSupported |= (1u << NVME_SES_CRYPTO_ERASE);
      }
    }
  }

  CopyAsciiTrimmed (Id + NVME_IDENTIFY_SERIAL_OFF, 20, Caps->Serial,
                    sizeof (Caps->Serial));
  CopyAsciiTrimmed (Id + NVME_IDENTIFY_MODEL_OFF, 40, Caps->Model,
                    sizeof (Caps->Model));

  DEBUG ((
    DEBUG_INFO,
    "[NvmeErase] model='%a' serial='%a' SANICAP=0x%x (block=%d overwrite=%d "
    "crypto=%d) formatSes=0x%x\n",
    Caps->Model,
    Caps->Serial,
    (UINT32)Caps->Sanicap,
    Caps->SanitizeBlockErase,
    Caps->SanitizeOverwrite,
    Caps->SanitizeCryptoErase,
    (UINT32)Caps->FormatSesSupported
    ));

  FreePool (Id);
  return EFI_SUCCESS;
}

EFI_STATUS
NvmeFormat (
  IN  EFI_HANDLE      DeviceHandle,
  IN  UINT8           Ses,
  IN  ERASE_PUMP_CB   Pump,
  IN  VOID           *PumpCtx,
  OUT ERASE_REPORT   *Report
  )
{
  EFI_STATUS                  Status;
  NVME_LOCATION               Loc;
  EFI_NVM_EXPRESS_COMPLETION  Comp;

  if (Report == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  ZeroMem (Report, sizeof (*Report));
  Report->AlgoId    = "nvme-format";
  Report->PassCount = 1;

  Status = FindNvmeLocation (DeviceHandle, &Loc);
  if (EFI_ERROR (Status)) {
    Report->Status = ERASE_ERR_IO;
    Report->Reason = "not an NVMe drive: no NVM Express Pass-Thru protocol "
                     "covers it";
    return EFI_UNSUPPORTED;
  }

  /* CDW10: LBAF (bits 3:0) | MSET (7:4) | PI (10:8) | PIL (11) | SES (13:12).
     LBAF 0 = the namespace's first supported LBA format, MSET 0 = no metadata,
     PI 0 = no protection information. */
  DEBUG ((
    DEBUG_INFO,
    "[NvmeErase] FORMAT NVM nsid=%u SES=%u (%a)\n",
    (UINT32)Loc.Nsid,
    (UINT32)Ses,
    (Ses == NVME_SES_CRYPTO_ERASE) ? "cryptographic erase" : "user data erase"
    ));

  Status = NvmeAdmin (
             &Loc,
             Loc.Nsid,
             NVME_ADMIN_FORMAT_NVM,
             (UINT32)(Ses << 12),
             0,
             NULL,
             0,
             &Comp
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[NvmeErase] Format NVM failed: %r\n", Status));
    Report->Status = ERASE_ERR_IO;
    return Status;
  }

  /* Format is synchronous from the host's point of view: it completes before
     the command returns, unlike Sanitize. */
  if (Pump != NULL) {
    Pump (PumpCtx);
  }

  DEBUG ((DEBUG_INFO, "[NvmeErase] Format NVM complete\n"));
  Report->PassesCompleted = 1;
  Report->Status          = ERASE_OK;
  return EFI_SUCCESS;
}

/** Read the Sanitize Status log (LID 0x81). SPROG is at bytes 3:0, in units of
    1/65536 of the operation. */
STATIC
EFI_STATUS
NvmeSanitizeProgress (
  IN  NVME_LOCATION  *Loc,
  OUT UINT32         *Sprog,
  OUT UINT8          *Status
  )
{
  UINT8  Log[64];

  ZeroMem (Log, sizeof (Log));
  /* CDW10: LID (bits 7:0) | NUMDL (bits 31:16) - request 64 bytes (NUMDL = 0
     means one dword; ask for 15 extra). */
  {
    EFI_STATUS Status2 = NvmeAdmin (
                           Loc,
                           0xFFFFFFFFu,
                           NVME_ADMIN_GET_LOG_PAGE,
                           (UINT32)NVME_LOG_SANITIZE_STATUS | (15u << 16),
                           0,
                           Log,
                           sizeof (Log),
                           NULL
                           );
    if (EFI_ERROR (Status2)) {
      return Status2;
    }
  }

  *Sprog  = *(UINT32 *)&Log[0];
  *Status = Log[4];
  return EFI_SUCCESS;
}

EFI_STATUS
NvmeSanitize (
  IN  EFI_HANDLE      DeviceHandle,
  IN  UINT8           Action,
  IN  ERASE_PUMP_CB   Pump,
  IN  VOID           *PumpCtx,
  OUT ERASE_REPORT   *Report
  )
{
  EFI_STATUS     Status;
  NVME_LOCATION  Loc;
  UINT32         Sprog = 0;
  UINT8          Sstat = 0;
  UINT64         StartMs;
  UINT32         LastPercent = 0xFFFFFFFFu;
  BOOLEAN        Supported = FALSE;

  if (Report == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  ZeroMem (Report, sizeof (*Report));
  Report->AlgoId    = "nvme-sanitize";
  Report->PassCount = 1;

  Status = FindNvmeLocation (DeviceHandle, &Loc);
  if (EFI_ERROR (Status)) {
    Report->Status = ERASE_ERR_IO;
    Report->Reason = "not an NVMe drive: no NVM Express Pass-Thru protocol "
                     "covers it";
    return EFI_UNSUPPORTED;
  }

  switch (Action) {
    case NVME_SANACT_BLOCK_ERASE:  Supported = TRUE; break;
    case NVME_SANACT_OVERWRITE:    Supported = TRUE; break;
    case NVME_SANACT_CRYPTO_ERASE: Supported = TRUE; break;
    default:
      DEBUG ((DEBUG_ERROR, "[NvmeErase] unknown sanitize action %u\n",
              (UINT32)Action));
      Report->Status = ERASE_ERR_PARAM;
      return EFI_INVALID_PARAMETER;
  }
  (void)Supported;

  DEBUG ((
    DEBUG_INFO,
    "[NvmeErase] SANITIZE SANACT=%u (%a) - asynchronous, progress from log 0x81\n",
    (UINT32)Action,
    (Action == NVME_SANACT_CRYPTO_ERASE) ? "crypto erase" :
    (Action == NVME_SANACT_OVERWRITE)    ? "overwrite"    : "block erase"
    ));

  /* CDW10: SANACT (bits 2:0) | AUSE (bit 9). AUSE 0 = the controller may
     deallocate, which is what makes block erase fast. */
  Status = NvmeAdmin (
             &Loc,
             0xFFFFFFFFu,              /* device-wide */
             NVME_ADMIN_SANITIZE,
             (UINT32)Action,
             0,
             NULL,
             0,
             NULL
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[NvmeErase] Sanitize command rejected: %r\n", Status));
    Report->Status = ERASE_ERR_IO;
    return Status;
  }

  StartMs = LvglTickGetMs ();

  for (;;) {
    UINT32 Percent;

    if (EFI_ERROR (NvmeSanitizeProgress (&Loc, &Sprog, &Sstat))) {
      /* A device that does not implement the log page: fall back to waiting a
         bounded time rather than pretending to know the progress. */
      DEBUG ((DEBUG_WARN, "[NvmeErase] Sanitize Status log unavailable; "
                          "waiting 60 s\n"));
      gBS->Stall (60 * 1000 * 1000);
      break;
    }

    Percent = (UINT32)(((UINT64)Sprog * 100) / 65536ULL);
    if (Percent != LastPercent) {
      LastPercent = Percent;
      Report->TotalBytesPlanned = 100;
      Report->BytesWritten      = Percent;
      DEBUG ((DEBUG_INFO, "[NvmeErase] sanitize progress %u%% (SSTAT=%u)\n",
              Percent, (UINT32)Sstat));
      if (Pump != NULL) {
        Pump (PumpCtx);
      }
    }

    if (Percent >= 100 || Sstat == 0x01 || Sstat == 0x02) {
      /* 0x01 = completed successfully, 0x02 = failed. */
      break;
    }
    if (LvglTickGetMs () - StartMs > 30ULL * 60ULL * 1000ULL) {
      DEBUG ((DEBUG_WARN, "[NvmeErase] sanitize exceeded 30 minutes\n"));
      Report->Status = ERASE_ERR_IO;
      return EFI_TIMEOUT;
    }
    gBS->Stall (500 * 1000);
  }

  DEBUG ((DEBUG_INFO, "[NvmeErase] Sanitize complete (SSTAT=%u)\n", (UINT32)Sstat));

  Report->PassesCompleted = 1;
  Report->Status          = (Sstat == 0x02) ? ERASE_ERR_IO : ERASE_OK;
  return (Sstat == 0x02) ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}
