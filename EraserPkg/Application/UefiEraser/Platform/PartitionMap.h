/** @file
  Platform/PartitionMap.h - GPT/MBR partition table reading, for display only.

  The eraser does not need this to work: a partition's own BlockIo already
  bounds it exactly. This exists so the target tree can say "EFI System" or a
  GPT partition name instead of "分区 3", and so the confirmation dialog can
  show the user what they are about to destroy.

  Everything here is best-effort. A malformed or absent partition table is not
  an error: the caller gets "unknown" names and whole-disk erasure still works.

  Copyright (c) 2026, Mike Wu. All rights reserved.
**/
#ifndef ERASE_PLATFORM_PARTITION_MAP_H
#define ERASE_PLATFORM_PARTITION_MAP_H

#include <Uefi.h>
#include <Protocol/BlockIo.h>
#include "../Core/Port.h"

#define PARTMAP_MAX_ENTRIES  32
#define PARTMAP_NAME_CHARS   48
#define PARTMAP_TYPE_CHARS   40

typedef struct {
  UINT32  Index;                     /**< 1-based, as in the partition table */
  UINT64  FirstLba;
  UINT64  LastLba;                   /**< inclusive */
  CHAR16  Name[PARTMAP_NAME_CHARS];  /**< GPT partition name, or "" */
  CHAR16  Type[PARTMAP_TYPE_CHARS];  /**< friendly type, or "unknown" */
} PARTMAP_ENTRY;

typedef struct {
  BOOLEAN       IsGpt;
  BOOLEAN       Valid;
  /** The media was read, so this map is the complete truth about the disk -
      including the verdict "there is no usable partition table here".

      Callers that have to decide what still exists (see AppRefreshTargets) key
      off this flag. A partition handle outlives the table it was built from:
      the firmware goes on offering a BlockIo for partitions that an erasure
      has already destroyed, so its handle database cannot be the reference.
      The bytes at LBA 0 can. FALSE means the read itself failed and nothing may
      be concluded. */
  BOOLEAN       Authoritative;
  UINT32        Count;
  PARTMAP_ENTRY Entries[PARTMAP_MAX_ENTRIES];
} PARTMAP;

/** Parse the partition table of a whole-disk device.

    Reads LBA 0 and, when it is a protective MBR, the GPT header at LBA 1 plus
    the entry array. Verifies the GPT header CRC32 and the entry-array CRC32;
    a mismatch marks the map Valid but leaves Count at 0, because a torn table
    is exactly the state where guessing would be dangerous.

    @param[in]  Disk    a whole-disk device (LogicalPartition == FALSE)
    @param[out] Map     zeroed, then filled
*/
EFI_STATUS
PartMapRead (
  IN  EFI_BLOCK_IO_PROTOCOL  *Bio,
  IN  UINT64                  DiskLength,
  OUT PARTMAP                *Map
  );

/** Friendly name for a GPT type GUID, ASCII-ish, or NULL when unknown. */
CONST CHAR16 *
PartMapTypeName (
  IN CONST EFI_GUID  *Type
  );

#endif /* ERASE_PLATFORM_PARTITION_MAP_H */
