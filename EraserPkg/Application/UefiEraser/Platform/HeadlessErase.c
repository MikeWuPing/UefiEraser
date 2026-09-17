/** @file
  Platform/HeadlessErase.c - see HeadlessErase.h.

  Copyright (c) 2026, Mike Wu. All rights reserved.
**/

#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>     /* GetDevicePathSize */
#include <Library/PrintLib.h>          /* UnicodeSPrint */
#include <Library/LvglUefiPort.h>      /* LvglTickGetMs */
#include <Library/DebugLib.h>

#include <Protocol/ShellParameters.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/DevicePath.h>
#include <Guid/FileInfo.h>

#include "HeadlessErase.h"
#include "../Core/EraseAlgo.h"
#include "../Core/EraseEngine.h"
#include "OverwriteErase.h"
#include "FreeSpaceWipe.h"
#include "AtaErase.h"
#include "NvmeErase.h"
#include "LogPersist.h"

/** Which operation the `erase=` selector asked for. */
typedef enum {
  MODE_OVERWRITE = 0,   /**< multi-pass overwrite of the whole target */
  MODE_FREESPACE,       /**< fill-and-delete the volume's free space */
  MODE_ATA,             /**< ATA SECURITY ERASE UNIT */
  MODE_NVME_FORMAT,     /**< NVMe Format NVM */
  MODE_NVME_SANITIZE    /**< NVMe Sanitize */
} ERASE_MODE;

/** Map ERASE_STATUS to a short result string for the audit log. */
STATIC
CONST CHAR8 *
ResultFromStatus (
  IN ERASE_STATUS  Status,
  IN BOOLEAN       Cancelled
  )
{
  if (Cancelled) {
    return "CANCELLED";
  }
  switch (Status) {
    case ERASE_OK:            return "OK";
    case ERASE_ERR_VERIFY:    return "VERIFY_FAIL";
    case ERASE_ERR_IO:        return "IO_ERROR";
    case ERASE_ERR_PARAM:     return "PARAM_ERROR";
    case ERASE_ERR_CANCELLED: return "CANCELLED";
    case ERASE_ERR_MEDIA_CHANGED: return "MEDIA_CHANGED";
    default:                  return "UNKNOWN";
  }
}

/** Build a target name string for the audit log.
    DiskIdx is the enumeration index the `erase=` selector used; Dev only knows
    its own partition number, so without it every whole disk would log as
    "disk:0" and every partition as "part:<n>:<n>". */
STATIC
VOID
BuildTargetName (
  IN  BLOCK_DEV  *Dev,
  IN  ERASE_MODE  Mode,
  IN  UINT32      DiskIdx,
  OUT CHAR16     *Out,
  IN  UINTN       OutChars
  )
{
  if (Mode == MODE_FREESPACE) {
    UnicodeSPrint (Out, OutChars, L"freespace:%u:%u",
                   DiskIdx, (UINT32)Dev->PartitionNo);
  } else if (Dev->LogicalPartition) {
    UnicodeSPrint (Out, OutChars, L"part:%u:%u",
                   DiskIdx, (UINT32)Dev->PartitionNo);
  } else {
    UnicodeSPrint (Out, OutChars, L"disk:%u", DiskIdx);
  }
}

/** Write both audit artifacts of one completed target: the CSV record and the
    human-readable report. Best-effort by design - neither may turn a completed
    erasure into an error, and neither may change the returned status. */
STATIC
VOID
PersistRun (
  IN BLOCK_DEV           *Dev,
  IN ERASE_MODE           Mode,
  IN UINT32               DiskIdx,
  IN CONST CHAR8         *ModeStr,
  IN CONST CHAR8         *AlgoId,
  IN UINT32               Passes,
  IN EFI_STATUS           Status,
  IN CONST ERASE_REPORT  *Rep,
  IN BOOLEAN              Verify
  )
{
  CHAR16                Name[64];
  ERASE_REPORT_SUMMARY  S;

  BuildTargetName (Dev, Mode, DiskIdx, Name, sizeof (Name));
  LogPersistRecord (
    Name,
    ModeStr,
    AlgoId,
    Passes,
    ResultFromStatus (Rep->Status, Rep->Cancelled),
    Rep->BytesWritten
    );

  S.ModeName     = ModeStr;
  S.AlgoId       = AlgoId;
  S.Result       = ResultFromStatus (Rep->Status, Rep->Cancelled);
  S.Verify       = Verify;
  S.Targets      = 1;
  S.Failed       = EFI_ERROR (Status) ? 1 : 0;
  S.BytesWritten = Rep->BytesWritten;
  S.Passes       = Rep->PassesCompleted;
  S.Mismatches   = Rep->VerifyMismatches;
  S.TargetsText  = Name;
  (void)LogPersistWriteReport (&S);
}

