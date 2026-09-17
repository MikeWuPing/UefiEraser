/** @file
  Platform/BlockDevice.h - block device enumeration and raw I/O.

  Covers both whole physical disks (Media->LogicalPartition == FALSE) and the
  partitions on them (TRUE). A partition's EFI_BLOCK_IO_PROTOCOL already covers
  exactly its own LBA range, so erasing "a partition" needs no special
  arithmetic - writing LBA 0..LastBlock of the partition handle stays inside the
  partition by construction. That is what makes the boundary guarantee in
  tools/Verify-Erased.py achievable.

  Copyright (c) 2026, Mike Wu. All rights reserved.
**/
#ifndef ERASE_PLATFORM_BLOCK_DEVICE_H
#define ERASE_PLATFORM_BLOCK_DEVICE_H

#include <Uefi.h>
/* <Uefi.h> does NOT declare the protocol structs - EFI_BLOCK_IO_PROTOCOL and
   EFI_DEVICE_PATH_PROTOCOL come from these headers. Omitting them produces a
   cascade of C2061/C2059 syntax errors pointing at every use of the type. */
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>

#include "../Core/Port.h"
#include "../Core/TargetModel.h"

/** Upper bound on enumerated targets. A machine with more block devices than
    this is possible but absurd; overflow is reported rather than truncated
    silently. */
#define BLOCK_DEV_MAX  64

typedef struct {
  EFI_BLOCK_IO_PROTOCOL  *BlockIo;
  EFI_HANDLE              Handle;
  EFI_HANDLE              ParentHandle;   /**< whole-disk handle, or NULL */
  EFI_DEVICE_PATH_PROTOCOL *DevicePath;
  UINT64                  Offset;         /**< always 0: BlockIo is relative */
  UINT64                  Length;
  UINT32                  BlockSize;
  UINT32                  PartitionNo;    /**< 0 for a whole disk */
  /** Where this partition sits on its parent disk, straight out of the HD node
      of its device path. 0 when the path did not say - see Blocks. */
  UINT64                  FirstLba;
  UINT64                  Blocks;         /**< partition size in blocks; 0 when
                                               the device path did not say, in
                                               which case the range is unknown */
  BOOLEAN                 LogicalPartition;
  BOOLEAN                 ReadOnly;
  BOOLEAN                 Removable;
  BOOLEAN                 IsBootVolume;
} BLOCK_DEV;

typedef struct {
  BLOCK_DEV  Items[BLOCK_DEV_MAX];
  UINT32     Count;
  BOOLEAN    Overflowed;
} BLOCK_DEV_LIST;

/** Enumerate every usable block device.

    Skips handles with no media present. Sizes targets and resolves, for each
    partition, the parent whole-disk handle and its partition number (from the
    HD device-path node). Flags the device the running image came from so the
    UI can exclude it by default.

    @param[out] List         filled in; zeroed first
    @param[in]  ImageHandle  the running application, for boot-volume detection
*/
EFI_STATUS
BlockDevEnumerate (
  OUT BLOCK_DEV_LIST  *List,
  IN  EFI_HANDLE       ImageHandle
  );

/** Raw read/write/flush. Offset and Len are byte-based and are split into
    BlockSize-aligned transfers internally.

    BlockIo rejects unaligned transfers, and a target length is rarely a whole
    number of blocks, so the tail is handled with a read-modify-write: read the
    containing block, patch the covered bytes, write it back. That keeps the
    erasure's boundary exact instead of silently rounding up and destroying a
    neighbour's data. */
EFI_STATUS
BlockDevRead (
  IN  BLOCK_DEV  *Dev,
  IN  UINT64      Offset,
  IN  UINTN       Len,
  OUT VOID       *Buf
  );

EFI_STATUS
BlockDevWrite (
  IN BLOCK_DEV  *Dev,
  IN UINT64      Offset,
  IN UINTN       Len,
  IN CONST VOID *Buf
  );

EFI_STATUS
BlockDevFlush (
  IN BLOCK_DEV  *Dev
  );

/** Re-read Media and fail with EFI_MEDIA_CHANGED if MediaId moved under us.
    Called before a destructive job starts and periodically during it: on
    removable media a mid-erase swap would otherwise send the remaining passes
    to a different device. */
EFI_STATUS
BlockDevCheckMedia (
  IN BLOCK_DEV  *Dev
  );

/** Render a byte count as a human string: "500 GB", "1.5 TB", "512 MB".
    Binary units (KiB/MiB/GiB) scaled to the nearest sensible one. */
VOID
BlockDevFormatSize (
  IN  UINT64  Bytes,
  OUT CHAR16  *Out,
  IN  UINTN   OutChars
  );

#endif /* ERASE_PLATFORM_BLOCK_DEVICE_H */
