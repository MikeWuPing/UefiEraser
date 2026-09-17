/** @file
  Core/EraseEngine.c - see EraseEngine.h for the contract.

  Copyright (c) 2026, Mike Wu. All rights reserved.
**/

#include "EraseEngine.h"

E_U32
EraseJobPassCount (
  const ERASE_JOB  *Job
  )
{
  if (Job == E_NULL || Job->Algo == E_NULL) {
    return 0;
  }
  /* PassCount == 0 in the table means "the pass list is supplied by the job"
     (the custom method). */
  if (Job->Algo->PassCount == 0) {
    return (Job->CustomRandomPasses == 0) ? 1 : Job->CustomRandomPasses;
  }
  return Job->Algo->PassCount;
}

/** The pass descriptor for pass index `P` (0-based). For the custom method
    every pass is a random stream, which is what kRandom[] holds. */
static const ERASE_PASS *
PassAt (
  const ERASE_JOB  *Job,
  E_U32             P
  )
{
  if (Job->Algo->PassCount == 0) {
    return &Job->Algo->Passes[0];   /* kRandom: one random pass, reused */
  }
  return &Job->Algo->Passes[P];
}

ERASE_STATUS
EraseJobRun (
  ERASE_JOB  *Job
  )
{
  ERASE_PRNG         Prng;
  ERASE_RAND_SLOTS   Slots;
  const ERASE_PASS  *Pass = E_NULL;
  ERASE_STATUS       Status;
  E_U32              PassCount;
  E_U32              P;
  E_U64              TotalBytes;
  E_U64              Written;
  E_U64              Off;
  E_SIZE             ThisChunk;
  E_BOOL             SaveState;
  ERASE_PRNG         LastPassPrng;

  /* Zero the snapshot so the compiler can see it is initialised on every path
     that reads it (it is only read when SaveState was set). */
  E_MEMSET (&LastPassPrng, 0, sizeof (LastPassPrng));

  if (Job == E_NULL || Job->Report == E_NULL) {
    return ERASE_ERR_PARAM;
  }
  E_MEMSET (Job->Report, 0, sizeof (ERASE_REPORT));
  Job->Report->TargetOffset = Job->TargetOffset;
  Job->Report->TargetLength = Job->TargetLength;

  if (Job->Algo == E_NULL || Job->Write == E_NULL ||
      Job->Buffer == E_NULL || Job->ChunkSize == 0) {
    Job->Report->Status = ERASE_ERR_PARAM;
    return ERASE_ERR_PARAM;
  }
  if (Job->VerifyLastPass && Job->Read == E_NULL) {
    Job->Report->Status = ERASE_ERR_PARAM;
    return ERASE_ERR_PARAM;
  }

  PassCount = EraseJobPassCount (Job);
  if (PassCount == 0) {
    Job->Report->Status = ERASE_ERR_PARAM;
    return ERASE_ERR_PARAM;
  }

  Job->Report->AlgoId          = Job->Algo->Id;
  Job->Report->PassCount       = PassCount;
  Job->Report->TotalBytesPlanned = EraseAlgoTotalBytes (Job->Algo, Job->TargetLength);
  if (Job->Algo->PassCount == 0) {
    /* EraseAlgoTotalBytes assumes 1 pass for the custom method; fix it up. */
    Job->Report->TotalBytesPlanned = Job->TargetLength * PassCount;
  }

  if (Job->TargetLength == 0) {
    Job->Report->Status = ERASE_OK;
    return ERASE_OK;
  }

  ErasePrngSeed (&Prng, Job->Seed);
  EraseRandSlotsInit (&Slots, &Prng);

  TotalBytes = Job->Report->TotalBytesPlanned;
  Written    = 0;
  Status     = ERASE_OK;

  for (P = 0; P < PassCount; P++) {
    Pass = PassAt (Job, P);

    /* Snapshot the generator right before the final pass so the verification
       step can replay exactly the bytes that pass wrote. */
    SaveState = (E_BOOL)(Job->VerifyLastPass && (P == PassCount - 1));
    if (SaveState) {
      LastPassPrng = Prng;
    }

    Off = 0;
    while (Off < Job->TargetLength) {
      if (Job->IsCancelled != E_NULL && Job->IsCancelled (Job->IoContext)) {
        Job->Report->Cancelled = E_TRUE;
        Job->Report->Status    = ERASE_ERR_CANCELLED;
        return ERASE_ERR_CANCELLED;
      }

      ThisChunk = (E_SIZE)(Job->TargetLength - Off);
      if ((E_U64)ThisChunk > (E_U64)Job->ChunkSize) {
        ThisChunk = Job->ChunkSize;
      }

      ErasePassFill (Pass, Job->Buffer, ThisChunk, &Prng, &Slots);

      Status = Job->Write (Job->IoContext,
                           Job->TargetOffset + Off,
                           ThisChunk,
                           Job->Buffer);
      if (Status != ERASE_OK) {
        Job->Report->FailedOffset = Off;
        Job->Report->Status       = Status;
        return Status;
      }

      Off     += ThisChunk;
      Written += ThisChunk;
      Job->Report->BytesWritten = Written;

      if (Job->Progress != E_NULL) {
        Job->Progress (Job->IoContext, Written, TotalBytes, P + 1, PassCount);
      }
    }

    Job->Report->PassesCompleted = P + 1;
  }

  /* ---- verification: replay the last pass and compare ---- */
  if (Job->VerifyLastPass && Pass != E_NULL) {
    ERASE_PRNG VerifyPrng = LastPassPrng;

    Off = 0;
    while (Off < Job->TargetLength) {
      ThisChunk = (E_SIZE)(Job->TargetLength - Off);
      if ((E_U64)ThisChunk > (E_U64)Job->ChunkSize) {
        ThisChunk = Job->ChunkSize;
      }

      ErasePassFill (Pass, Job->Buffer, ThisChunk, &VerifyPrng, &Slots);

      Status = Job->Read (Job->IoContext,
                          Job->TargetOffset + Off,
                          ThisChunk,
                          Job->Buffer + Job->ChunkSize);
      if (Status != ERASE_OK) {
        Job->Report->FailedOffset = Off;
        Job->Report->Status       = Status;
        return Status;
      }

      /* Compare byte by byte; counting mismatches beats bailing on the first
         one because a whole-sector mismatch (a drive that ignored the write)
         reads very differently from a handful of stuck bits. */
      {
        E_SIZE I;
        for (I = 0; I < ThisChunk; I++) {
          if (Job->Buffer[I] != Job->Buffer[Job->ChunkSize + I]) {
            Job->Report->VerifyMismatches++;
          }
        }
      }

      Off += ThisChunk;
      Job->Report->BytesVerified = Off;
    }

    Job->Report->Verified = (E_BOOL)(Job->Report->VerifyMismatches == 0);
    if (!Job->Report->Verified) {
      Job->Report->Status = ERASE_ERR_VERIFY;
      return ERASE_ERR_VERIFY;
    }
  }

  Job->Report->Status = ERASE_OK;
  return ERASE_OK;
}