#define MAX_ARGS  16
#define ARG_CHARS 64

/** Config file name on the volume the application was loaded from. */
#define CFG_NAME  L"\\UefiEraser.cfg"

STATIC CHAR16  mArgv[MAX_ARGS][ARG_CHARS];
STATIC UINTN   mArgc;

/** Split a whitespace-separated argument string into mArgv.

    A leading UTF-16 BOM (0xFEFF) is skipped: the config file is written by the
    host harness as UTF-16LE and conventionally carries one, and leaving it in
    place would glue a zero-width character onto the first argument. */
STATIC
VOID
SplitArgs (
  IN CONST CHAR16  *P,
  IN UINTN          Chars
  )
{
  UINTN I;
  UINTN K = 0;

  if (Chars > 0 && P[0] == 0xFEFF) {
    P++;
    Chars--;
  }

  for (I = 0; I < Chars && mArgc < MAX_ARGS; I++) {
    if (P[I] == L' ' || P[I] == L'\r' || P[I] == L'\n' ||
        P[I] == L'\t' || P[I] == 0)
    {
      if (K > 0) {
        mArgv[mArgc][K] = 0;
        mArgc++;
        K = 0;
      }
      if (P[I] == 0) {
        break;
      }
      continue;
    }
    if (K + 1 < ARG_CHARS) {
      mArgv[mArgc][K++] = P[I];
    }
  }
  if (K > 0 && mArgc < MAX_ARGS) {
    mArgv[mArgc][K] = 0;
    mArgc++;
  }
}

/** Read the boot volume's UefiEraser.cfg into mArgv.

    This is the PRIMARY way a headless run is configured, and the reason is
    purely mechanical: an EFI image booted directly through EFI/BOOT/BOOTX64.EFI
    receives no command line, and reaching the internal UEFI Shell to get one
    is not reliable - OVMF's default boot order does not include the shell as a
    fallback ("No bootable option or device was found"), so removing BOOTX64.EFI
    to force the shell just leaves the firmware with nothing to boot.

    So the app boots the deterministic way and reads its instructions from a
    file next to itself. */
