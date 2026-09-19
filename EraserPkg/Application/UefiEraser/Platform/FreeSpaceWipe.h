/** @file
  Platform/FreeSpaceWipe.h - erasing a volume's unused space.

  Method: fill the volume with temporary files written in the algorithm's
  pattern, then delete them; repeat once per pass. This is exactly what Eraser
  does (its ErasureMethodBase documents FreeSpaceFileUnit as "the size of each
  temp file used when erasing unused space"), and it is deliberately
  filesystem-agnostic: it never parses a FAT or an allocation table, so it works
  the same on FAT12/16/32 and exFAT and cannot corrupt a volume by
  misunderstanding its metadata.

  What it does and does not do: it overwrites the clusters that are currently
  FREE, which is where deleted files' contents still sit. It does not touch live
  files - those are left byte-identical, and tools/Verify-FreeSpace.py asserts
  exactly that.

  Copyright (c) 2026, Mike Wu. All rights reserved.
**/
#ifndef ERASE_PLATFORM_FREE_SPACE_WIPE_H
#define ERASE_PLATFORM_FREE_SPACE_WIPE_H

#include <Uefi.h>
#include "../Core/Port.h"
#include "../Core/EraseAlgo.h"
#include "../Core/EraseEngine.h"
#include "OverwriteErase.h"

/** How much to write per Write() call. 1 MiB keeps the call count sane on a
    slow emulated disk without holding a large pool allocation. */
#define FREESPACE_CHUNK_BYTES  (1024u * 1024u)

/** Target size of one temp file. Deliberately modest so that a small volume
    still gets a useful number of files, and so a cancelled run leaves less to
    clean up. The volume's own free space decides the real total. */
#define FREESPACE_FILE_UNIT    (64u * 1024u * 1024u)

/** Upper bound on temp files per pass.

    Filling a volume needs the files to ACCUMULATE until it is full - deleting
    each one as it is written frees the space again and the loop never ends.
    The cap therefore bounds how much free space one pass can cover:
    FREESPACE_MAX_FILES * FREESPACE_FILE_UNIT = 32 GiB. A larger volume is
    reported as partially covered rather than silently under-wiped. */
#define FREESPACE_MAX_FILES    512u

/** Wipe the free space of the volume behind `VolumeHandle`.

    @param[in]  VolumeHandle  a handle carrying EFI_SIMPLE_FILE_SYSTEM_PROTOCOL
    @param[in]  Algo          algorithm whose pass patterns are written
    @param[in]  CustomPasses  pass count for the custom method
    @param[in]  Seed          PRNG seed
    @param[in]  Pump          UI pump (may be NULL; progress then goes to serial)
    @param[in]  PumpCtx       context for Pump
    @param[out] Report        filled in; BytesWritten is the total across passes

    @retval EFI_SUCCESS        every pass completed (a full volume is success,
                               not an error - that is the point)
    @retval EFI_ABORTED        cancelled
    @retval EFI_UNSUPPORTED    the volume is not writable or cannot be opened
    @retval others             an I/O failure
*/
EFI_STATUS
FreeSpaceWipeVolume (
  IN  EFI_HANDLE              VolumeHandle,
  IN  CONST ERASE_ALGORITHM  *Algo,
  IN  UINT32                  CustomPasses,
  IN  UINT64                  Seed,
  IN  ERASE_PUMP_CB           Pump,
  IN  VOID                   *PumpCtx,
  OUT ERASE_REPORT           *Report
  );

#endif /* ERASE_PLATFORM_FREE_SPACE_WIPE_H */
