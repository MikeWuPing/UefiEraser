/** @file
  Platform/FreeSpaceWipe.c - see FreeSpaceWipe.h.

  Copyright (c) 2026, Mike Wu. All rights reserved.
**/

#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/PrintLib.h>          /* UnicodeSPrint */
#include <Library/DebugLib.h>
#include <Library/LvglUefiPort.h>      /* LvglTickGetMs */

#include <Protocol/SimpleFileSystem.h>

#include "FreeSpaceWipe.h"

/** Temp file name pattern: \ERWnnnn.TMP. 8.3-safe, so it works on FAT12/16 as
    well as FAT32/exFAT, and with the fixed ERW prefix the verifier can spot a
    file a cancelled run left behind. */
#define WIPE_FILE_FMT  L"\\ERW%04d.TMP"

/** Write `Len` bytes of the current pass's pattern into `File`.

    Returns the number of bytes actually accepted. A short write is the normal
    way a full volume announces itself, so it is not an error. */
STATIC
UINT64
WritePattern (
  IN EFI_FILE_PROTOCOL  *File,
  IN UINT8              *Buf,
  IN UINTN               Len,
  IN const ERASE_PASS   *Pass,
  IN ERASE_PRNG         *Prng,
  IN ERASE_RAND_SLOTS   *Slots
  )
{
  UINT64  Written = 0;

  while (Written < Len) {
    UINTN       ThisChunk = (UINTN)(Len - Written);
    EFI_STATUS  Status;

    if (ThisChunk > FREESPACE_CHUNK_BYTES) {
      ThisChunk = FREESPACE_CHUNK_BYTES;
    }

    ErasePassFill (Pass, Buf, ThisChunk, Prng, Slots);

    Status = File->Write (File, &ThisChunk, Buf);
    if (EFI_ERROR (Status) || ThisChunk == 0) {
      break;                       /* volume full: expected, not a failure */
    }
    Written += ThisChunk;
    if (ThisChunk < FREESPACE_CHUNK_BYTES) {
      break;                       /* short write: the volume is full */
    }
  }
  return Written;
}

