/** @file
  Platform/OverwriteErase.h - the overwrite erasure session.

  Bridges the UEFI-free Core engine to a real block device: owns the work
  buffer, translates block I/O into the engine's callbacks, tracks throughput
  and ETA, and pumps the UI from inside the progress callback.

  Why the UI is pumped from the progress callback rather than from a separate
  thread: UEFI has no threads and the erasure is a long synchronous loop, so if
  the callback did not pump LVGL the window would freeze and - worse - the
  Cancel button would never be read. `Pump` is supplied by the Ui layer, which
  keeps Platform from depending on Ui.

  Copyright (c) 2026, Mike Wu. All rights reserved.
**/
#ifndef ERASE_PLATFORM_OVERWRITE_ERASE_H
#define ERASE_PLATFORM_OVERWRITE_ERASE_H

#include <Uefi.h>
#include "../Core/Port.h"
#include "../Core/EraseAlgo.h"
#include "../Core/EraseEngine.h"
#include "BlockDevice.h"

typedef struct {
  UINT64  Done;        /**< bytes written across all passes */
  UINT64  Total;       /**< bytes the whole job will write */
  UINT32  Pass;        /**< 1-based current pass */
  UINT32  PassCount;
  UINT64  BytesPerSec; /**< measured over the last sampling window */
  UINT32  EtaSec;      /**< 0 when not yet estimable */
  UINT32  Percent;     /**< 0..100 */
} ERASE_PROGRESS;

/** Called from inside the erasure loop so the UI can redraw and let the user
    press Cancel. Must not block. */
typedef VOID (*ERASE_PUMP_CB) (
                VOID  *Ctx
                );

typedef struct {
  BLOCK_DEV              *Dev;
  const ERASE_ALGORITHM  *Algo;
  UINT32                  CustomPasses;   /**< for the custom method */
  BOOLEAN                 Verify;

  ERASE_JOB               Job;
  ERASE_REPORT            Report;
  ERASE_PROGRESS          Progress;

  UINT8                  *Buffer;
  UINTN                   BufferBytes;

  BOOLEAN                 CancelRequested;  /**< set by the UI, polled by the engine */
  BOOLEAN                 Running;
  BOOLEAN                 Finished;
  BOOLEAN                 MediaChanged;

  ERASE_PUMP_CB           Pump;
  VOID                   *PumpCtx;

  UINT64                  StartMs;
  UINT64                  LastSampleMs;
  UINT64                  LastSampleDone;
} ERASE_SESSION;

/** Prepare a session. Allocates the work buffer (ChunkSize, doubled when
    verifying) and zeroes the report. Does NOT touch the device.

    @retval EFI_SUCCESS           ready to run
    @retval EFI_OUT_OF_RESOURCES  buffer allocation failed
    @retval EFI_INVALID_PARAMETER Dev/Algo NULL, or the device is read-only
*/
EFI_STATUS
EraseSessionInit (
  OUT ERASE_SESSION             *S,
  IN  BLOCK_DEV                 *Dev,
  IN  CONST ERASE_ALGORITHM     *Algo,
  IN  UINT32                     CustomPasses,
  IN  BOOLEAN                    Verify,
  IN  ERASE_PUMP_CB              Pump,
  IN  VOID                      *PumpCtx
  );

/** Release the work buffer. Safe to call on a zeroed or already-freed session. */
VOID
EraseSessionFree (
  IN OUT ERASE_SESSION  *S
  );

/** Run the erasure to completion (or to cancellation).

    Blocks until done. Pumps the UI through `Pump` on every progress tick and
    aborts with EFI_ABORTED when `CancelRequested` becomes TRUE.

    @retval EFI_SUCCESS           all passes written (and verified if asked)
    @retval EFI_ABORTED           cancelled; S->Report records how far it got
    @retval EFI_MEDIA_CHANGED     the media changed under us
    @retval others                a block I/O failure; S->Report has the offset
*/
EFI_STATUS
EraseSessionRun (
  IN OUT ERASE_SESSION  *S
  );

#endif /* ERASE_PLATFORM_OVERWRITE_ERASE_H */
