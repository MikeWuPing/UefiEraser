/** @file
  Platform/OverwriteErase.c - see OverwriteErase.h.

  Copyright (c) 2026, Mike Wu. All rights reserved.
**/

#include <Uefi.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/LvglUefiPort.h>   /* LvglTickGetMs: the calibrated ms tick */

#include "OverwriteErase.h"

/** How often the progress struct and the UI are refreshed. 250 ms keeps the
    bar visibly moving without letting a redraw dominate the write throughput
    on slow media. */
#define PROGRESS_INTERVAL_MS  250u

/** Media is re-checked this often (in bytes written). A removable device
    swapped mid-erase would otherwise receive the remaining passes. */
#define MEDIA_CHECK_BYTES  (256ULL * 1024 * 1024)

/* -------------------------------------------------------------- callbacks */

STATIC
ERASE_STATUS
SessionWrite (
  IN VOID        *Ctx,
  IN E_U64        Offset,
  IN E_SIZE       Len,
  IN CONST E_U8  *Data
  )
{
  ERASE_SESSION *S = (ERASE_SESSION *)Ctx;
  EFI_STATUS     Status;

  Status = BlockDevWrite (S->Dev, Offset, (UINTN)Len, Data);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "[Erase] write failed at offset %lu len %u: %r\n",
      (UINT64)Offset,
      (UINT32)Len,
      Status
      ));
    return ERASE_ERR_IO;
  }
  return ERASE_OK;
}

STATIC
ERASE_STATUS
SessionRead (
  IN VOID    *Ctx,
  IN E_U64    Offset,
  IN E_SIZE   Len,
  IN E_U8    *Data
  )
{
  ERASE_SESSION *S = (ERASE_SESSION *)Ctx;
  EFI_STATUS     Status;

  Status = BlockDevRead (S->Dev, Offset, (UINTN)Len, Data);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "[Erase] read-back failed at offset %lu len %u: %r\n",
      (UINT64)Offset,
      (UINT32)Len,
      Status
      ));
    return ERASE_ERR_IO;
  }
  return ERASE_OK;
}

STATIC
VOID
SessionProgress (
  IN VOID   *Ctx,
  IN E_U64   Done,
  IN E_U64   Total,
  IN E_U32   Pass,
  IN E_U32   PassCount
  )
{
  ERASE_SESSION *S   = (ERASE_SESSION *)Ctx;
  UINT64         Now = LvglTickGetMs ();

  S->Progress.Done      = Done;
  S->Progress.Total     = Total;
  S->Progress.Pass      = Pass;
  S->Progress.PassCount = PassCount;
  S->Progress.Percent   = (Total == 0) ? 100u : (UINT32)((Done * 100) / Total);

  /* Rate over a sampling window rather than per-chunk: instantaneous deltas are
     far too noisy to show a user, and the ETA derived from them jumps around. */
  if (Now >= S->LastSampleMs + PROGRESS_INTERVAL_MS) {
    UINT64 DeltaMs   = Now - S->LastSampleMs;
    UINT64 DeltaDone = Done - S->LastSampleDone;

    if (DeltaMs > 0 && DeltaDone > 0) {
      S->Progress.BytesPerSec = (DeltaDone * 1000) / DeltaMs;
      if (S->Progress.BytesPerSec > 0) {
        UINT64 Remaining = (Total > Done) ? (Total - Done) : 0;
        UINT64 EtaSec    = Remaining / S->Progress.BytesPerSec;
        S->Progress.EtaSec = (EtaSec > 0xFFFFFFFFULL) ? 0xFFFFFFFFu : (UINT32)EtaSec;
      }
    }

    /* Periodic media-change probe, so a swapped removable disk is caught within
       a bounded amount of damage rather than after the whole job. Compared
       BEFORE LastSampleDone is advanced - otherwise the test is always false. */
    if ((Done / MEDIA_CHECK_BYTES) != (S->LastSampleDone / MEDIA_CHECK_BYTES)) {
      if (EFI_ERROR (BlockDevCheckMedia (S->Dev))) {
        S->MediaChanged = TRUE;
      }
    }

    S->LastSampleMs   = Now;
    S->LastSampleDone = Done;

    if (S->Pump != NULL) {
      S->Pump (S->PumpCtx);
    } else {
      /* Headless run: no UI to redraw, but the QEMU harness still needs to see
         that a long erasure is alive rather than hung. Log one line per
         sampling window. */
      DEBUG ((
        DEBUG_INFO,
        "[Erase] progress pass=%u/%u done=%lu total=%lu %u%%\n",
        (UINT32)Pass,
        (UINT32)PassCount,
        (UINT64)Done,
        (UINT64)Total,
        (UINT32)S->Progress.Percent
        ));
    }
  }
}

STATIC
E_BOOL
SessionIsCancelled (
  IN VOID  *Ctx
  )
{
  ERASE_SESSION *S = (ERASE_SESSION *)Ctx;

  if (S->MediaChanged) {
    /* Report through the cancel channel: the engine aborts, and Run maps it
       back onto EFI_MEDIA_CHANGED so the UI can say why. */
    return E_TRUE;
  }
  return (E_BOOL)S->CancelRequested;
}

/* ------------------------------------------------------------- lifecycle */