EFI_STATUS
FreeSpaceWipeVolume (
  IN  EFI_HANDLE              VolumeHandle,
  IN  CONST ERASE_ALGORITHM  *Algo,
  IN  UINT32                  CustomPasses,
  IN  UINT64                  Seed,
  IN  ERASE_PUMP_CB           Pump,
  IN  VOID                   *PumpCtx,
  OUT ERASE_REPORT           *Report
  )
{
  EFI_STATUS                       Status;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs = NULL;
  EFI_FILE_PROTOCOL                *Root = NULL;
  UINT8                            *Buf = NULL;
  ERASE_PRNG                        Prng;
  ERASE_RAND_SLOTS                  Slots;
  UINT32                            PassCount;
  UINT32                            Pass;
  UINT64                            TotalWritten = 0;
  UINT64                            StartMs;
  UINT64                            LastReportMs;

  if (Algo == NULL || Report == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  ZeroMem (Report, sizeof (*Report));
  Report->AlgoId = Algo->Id;

  Status = gBS->HandleProtocol (
                  VolumeHandle,
                  &gEfiSimpleFileSystemProtocolGuid,
                  (VOID **)&Fs
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[FreeSpace] no SimpleFileSystem on the volume: %r\n",
            Status));
    return EFI_UNSUPPORTED;
  }

  Status = Fs->OpenVolume (Fs, &Root);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[FreeSpace] OpenVolume failed: %r\n", Status));
    return EFI_UNSUPPORTED;
  }

  Buf = AllocatePool (FREESPACE_CHUNK_BYTES);
  if (Buf == NULL) {
    Root->Close (Root);
    return EFI_OUT_OF_RESOURCES;
  }

  PassCount = (Algo->PassCount != 0) ? Algo->PassCount
                                     : ((CustomPasses != 0) ? CustomPasses : 1);
  Report->PassCount = PassCount;

  ErasePrngSeed (&Prng, Seed);
  EraseRandSlotsInit (&Slots, &Prng);

  StartMs      = LvglTickGetMs ();
  LastReportMs = StartMs;

  DEBUG ((
    DEBUG_INFO,
    "[FreeSpace] begin algo=%a passes=%u\n",
    Algo->Id,
    (UINT32)PassCount
    ));

  for (Pass = 0; Pass < PassCount; Pass++) {
    const ERASE_PASS  *Ps = (Algo->PassCount != 0) ? &Algo->Passes[Pass]
                                                   : &Algo->Passes[0];
    UINT64  PassWritten = 0;
    UINT32  Files = 0;
    UINT32  I;

    /* PHASE 1 - fill. Every temp file is created and LEFT IN PLACE.

       Deleting each file as soon as it is written (the obvious-looking loop)
       never terminates: deleting frees the clusters again, so the next file
       writes just as much as the last one. That is exactly what happened on the
       first run of this code - thirteen files of 9 MiB each, 122 MiB written to
       a 10 MiB volume, still going. Filling only works if the files accumulate
       until the volume genuinely has no room left. */
    for (I = 0; I < FREESPACE_MAX_FILES; I++) {
      EFI_FILE_PROTOCOL  *File = NULL;
      CHAR16              Name[32];
      UINT64              FileWritten;

      UnicodeSPrint (Name, sizeof (Name), WIPE_FILE_FMT, I);

      Status = Root->Open (
                       Root,
                       &File,
                       Name,
                       EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                       0
                       );
      if (EFI_ERROR (Status)) {
        /* No room for even a directory entry: the volume is full. This is the
           normal way phase 1 ends, not an error. */
        break;
      }

      FileWritten = WritePattern (File, Buf, FREESPACE_FILE_UNIT, Ps, &Prng, &Slots);
      File->Flush (File);
      File->Close (File);

      if (FileWritten == 0) {
        /* Created but could not write: remove the empty entry and stop. */
        Status = Root->Open (Root, &File, Name,
                             EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
        if (!EFI_ERROR (Status)) {
          File->Delete (File);
        }
        break;
      }

      Files++;
      PassWritten  += FileWritten;
      TotalWritten += FileWritten;

      Report->BytesWritten      = TotalWritten;
      Report->TotalBytesPlanned = TotalWritten;
      Report->PassesCompleted   = Pass;

      DEBUG ((
        DEBUG_INFO,
        "[FreeSpace] pass=%u/%u fill file=%u wrote=%lu total=%lu\n",
        (UINT32)(Pass + 1),
        (UINT32)PassCount,
        (UINT32)Files,
        (UINT64)FileWritten,
        (UINT64)TotalWritten
        ));

      if (LvglTickGetMs () >= LastReportMs + 250) {
        LastReportMs = LvglTickGetMs ();
        if (Pump != NULL) {
          Pump (PumpCtx);
        }
      }
    }

    if (I == FREESPACE_MAX_FILES) {
      DEBUG ((
        DEBUG_WARN,
        "[FreeSpace] stopped at the %u-file cap with free space remaining; "
        "not all of the volume was covered\n",
        (UINT32)FREESPACE_MAX_FILES
        ));
    }

    /* PHASE 2 - delete everything written. Only now, with the volume full, do
       the freed clusters get handed back; the pattern stays on the medium until
       something else writes there, which is the whole point. */
    for (I = 0; I < Files; I++) {
      EFI_FILE_PROTOCOL  *File = NULL;
      CHAR16              Name[32];

      UnicodeSPrint (Name, sizeof (Name), WIPE_FILE_FMT, I);
      Status = Root->Open (Root, &File, Name,
                           EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
      if (!EFI_ERROR (Status)) {
        File->Delete (File);         /* Delete closes the handle on success */
      }
    }

    DEBUG ((
      DEBUG_INFO,
      "[FreeSpace] pass %u done: %u file(s), %lu bytes\n",
      (UINT32)(Pass + 1),
      (UINT32)Files,
      (UINT64)PassWritten
      ));

    Report->PassesCompleted = Pass + 1;
  }

  Report->Status = ERASE_OK;
  DEBUG ((
    DEBUG_INFO,
    "[FreeSpace] done passes=%u/%u written=%lu elapsed=%lu ms\n",
    (UINT32)Report->PassesCompleted,
    (UINT32)PassCount,
    (UINT64)TotalWritten,
    (UINT64)(LvglTickGetMs () - StartMs)
    ));

  FreePool (Buf);
  Root->Close (Root);
  return EFI_SUCCESS;
}