STATIC
BOOLEAN
LoadArgsFromCfgFile (
  IN EFI_HANDLE  ImageHandle
  )
{
  EFI_STATUS                       Status;
  EFI_LOADED_IMAGE_PROTOCOL        *Li = NULL;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs = NULL;
  EFI_FILE_PROTOCOL                *Root = NULL;
  EFI_FILE_PROTOCOL                *File = NULL;
  EFI_FILE_INFO                    *Info = NULL;
  UINTN                            InfoSize = 0;
  CHAR16                           *Buf = NULL;
  UINTN                            ReadSize;

  Status = gBS->OpenProtocol (
                  ImageHandle,
                  &gEfiLoadedImageProtocolGuid,
                  (VOID **)&Li,
                  ImageHandle,
                  NULL,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  if (EFI_ERROR (Status) || Li == NULL || Li->DeviceHandle == NULL) {
    return FALSE;
  }

  Status = gBS->HandleProtocol (
                  Li->DeviceHandle,
                  &gEfiSimpleFileSystemProtocolGuid,
                  (VOID **)&Fs
                  );
  if (EFI_ERROR (Status)) {
    return FALSE;
  }

  Status = Fs->OpenVolume (Fs, &Root);
  if (EFI_ERROR (Status)) {
    return FALSE;
  }

  Status = Root->Open (Root, &File, CFG_NAME, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status)) {
    Root->Close (Root);
    return FALSE;
  }

  /* Size the file via its FileInfo. */
  Status = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, NULL);
  if (Status != EFI_BUFFER_TOO_SMALL || InfoSize == 0) {
    File->Close (File);
    Root->Close (Root);
    return FALSE;
  }
  Info = AllocatePool (InfoSize);
  if (Info == NULL) {
    File->Close (File);
    Root->Close (Root);
    return FALSE;
  }
  Status = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, Info);
  if (EFI_ERROR (Status) || Info->FileSize == 0) {
    FreePool (Info);
    File->Close (File);
    Root->Close (Root);
    return FALSE;
  }

  /* FileSize + one CHAR16 for a terminator. */
  Buf = AllocateZeroPool (Info->FileSize + sizeof (CHAR16));
  if (Buf == NULL) {
    FreePool (Info);
    File->Close (File);
    Root->Close (Root);
    return FALSE;
  }

  ReadSize = Info->FileSize;
  Status = File->Read (File, &ReadSize, Buf);
  File->Close (File);
  Root->Close (Root);
  FreePool (Info);

  if (EFI_ERROR (Status) || ReadSize == 0) {
    FreePool (Buf);
    return FALSE;
  }

  SplitArgs (Buf, ReadSize / sizeof (CHAR16));
  FreePool (Buf);

  DEBUG ((DEBUG_INFO, "[Headless] config file gave %u argument(s)\n",
          (UINT32)mArgc));
  /* Echo them: a mis-encoded config file is otherwise indistinguishable from a
     bad argument, and the echo makes the difference obvious in the log. */
  {
    UINTN K;
    for (K = 0; K < mArgc; K++) {
      DEBUG ((DEBUG_INFO, "[Headless]   arg[%u]=%s\n", (UINT32)K, mArgv[K]));
    }
  }
  return (BOOLEAN)(mArgc > 0);
}

/** Snapshot the arguments, trying three sources in order of directness:

      1. the shell command line (interactive use)
      2. LoadOptions (a boot option that carries arguments)
      3. UefiEraser.cfg on the boot volume (how the QEMU harness drives it)

    The first two are absent for a directly-booted image, which is the normal
    case under the QEMU harness. */
