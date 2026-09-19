/** @file
  Core/EraseEngine.h - pass driver for multi-pass overwrite erasure.

  The engine knows nothing about UEFI: I/O arrives through function pointers,
  and the caller owns the work buffer. That is what makes the algorithm and
  progress logic unit-testable on the host against an in-memory buffer, with no
  emulator in the loop.

  Pass semantics (one pass rewrites the whole target range):
    - every pass rewrites the WHOLE target range from TargetOffset
    - PASS_CONSTANT repeats its pattern (period 1..3) across each chunk, and
      because ChunkSize is a multiple of 3, 4, 512 and 1024 the pattern never
      shifts phase at a chunk boundary
    - PASS_RANDOM advances one PRNG stream per pass
    - PASS_CONST_RANDOM draws one byte per erasure (resolved before pass 1)
    - the last pass is read back and compared against a replay of its own
      generator when VerifyLastPass is set

  Copyright (c) 2026, Mike Wu. All rights reserved.
**/
#ifndef ERASE_CORE_ERASE_ENGINE_H
#define ERASE_CORE_ERASE_ENGINE_H

#include "Port.h"
#include "Prng.h"
#include "EraseAlgo.h"

/* ------------------------------------------------------------- callbacks */

/** Write Len bytes at Offset. Return ERASE_OK or an error to abort. */
typedef ERASE_STATUS (*ERASE_WRITE_CB) (
                       void         *Ctx,
                       E_U64         Offset,
                       E_SIZE        Len,
                       const E_U8   *Data
                       );

/** Read Len bytes from Offset (only needed when VerifyLastPass is set). */
typedef ERASE_STATUS (*ERASE_READ_CB) (
                       void         *Ctx,
                       E_U64         Offset,
                       E_SIZE        Len,
                       E_U8         *Data
                       );

/** Progress tick. `Done`/`Total` are bytes across the whole job (all passes),
    so the UI can show a single bar. Called at least once per chunk. */
typedef void (*ERASE_PROGRESS_CB) (
                void    *Ctx,
                E_U64    Done,
                E_U64    Total,
                E_U32    Pass,       /**< 1-based */
                E_U32    PassCount
                );

/** Polled before every chunk. Returning E_TRUE aborts with
    ERASE_ERR_CANCELLED. */
typedef E_BOOL (*ERASE_CANCEL_CB) (
                 void  *Ctx
                 );

/* ---------------------------------------------------------------- report */

typedef struct {
  const char   *AlgoId;            /**< points at the static table entry */
  E_U64         TargetOffset;
  E_U64         TargetLength;
  E_U64         TotalBytesPlanned; /**< TargetLength * passes */
  E_U64         BytesWritten;
  E_U64         BytesVerified;
  E_U32         PassCount;
  E_U32         PassesCompleted;
  E_U32         VerifyMismatches;
  E_BOOL        Cancelled;
  E_BOOL        Verified;
  ERASE_STATUS  Status;
  E_U64         FailedOffset;      /**< valid when Status is an error */
  /** Why it failed, when the layer that ran the command knows and the status
      alone would hide it. ASCII, points at a static string, NULL otherwise.

      ERASE_STATUS is deliberately coarse: "the command did not do what was
      asked" is I/O error whatever the cause. For the device-level modes the
      cause is the whole message though - an ATA disk with no Security feature
      set and a frozen one both fail, and the user can act on the difference.
      The console log and the report dialog show this line. */
  const char   *Reason;
} ERASE_REPORT;

/* ------------------------------------------------------------------- job */

typedef struct {
  const ERASE_ALGORITHM  *Algo;
  E_U32                   CustomRandomPasses;  /**< used when Algo->PassCount == 0 */
  E_U64                   TargetOffset;
  E_U64                   TargetLength;
  E_SIZE                  ChunkSize;           /**< Buffer must be this big */
  E_BOOL                  VerifyLastPass;
  E_U64                   Seed;                /**< PRNG seed for this erasure */
  E_U8                   *Buffer;              /**< caller-owned scratch; must be
                                                    2 * ChunkSize when
                                                    VerifyLastPass is set (the
                                                    second half holds the bytes
                                                    read back for comparison) */
  void                   *IoContext;
  ERASE_WRITE_CB          Write;
  ERASE_READ_CB           Read;
  ERASE_PROGRESS_CB       Progress;
  ERASE_CANCEL_CB         IsCancelled;
  ERASE_REPORT           *Report;
} ERASE_JOB;

/** The chunk size Eraser uses (DiskOperationUnit = 1536 * 4096 = 6 MiB). It is
    divisible by 3, 4, 512 and 1024, which is exactly what the 3-byte Gutmann
    patterns and every common sector size need to stay in phase across chunk
    boundaries. */
#define ERASE_CHUNK_SIZE   (1536u * 4096u)

/** Run the job. Fills *Job->Report. Never allocates.

    @retval ERASE_OK              every pass completed (and verified, if asked)
    @retval ERASE_ERR_CANCELLED   the cancel callback asked to stop; the report
                                  still records how far it got
    @retval ERASE_ERR_IO          a read/write callback failed
    @retval ERASE_ERR_VERIFY      the last pass did not read back as written
    @retval ERASE_ERR_PARAM       Buffer/Write/Algo missing, or ChunkSize 0
*/
ERASE_STATUS
EraseJobRun (
  ERASE_JOB  *Job
  );

/** Number of passes this job will run (handles the custom-random case). */
E_U32
EraseJobPassCount (
  const ERASE_JOB  *Job
  );

#endif /* ERASE_CORE_ERASE_ENGINE_H */
