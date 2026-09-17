/** @file
  Platform/HeadlessErase.h - command-line driven erasure, no UI.

  Why this exists: the byte-level proof that an erasure really happened comes
  from tools/Verify-Erased.py reading the target image afterwards. Driving the
  three-gate confirmation dialog through QMP mouse events to reach that state
  would make the most important assertion in the project depend on pixel-exact
  clicking - brittle, and it would break silently on any layout change.

  So the app has a headless mode: `UefiEraser.efi erase=disk:1 algo=...` runs
  the identical Core engine and Platform I/O with no UI, logs everything to the
  serial channel, and exits. The interactive path is verified separately by
  screenshots, which is what screenshots are good for.

  Arguments (space separated, order independent):
    erase=disk:<i>          i-th whole disk in enumeration order (0-based)
    erase=part:<i>:<n>      partition n of whole disk i
    algo=<id>               algorithm id from Core/EraseAlgo.c (default
                            dod522022m)
    custom=<n>              pass count for the custom method
    verify                  read the last pass back and compare

  Copyright (c) 2026, Mike Wu. All rights reserved.
**/
#ifndef ERASE_PLATFORM_HEADLESS_ERASE_H
#define ERASE_PLATFORM_HEADLESS_ERASE_H

#include <Uefi.h>
#include "../Core/Port.h"
#include "BlockDevice.h"

/** TRUE when the command line asks for a headless erasure.

    UefiMain uses this to decide whether to build the UI at all, so a headless
    run never initialises the display. */
BOOLEAN
HeadlessEraseRequested (
  IN EFI_HANDLE  ImageHandle
  );

/** Run the erasure described on the command line and return.

    Logs APP_VERSION-style markers to the serial channel:
      [Headless] ERASE BEGIN target=... algo=... passes=...
      [Headless] ERASE DONE  status=... written=... verified=...
    so the QEMU harness can assert on them.

    @retval EFI_SUCCESS  the erasure completed (verify included)
    @retval EFI_ABORTED  the target selection could not be resolved
    @retval others       the underlying block I/O failure
*/
EFI_STATUS
HeadlessEraseRun (
  IN EFI_HANDLE         ImageHandle,
  IN BLOCK_DEV_LIST     *Devs
  );

#endif /* ERASE_PLATFORM_HEADLESS_ERASE_H */