STATIC
VOID
LoadArgs (
  IN EFI_HANDLE  ImageHandle
  )
{
  EFI_STATUS                    Status;
  EFI_SHELL_PARAMETERS_PROTOCOL *Sp = NULL;
  EFI_LOADED_IMAGE_PROTOCOL     *Li = NULL;
  UINTN                         I;

  mArgc = 0;

  Status = gBS->OpenProtocol (
                  ImageHandle,
                  &gEfiShellParametersProtocolGuid,
                  (VOID **)&Sp,
                  ImageHandle,
                  NULL,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  if (!EFI_ERROR (Status) && Sp != NULL && Sp->Argv != NULL && Sp->Argc > 1) {
    for (I = 0; I < Sp->Argc && mArgc < MAX_ARGS; I++) {
      StrnCpyS (mArgv[mArgc], ARG_CHARS, Sp->Argv[I], ARG_CHARS - 1);
      mArgc++;
    }
    return;
  }

  Status = gBS->OpenProtocol (
                  ImageHandle,
                  &gEfiLoadedImageProtocolGuid,
                  (VOID **)&Li,
                  ImageHandle,
                  NULL,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  if (!EFI_ERROR (Status) && Li != NULL && Li->LoadOptions != NULL &&
      Li->LoadOptionsSize >= sizeof (CHAR16))
  {
    SplitArgs ((CONST CHAR16 *)Li->LoadOptions,
               Li->LoadOptionsSize / sizeof (CHAR16));
    if (mArgc > 0) {
      return;
    }
  }

  LoadArgsFromCfgFile (ImageHandle);
}

/** Find an argument starting with `Prefix` and return the part after it.
    Returns NULL when absent. */
STATIC
CONST CHAR16 *
ArgValue (
  IN CONST CHAR16  *Prefix
  )
{
  UINTN        I;
  UINTN        PrefixLen = StrLen (Prefix);

  for (I = 0; I < mArgc; I++) {
    if (StrnCmp (mArgv[I], Prefix, PrefixLen) == 0) {
      return mArgv[I] + PrefixLen;
    }
  }
  return NULL;
}

STATIC
BOOLEAN
HasFlag (
  IN CONST CHAR16  *Flag
  )
{
  UINTN I;

  for (I = 0; I < mArgc; I++) {
    if (StrCmp (mArgv[I], Flag) == 0) {
      return TRUE;
    }
  }
  return FALSE;
}

/** Parse a decimal number, returning -1 on any non-digit. The WHOLE string
    must be digits (used for `disk:<i>` and `custom=<n>`). */
STATIC
INT64
ParseDec (
  IN CONST CHAR16  *S
  )
{
  INT64  V = 0;

  if (S == NULL || *S == 0) {
    return -1;
  }
  while (*S != 0) {
    if (*S < L'0' || *S > L'9') {
      return -1;
    }
    V = V * 10 + (*S - L'0');
    if (V > 0x7FFFFFFF) {
      return -1;
    }
    S++;
  }
  return V;
}

/** Parse a LEADING decimal number and report how many characters it used.

    Needed for `part:<disk>:<n>`, where a plain ParseDec would fail on the ':'
    separator rather than returning the disk index. */
STATIC
INT64
ParseDecPrefix (
  IN  CONST CHAR16  *S,
  OUT UINTN         *Consumed
  )
{
  INT64  V = 0;
  UINTN  N = 0;

  *Consumed = 0;
  if (S == NULL) {
    return -1;
  }
  while (S[N] >= L'0' && S[N] <= L'9') {
    V = V * 10 + (S[N] - L'0');
    if (V > 0x7FFFFFFF) {
      return -1;
    }
    N++;
  }
  if (N == 0) {
    return -1;
  }
  *Consumed = N;
  return V;
}

BOOLEAN
HeadlessEraseRequested (
  IN EFI_HANDLE  ImageHandle
  )
{
  LoadArgs (ImageHandle);
  return (BOOLEAN)(ArgValue (L"erase=") != NULL);
}

/* --------------------------------------------------------------- the run */

/** Find the SimpleFileSystem volume that lives on `Part`.

    A mounted volume is a separate handle whose device path is the partition's
    path (sometimes with extra nodes), so the match is a prefix test in both
    directions - i.e. equality - on the partition's path. */
STATIC
EFI_HANDLE
FindVolumeForPartition (
  IN BLOCK_DEV  *Part
  )
{
  EFI_STATUS                 Status;
  EFI_HANDLE                *Handles = NULL;
  UINTN                      Count = 0;
  UINTN                      I;
  EFI_HANDLE                 Found = NULL;

  if (Part->DevicePath == NULL) {
    return NULL;
  }

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiSimpleFileSystemProtocolGuid,
                  NULL,
                  &Count,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    return NULL;
  }

  for (I = 0; I < Count; I++) {
    EFI_DEVICE_PATH_PROTOCOL  *Dp = NULL;

    Status = gBS->HandleProtocol (
                    Handles[I],
                    &gEfiDevicePathProtocolGuid,
                    (VOID **)&Dp
                    );
    if (EFI_ERROR (Status) || Dp == NULL) {
      continue;
    }
    if (GetDevicePathSize (Dp) == GetDevicePathSize (Part->DevicePath) &&
        CompareMem (Dp, Part->DevicePath, GetDevicePathSize (Dp)) == 0)
    {
      Found = Handles[I];
      break;
    }
  }

  FreePool (Handles);
  return Found;
}

EFI_STATUS
HeadlessEraseRun (
  IN EFI_HANDLE      ImageHandle,
  IN BLOCK_DEV_LIST  *Devs
  )
{
  CONST CHAR16           *Sel;
  CONST CHAR16           *AlgoId;
  CONST ERASE_ALGORITHM  *Algo;
  BLOCK_DEV              *Dev = NULL;
  ERASE_SESSION           Session;
  EFI_STATUS              Status;
  ERASE_REPORT           *Rep;
  UINT32                  Custom = 0;
  BOOLEAN                 Verify;
  UINT32                  DiskIdx = 0;
  UINT32                  WantPart = 0;
  BOOLEAN                 WantPartition = FALSE;
  ERASE_MODE              Mode = MODE_OVERWRITE;
  UINT32                  ModeParam = 0;
  UINT32                  Seen = 0;
  UINT32                  I;
  CONST CHAR16           *P;
  INT64                   N;

  Sel = ArgValue (L"erase=");
  if (Sel == NULL) {
    return EFI_ABORTED;
  }

  /* ---- parse the selector ----
     disk:<i>                 overwrite the whole disk
     part:<i>:<n>             overwrite one partition
     freespace:<i>:<n>        wipe the free space of a partition's volume
     ata:<i>[:enhanced]       ATA SECURITY ERASE UNIT on the whole disk
     nvme-format:<i>:<ses>    NVMe Format NVM (1 = user data, 2 = crypto)
     nvme-sanitize:<i>:<act>  NVMe Sanitize (1 = block, 2 = overwrite, 4 = crypto) */
  if (StrnCmp (Sel, L"disk:", 5) == 0) {
    N = ParseDec (Sel + 5);
    if (N < 0) {
      DEBUG ((DEBUG_ERROR, "[Headless] bad disk index\n"));
      return EFI_ABORTED;
    }
    DiskIdx = (UINT32)N;
  } else if (StrnCmp (Sel, L"ata:", 4) == 0 ||
             StrnCmp (Sel, L"nvme-format:", 12) == 0 ||
             StrnCmp (Sel, L"nvme-sanitize:", 14) == 0)
  {
    UINTN Used = 0;

    if (StrnCmp (Sel, L"ata:", 4) == 0) {
      Mode = MODE_ATA;
      P    = Sel + 4;
    } else if (StrnCmp (Sel, L"nvme-format:", 12) == 0) {
      Mode = MODE_NVME_FORMAT;
      P    = Sel + 12;
    } else {
      Mode = MODE_NVME_SANITIZE;
      P    = Sel + 14;
    }

    N = ParseDecPrefix (P, &Used);
    if (N < 0) {
      DEBUG ((DEBUG_ERROR, "[Headless] bad disk index in the selector\n"));
      return EFI_ABORTED;
    }
    DiskIdx = (UINT32)N;
    P += Used;

    /* Optional ":<param>". Absent means "let the app choose": enhanced for
       ATA, user-data-erase for Format, block-erase for Sanitize. */
    if (*P == L':') {
      N = ParseDecPrefix (P + 1, &Used);
      if (N < 0) {
        DEBUG ((DEBUG_ERROR, "[Headless] bad parameter after ':'\n"));
        return EFI_ABORTED;
      }
      ModeParam = (UINT32)N;
    } else if (*P != 0) {
      DEBUG ((DEBUG_ERROR, "[Headless] unexpected characters in the selector\n"));
      return EFI_ABORTED;
    }
  } else if (StrnCmp (Sel, L"part:", 5) == 0 || StrnCmp (Sel, L"freespace:", 10) == 0) {
    UINTN Used = 0;

    if (StrnCmp (Sel, L"freespace:", 10) == 0) {
      Mode = MODE_FREESPACE;
      P    = Sel + 10;
    } else {
      P = Sel + 5;
    }

    N = ParseDecPrefix (P, &Used);
    if (N < 0) {
      DEBUG ((DEBUG_ERROR, "[Headless] bad disk index in the selector\n"));
      return EFI_ABORTED;
    }
    DiskIdx = (UINT32)N;
    P += Used;
    if (*P != L':') {
      DEBUG ((DEBUG_ERROR, "[Headless] selector needs <disk>:<part>\n"));
      return EFI_ABORTED;
    }
    N = ParseDecPrefix (P + 1, &Used);
    if (N <= 0) {
      DEBUG ((DEBUG_ERROR, "[Headless] bad partition number\n"));
      return EFI_ABORTED;
    }
    WantPart      = (UINT32)N;
    WantPartition = TRUE;
  } else {
    DEBUG ((DEBUG_ERROR, "[Headless] erase= must be disk:<i> or part:<i>:<n>\n"));
    return EFI_ABORTED;
  }

  /* ---- resolve the target ---- */
  for (I = 0; I < Devs->Count; I++) {
    BLOCK_DEV *D = &Devs->Items[I];

    if (D->LogicalPartition) {
      continue;
    }
    if (Seen == DiskIdx) {
      if (!WantPartition) {
        Dev = D;
      } else {
        /* scan this disk's partitions for the requested number */
        UINT32 J;
        for (J = 0; J < Devs->Count; J++) {
          BLOCK_DEV *Q = &Devs->Items[J];
          if (Q->LogicalPartition &&
              Q->ParentHandle == D->Handle &&
              Q->PartitionNo == WantPart)
          {
            Dev = Q;
            break;
          }
        }
      }
      break;
    }
    Seen++;
  }

  if (Dev == NULL) {
    DEBUG ((DEBUG_ERROR, "[Headless] target not found (disk %u)\n", (UINT32)DiskIdx));
    return EFI_ABORTED;
  }
  if (Dev->ReadOnly) {
    DEBUG ((DEBUG_ERROR, "[Headless] target is read-only\n"));
    return EFI_ABORTED;
  }
  if (Dev->IsBootVolume) {
    DEBUG ((DEBUG_ERROR, "[Headless] refusing to erase the boot volume\n"));
    return EFI_ABORTED;
  }

  /* ---- options ---- */
  AlgoId = ArgValue (L"algo=");
  if (AlgoId != NULL) {
    CHAR8  Id[64];
    UnicodeStrToAsciiStrS (AlgoId, Id, sizeof (Id));
    Algo = EraseAlgoById (Id);
    if (Algo == NULL) {
      DEBUG ((DEBUG_ERROR, "[Headless] unknown algo=%a\n", Id));
      return EFI_ABORTED;
    }
  } else {
    Algo = EraseAlgoById ("dod522022m");
  }

  N = ParseDec (ArgValue (L"custom="));
  if (N > 0) {
    Custom = (UINT32)N;
  }
  Verify = HasFlag (L"verify");

  /* ---- free-space mode: a different operation on the same target ---- */
  if (Mode == MODE_FREESPACE) {
    EFI_HANDLE    Vol;
    ERASE_REPORT  FsReport;

    Vol = FindVolumeForPartition (Dev);
    if (Vol == NULL) {
      DEBUG ((DEBUG_ERROR,
              "[Headless] no mounted volume on partition %u (not a filesystem?)\n",
              (UINT32)Dev->PartitionNo));
      return EFI_UNSUPPORTED;
    }

    DEBUG ((
      DEBUG_INFO,
      "[Headless] FREESPACE BEGIN part#%u algo=%a passes=%u\n",
      (UINT32)Dev->PartitionNo,
      Algo->Id,
      (Algo->PassCount != 0) ? Algo->PassCount
                             : ((Custom != 0) ? Custom : 1)
      ));

    Status = FreeSpaceWipeVolume (
               Vol,
               Algo,
               Custom,
               LvglTickGetMs () ^ (UINT64)(UINTN)Vol,
               NULL,          /* headless: progress goes to the serial log */
               NULL,
               &FsReport
               );

    DEBUG ((
      DEBUG_INFO,
      "[Headless] FREESPACE DONE status=%a passes=%u/%u written=%lu\n",
      EraseStatusName (FsReport.Status),
      (UINT32)FsReport.PassesCompleted,
      (UINT32)FsReport.PassCount,
      (UINT64)FsReport.BytesWritten
      ));
    PersistRun (
      Dev, Mode, DiskIdx, "freespace", Algo->Id,
      (Algo->PassCount != 0) ? Algo->PassCount : ((Custom != 0) ? Custom : 1),
      Status, &FsReport, Verify
      );
    return Status;
  }

  /* ---- device-level modes: the drive's own firmware does the erasing ---- */
  if (Mode == MODE_ATA) {
    ERASE_REPORT  DevReport;
    BOOLEAN       Enhanced = (ModeParam != 0);

    DEBUG ((
      DEBUG_INFO,
      "[Headless] ATA-ERASE BEGIN disk#%u enhanced=%d\n",
      (UINT32)DiskIdx,
      Enhanced
      ));

    Status = AtaSecureErase (Dev->Handle, Enhanced, NULL, NULL, &DevReport);

    DEBUG ((
      DEBUG_INFO,
      "[Headless] ATA-ERASE DONE status=%a passes=%u/%u written=%lu\n",
      EraseStatusName (DevReport.Status),
      (UINT32)DevReport.PassesCompleted,
      (UINT32)DevReport.PassCount,
      (UINT64)DevReport.BytesWritten
      ));
    PersistRun (Dev, Mode, DiskIdx, "ata", "-", 0, Status, &DevReport, Verify);
    return Status;
  }

  if (Mode == MODE_NVME_FORMAT || Mode == MODE_NVME_SANITIZE) {
    ERASE_REPORT  DevReport;
    UINT32        Param;

    /* Defaults when the selector gave no parameter: the safest useful action
       for each command. */
    if (ModeParam == 0) {
      Param = (Mode == MODE_NVME_FORMAT) ? NVME_SES_USER_DATA_ERASE
                                         : NVME_SANACT_BLOCK_ERASE;
    } else {
      Param = ModeParam;
    }

    DEBUG ((
      DEBUG_INFO,
      "[Headless] %a BEGIN disk#%u param=%u\n",
      (Mode == MODE_NVME_FORMAT) ? "NVME-FORMAT" : "NVME-SANITIZE",
      (UINT32)DiskIdx,
      (UINT32)Param
      ));

    if (Mode == MODE_NVME_FORMAT) {
      Status = NvmeFormat (Dev->Handle, (UINT8)Param, NULL, NULL, &DevReport);
    } else {
      Status = NvmeSanitize (Dev->Handle, (UINT8)Param, NULL, NULL, &DevReport);
    }

    DEBUG ((
      DEBUG_INFO,
      "[Headless] %a DONE status=%a passes=%u/%u written=%lu\n",
      (Mode == MODE_NVME_FORMAT) ? "NVME-FORMAT" : "NVME-SANITIZE",
      EraseStatusName (DevReport.Status),
      (UINT32)DevReport.PassesCompleted,
      (UINT32)DevReport.PassCount,
      (UINT64)DevReport.BytesWritten
      ));
    {
      CONST CHAR8 *ModeStr = (Mode == MODE_NVME_FORMAT) ? "nvme-format"
                                                        : "nvme-sanitize";
      PersistRun (Dev, Mode, DiskIdx, ModeStr, "-", 0, Status, &DevReport, Verify);
    }
    return Status;
  }

  DEBUG ((
    DEBUG_INFO,
    "[Headless] ERASE BEGIN %a#%u algo=%a passes=%u verify=%d bytes=%lu\n",
    Dev->LogicalPartition ? "part" : "disk",
    Dev->PartitionNo,
    Algo->Id,
    (Algo->PassCount != 0) ? Algo->PassCount : ((Custom != 0) ? Custom : 1),
    Verify,
    (UINT64)Dev->Length
    ));

  /* Pump = NULL: there is no UI, and OverwriteErase's progress callback falls
     back to a serial log line per sampling window in that case. */
  Status = EraseSessionInit (&Session, Dev, Algo, Custom, Verify, NULL, NULL);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[Headless] session init failed: %r\n", Status));
    return Status;
  }

  Status = EraseSessionRun (&Session);
  Rep    = &Session.Report;
  DEBUG ((
    DEBUG_INFO,
    "[Headless] ERASE DONE status=%a passes=%u/%u written=%lu planned=%lu "
    "verified=%d mismatches=%u\n",
    EraseStatusName (Rep->Status),
    (UINT32)Rep->PassesCompleted,
    (UINT32)Rep->PassCount,
    (UINT64)Rep->BytesWritten,
    (UINT64)Rep->TotalBytesPlanned,
    Rep->Verified,
    (UINT32)Rep->VerifyMismatches
    ));

  PersistRun (
    Dev, Mode, DiskIdx, "overwrite", Algo->Id,
    (Algo->PassCount != 0) ? Algo->PassCount : ((Custom != 0) ? Custom : 1),
    Status, Rep, Verify
    );

  EraseSessionFree (&Session);
  return Status;
}