EFI_STATUS
EraseSessionInit (
  OUT ERASE_SESSION             *S,
  IN  BLOCK_DEV                 *Dev,
  IN  CONST ERASE_ALGORITHM     *Algo,
  IN  UINT32                     CustomPasses,
  IN  BOOLEAN                    Verify,
  IN  ERASE_PUMP_CB              Pump,
  IN  VOID                      *PumpCtx
  )
{
  if (S == NULL || Dev == NULL || Algo == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if (Dev->ReadOnly) {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (S, sizeof (*S));
  S->Dev          = Dev;
  S->Algo         = Algo;
  S->CustomPasses = CustomPasses;
  S->Verify       = Verify;
  S->Pump         = Pump;
  S->PumpCtx      = PumpCtx;

  /* ChunkSize, doubled when verifying because the engine reads into the second
     half while comparing against the first. */
  S->BufferBytes = ERASE_CHUNK_SIZE * (Verify ? 2 : 1);

  /* Fall back to a smaller chunk if the pool cannot give us 6 (or 12) MiB.
     UEFI has no virtual memory, so this is a real possibility on a fragmented
     heap; a smaller chunk only costs throughput. */
  while (S->BufferBytes > 0) {
    S->Buffer = AllocatePool (S->BufferBytes);
    if (S->Buffer != NULL) {
      break;
    }
    S->BufferBytes /= 2;
  }
  if (S->Buffer == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  DEBUG ((
    DEBUG_INFO,
    "[Erase] session algo=%a passes=%u chunk=%u verify=%d target=%lu bytes\n",
    Algo->Id,
    (Algo->PassCount != 0) ? Algo->PassCount
                           : ((CustomPasses != 0) ? CustomPasses : 1),
    (UINT32)(S->BufferBytes / (Verify ? 2 : 1)),
    Verify,
    (UINT64)Dev->Length
    ));

  return EFI_SUCCESS;
}

VOID
EraseSessionFree (
  IN OUT ERASE_SESSION  *S
  )
{
  if (S == NULL) {
    return;
  }
  if (S->Buffer != NULL) {
    FreePool (S->Buffer);
    S->Buffer = NULL;
  }
  S->BufferBytes = 0;
}

EFI_STATUS
EraseSessionRun (
  IN OUT ERASE_SESSION  *S
  )
{
  ERASE_STATUS  Core;
  UINT64        Chunk;

  if (S == NULL || S->Buffer == NULL || S->Dev == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  /* Flush anything the firmware had queued before we start rewriting: a stale
     cached block written after pass 1 would silently undo it. */
  BlockDevFlush (S->Dev);

  Chunk = (UINT64)S->BufferBytes / (S->Verify ? 2 : 1);

  ZeroMem (&S->Job, sizeof (S->Job));
  S->Job.Algo               = S->Algo;
  S->Job.CustomRandomPasses = S->CustomPasses;
  S->Job.TargetOffset       = S->Dev->Offset;
  S->Job.TargetLength       = S->Dev->Length;
  S->Job.ChunkSize          = (E_SIZE)Chunk;
  S->Job.VerifyLastPass     = (E_BOOL)S->Verify;
  S->Job.Buffer             = S->Buffer;
  S->Job.IoContext          = S;
  S->Job.Write              = SessionWrite;
  S->Job.Read               = SessionRead;
  S->Job.Progress           = SessionProgress;
  S->Job.IsCancelled        = SessionIsCancelled;
  S->Job.Report             = &S->Report;

  /* Seed from the calibrated tick plus the target's identity: two erasures of
     the same device must not produce the same random stream, and there is no
     platform RNG to rely on. */
  S->Job.Seed = LvglTickGetMs () ^ (S->Dev->Length * 0x9E3779B97F4A7C15ULL)
                ^ ((UINT64)(UINTN)S->Dev->Handle);

  S->Running          = TRUE;
  S->Finished         = FALSE;
  S->CancelRequested  = FALSE;
  S->MediaChanged     = FALSE;
  S->StartMs          = LvglTickGetMs ();
  S->LastSampleMs     = S->StartMs;
  S->LastSampleDone   = 0;
  ZeroMem (&S->Progress, sizeof (S->Progress));
  S->Progress.PassCount = EraseJobPassCount (&S->Job);

  Core = EraseJobRun (&S->Job);

  S->Running  = FALSE;
  S->Finished = TRUE;

  BlockDevFlush (S->Dev);

  DEBUG ((
    DEBUG_INFO,
    "[Erase] done status=%a passes=%u/%u written=%lu verified=%d mismatches=%u\n",
    EraseStatusName (Core),
    (UINT32)S->Report.PassesCompleted,
    (UINT32)S->Report.PassCount,
    (UINT64)S->Report.BytesWritten,
    S->Report.Verified,
    (UINT32)S->Report.VerifyMismatches
    ));

  if (S->MediaChanged) {
    return EFI_MEDIA_CHANGED;
  }

  switch (Core) {
    case ERASE_OK:
      return EFI_SUCCESS;
    case ERASE_ERR_CANCELLED:
      return EFI_ABORTED;
    case ERASE_ERR_PARAM:
      return EFI_INVALID_PARAMETER;
    case ERASE_ERR_VERIFY:
      return EFI_VOLUME_CORRUPTED;
    case ERASE_ERR_MEDIA_CHANGED:
      return EFI_MEDIA_CHANGED;
    case ERASE_ERR_IO:
    default:
      return EFI_DEVICE_ERROR;
  }
}
