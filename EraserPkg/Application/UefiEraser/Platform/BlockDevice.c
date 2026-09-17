/** @file
  Platform/BlockDevice.c - see BlockDevice.h.

  Copyright (c) 2026, Mike Wu. All rights reserved.
**/

#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/PrintLib.h>
#include <Library/DebugLib.h>

#include <Protocol/BlockIo.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/DevicePath.h>

#include "BlockDevice.h"

/* ------------------------------------------------------------- device path */

/** True when `Prefix` names the same device as, or an ancestor of, `Path`.

    Device paths are node chains, so an ancestor test is a node-by-node walk
    that stops when the prefix ends. This is how the boot volume is identified:
    the running image's path is PciRoot/.../HD(1,GPT,...)/File(\UefiEraser.efi)
    and the partition's own path is that same chain minus the File node. */
STATIC
BOOLEAN
DevicePathIsPrefix (
  IN EFI_DEVICE_PATH_PROTOCOL  *Prefix,
  IN EFI_DEVICE_PATH_PROTOCOL  *Path
  )
{
  if (Prefix == NULL || Path == NULL) {
    return FALSE;
  }
  while (!IsDevicePathEnd (Prefix)) {
    if (IsDevicePathEnd (Path)) {
      return FALSE;                     /* Path ran out first */
    }
    if (DevicePathNodeLength (Prefix) != DevicePathNodeLength (Path) ||
        CompareMem (Prefix, Path, DevicePathNodeLength (Prefix)) != 0) {
      return FALSE;
    }
    Prefix = NextDevicePathNode (Prefix);
    Path   = NextDevicePathNode (Path);
  }
  return TRUE;
}

/** The LBA range a partition's device path says it covers, when it says.
    PartitionStart and PartitionSize are in blocks of the parent disk.

    This is the handle a partition table can be checked against: a table
    describes regions, and it says nothing about the partition numbers the
    firmware chose (an MBR extended region hands out numbers 5 and up that no
    primary entry mentions). */
STATIC
BOOLEAN
PartitionRangeFromPath (
  IN  EFI_DEVICE_PATH_PROTOCOL  *Dp,
  OUT UINT64                    *StartLba,
  OUT UINT64                    *Blocks
  )
{
  while (Dp != NULL && !IsDevicePathEnd (Dp)) {
    if (DevicePathType (Dp) == MEDIA_DEVICE_PATH &&
        DevicePathSubType (Dp) == MEDIA_HARDDRIVE_DP)
    {
      HARDDRIVE_DEVICE_PATH  *Hd = (HARDDRIVE_DEVICE_PATH *)Dp;

      *StartLba = (UINT64)Hd->PartitionStart;
      *Blocks   = (UINT64)Hd->PartitionSize;
      return TRUE;
    }
    Dp = NextDevicePathNode (Dp);
  }
  return FALSE;
}

/** Partition number from the HD node of a device path, or 0 when absent. */
STATIC
UINT32
PartitionNumberFromPath (
  IN EFI_DEVICE_PATH_PROTOCOL  *Dp
  )
{
  while (Dp != NULL && !IsDevicePathEnd (Dp)) {
    if (DevicePathType (Dp) == MEDIA_DEVICE_PATH &&
        DevicePathSubType (Dp) == MEDIA_HARDDRIVE_DP)
    {
      return ((HARDDRIVE_DEVICE_PATH *)Dp)->PartitionNumber;
    }
    Dp = NextDevicePathNode (Dp);
  }
  return 0;
}

/* ---------------------------------------------------------------- helpers */

/** Open BlockIo on a handle, returning NULL when it is not usable. */
STATIC
EFI_BLOCK_IO_PROTOCOL *
BlockIoOn (
  IN EFI_HANDLE  Handle
  )
{
  EFI_STATUS             Status;
  EFI_BLOCK_IO_PROTOCOL  *Bio = NULL;

  Status = gBS->OpenProtocol (
                  Handle,
                  &gEfiBlockIoProtocolGuid,
                  (VOID **)&Bio,
                  gImageHandle,
                  NULL,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  if (EFI_ERROR (Status) || Bio == NULL || Bio->Media == NULL) {
    return NULL;
  }
  if (!Bio->Media->MediaPresent) {
    return NULL;
  }
  return Bio;
}

/** Second pass: link each partition to its whole-disk parent.

    A partition's device path is the disk's device path plus a trailing
    HD(...) node, so the parent is the whole-disk device whose path is the
    longest proper prefix of the partition's path. Device-path prefix matching
    is used rather than EFI_PARTITION_INFO_PROTOCOL because that protocol
    describes the partition entry, not its parent. */
STATIC
VOID
ResolveParents (
  IN OUT BLOCK_DEV_LIST  *List
  )
{
  UINT32 I;
  UINT32 J;

  for (I = 0; I < List->Count; I++) {
    BLOCK_DEV  *Part = &List->Items[I];
    BLOCK_DEV  *Best = NULL;
    UINTN       BestLen = 0;

    if (!Part->LogicalPartition || Part->DevicePath == NULL) {
      continue;
    }

    for (J = 0; J < List->Count; J++) {
      BLOCK_DEV  *Disk = &List->Items[J];
      UINTN       Len;

      if (Disk->LogicalPartition || Disk->DevicePath == NULL) {
        continue;
      }
      if (!DevicePathIsPrefix (Disk->DevicePath, Part->DevicePath)) {
        continue;
      }

      /* Longest matching prefix wins, so a nested layout (a partition on a
         partition, which some firmware exposes) picks the nearest ancestor. */
      Len = GetDevicePathSize (Disk->DevicePath);
      if (Best == NULL || Len > BestLen) {
        Best    = Disk;
        BestLen = Len;
      }
    }

    if (Best != NULL) {
      Part->ParentHandle = Best->Handle;
    }
  }
}

/* ------------------------------------------------------------- enumerate */

EFI_STATUS
BlockDevEnumerate (
  OUT BLOCK_DEV_LIST  *List,
  IN  EFI_HANDLE       ImageHandle
  )
{
  EFI_STATUS                 Status;
  EFI_HANDLE                *Handles = NULL;
  UINTN                      HandleCount = 0;
  UINTN                      I;
  EFI_LOADED_IMAGE_PROTOCOL *Loaded = NULL;
  EFI_DEVICE_PATH_PROTOCOL  *BootPath = NULL;

  if (List == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  ZeroMem (List, sizeof (*List));

  /* Where did we come from? Used to flag the boot volume. Note that
     EFI_LOADED_IMAGE_PROTOCOL carries DeviceHandle + FilePath, NOT a
     DevicePath member - the path belongs to the device handle. */
  Status = gBS->OpenProtocol (
                  ImageHandle,
                  &gEfiLoadedImageProtocolGuid,
                  (VOID **)&Loaded,
                  ImageHandle,
                  NULL,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  if (!EFI_ERROR (Status) && Loaded != NULL && Loaded->DeviceHandle != NULL) {
    gBS->OpenProtocol (
           Loaded->DeviceHandle,
           &gEfiDevicePathProtocolGuid,
           (VOID **)&BootPath,
           ImageHandle,
           NULL,
           EFI_OPEN_PROTOCOL_GET_PROTOCOL
           );
  }

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiBlockIoProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[BlockDevice] LocateHandleBuffer(BlockIo): %r\n", Status));
    return Status;
  }

  for (I = 0; I < HandleCount; I++) {
    EFI_BLOCK_IO_PROTOCOL    *Bio;
    EFI_DEVICE_PATH_PROTOCOL *Dp = NULL;
    BLOCK_DEV                *Dev;

    Bio = BlockIoOn (Handles[I]);
    if (Bio == NULL) {
      continue;
    }

    if (List->Count >= BLOCK_DEV_MAX) {
      List->Overflowed = TRUE;
      break;
    }

    gBS->OpenProtocol (
           Handles[I],
           &gEfiDevicePathProtocolGuid,
           (VOID **)&Dp,
           gImageHandle,
           NULL,
           EFI_OPEN_PROTOCOL_GET_PROTOCOL
           );

    Dev = &List->Items[List->Count];
    ZeroMem (Dev, sizeof (*Dev));
    Dev->BlockIo          = Bio;
    Dev->Handle           = Handles[I];
    Dev->DevicePath       = Dp;
    Dev->Offset           = 0;
    Dev->BlockSize        = Bio->Media->BlockSize;
    Dev->LogicalPartition = Bio->Media->LogicalPartition;
    Dev->ReadOnly         = Bio->Media->ReadOnly;
    Dev->Removable        = Bio->Media->RemovableMedia;
    if (Dev->LogicalPartition) {
      UINT64  StartLba = 0;
      UINT64  Blocks   = 0;

      Dev->PartitionNo = PartitionNumberFromPath (Dp);
      if (PartitionRangeFromPath (Dp, &StartLba, &Blocks)) {
        Dev->FirstLba = StartLba;
        Dev->Blocks   = Blocks;
      }
    }
    Dev->ParentHandle     = NULL;   /* resolved in the second pass below */

    /* (LastBlock + 1) * BlockSize, in 64-bit: LastBlock is EFI_LBA (UINT64) and
       a large drive times 4096 overflows a UINT32. */
    Dev->Length = ((UINT64)Bio->Media->LastBlock + 1) * (UINT64)Bio->Media->BlockSize;

    /* A partition whose parent lookup failed is still erasable (its BlockIo is
       self-contained); only the tree grouping is lost. */
    Dev->IsBootVolume = DevicePathIsPrefix (Dp, BootPath);

    List->Count++;
  }

  FreePool (Handles);

  ResolveParents (List);

  DEBUG ((
    DEBUG_INFO,
    "[BlockDevice] %u target(s)%a\n",
    (UINT32)List->Count,
    List->Overflowed ? " (TRUNCATED at BLOCK_DEV_MAX)" : ""
    ));

  for (I = 0; I < List->Count; I++) {
    BLOCK_DEV *Dev = &List->Items[I];
    DEBUG ((
      DEBUG_INFO,
      "[BlockDevice]  %a #%u blk=%u size=%lu ro=%d rm=%d boot=%d\n",
      Dev->LogicalPartition ? "part" : "disk",
      Dev->PartitionNo,
      Dev->BlockSize,
      (UINT64)Dev->Length,
      Dev->ReadOnly,
      Dev->Removable,
      Dev->IsBootVolume
      ));
  }

  return EFI_SUCCESS;
}

/* ------------------------------------------------------------------- I/O */

STATIC
EFI_STATUS
AlignedTransfer (
  IN BLOCK_DEV  *Dev,
  IN UINT64      Offset,
  IN UINTN       Len,
  IN BOOLEAN     IsWrite,
  IN OUT VOID   *Buf
  )
{
  EFI_BLOCK_IO_PROTOCOL  *Bio = Dev->BlockIo;
  UINT32                  Bs  = Dev->BlockSize;
  UINT64                  Lba;
  UINTN                   Blocks;
  UINT8                  *Tail = NULL;
  UINT64                  TailLba;
  UINT32                  HeadSkip;
  UINT32                  HeadLen;
  EFI_STATUS              Status;

  if (Len == 0) {
    return EFI_SUCCESS;
  }
  if ((Offset + Len) > Dev->Length) {
    return EFI_INVALID_PARAMETER;
  }

  HeadSkip = (UINT32)(Offset % Bs);
  HeadLen  = (HeadSkip != 0) ? (Bs - HeadSkip) : 0;
  if (HeadLen > Len) {
    HeadLen = (UINT32)Len;
  }

  /* ---- head: the partial block at the start ---- */
  if (HeadSkip != 0) {
    UINT8 *Block = AllocatePool (Bs);
    if (Block == NULL) {
      return EFI_OUT_OF_RESOURCES;
    }
    Lba = Offset / Bs;
    Status = Bio->ReadBlocks (Bio, Bio->Media->MediaId, Lba, Bs, Block);
    if (EFI_ERROR (Status)) {
      FreePool (Block);
      return Status;
    }
    if (IsWrite) {
      CopyMem (Block + HeadSkip, Buf, HeadLen);
      Status = Bio->WriteBlocks (Bio, Bio->Media->MediaId, Lba, Bs, Block);
    } else {
      CopyMem (Buf, Block + HeadSkip, HeadLen);
      Status = EFI_SUCCESS;
    }
    FreePool (Block);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    Offset += HeadLen;
    Buf     = (UINT8 *)Buf + HeadLen;
    Len    -= HeadLen;
  }

  /* ---- middle: whole aligned blocks ---- */
  Blocks = Len / Bs;
  if (Blocks > 0) {
    Lba = Offset / Bs;
    if (IsWrite) {
      Status = Bio->WriteBlocks (Bio, Bio->Media->MediaId, Lba, (UINTN)Blocks * Bs, Buf);
    } else {
      Status = Bio->ReadBlocks (Bio, Bio->Media->MediaId, Lba, (UINTN)Blocks * Bs, Buf);
    }
    if (EFI_ERROR (Status)) {
      return Status;
    }
    Offset += (UINT64)Blocks * Bs;
    Buf     = (UINT8 *)Buf + (UINTN)Blocks * Bs;
    Len    -= (UINTN)Blocks * Bs;
  }

  /* ---- tail: the partial block at the end ---- */
  if (Len > 0) {
    TailLba = Offset / Bs;
    Tail    = AllocatePool (Bs);
    if (Tail == NULL) {
      return EFI_OUT_OF_RESOURCES;
    }
    Status = Bio->ReadBlocks (Bio, Bio->Media->MediaId, TailLba, Bs, Tail);
    if (EFI_ERROR (Status)) {
      FreePool (Tail);
      return Status;
    }
    if (IsWrite) {
      CopyMem (Tail, Buf, Len);
      Status = Bio->WriteBlocks (Bio, Bio->Media->MediaId, TailLba, Bs, Tail);
    } else {
      CopyMem (Buf, Tail, Len);
      Status = EFI_SUCCESS;
    }
    FreePool (Tail);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  return EFI_SUCCESS;
}

EFI_STATUS
BlockDevRead (
  IN  BLOCK_DEV  *Dev,
  IN  UINT64      Offset,
  IN  UINTN       Len,
  OUT VOID       *Buf
  )
{
  if (Dev == NULL || Buf == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  return AlignedTransfer (Dev, Offset, Len, FALSE, Buf);
}

EFI_STATUS
BlockDevWrite (
  IN BLOCK_DEV  *Dev,
  IN UINT64      Offset,
  IN UINTN       Len,
  IN CONST VOID *Buf
  )
{
  if (Dev == NULL || Buf == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if (Dev->ReadOnly) {
    return EFI_WRITE_PROTECTED;
  }
  /* The head/tail read-modify-write needs the block to be readable, so a write
     to a write-protected-but-readable device would silently succeed on the
     aligned part and fail on the tail. Refusing up front is honest. */
  return AlignedTransfer (Dev, Offset, Len, TRUE, (VOID *)Buf);
}

EFI_STATUS
BlockDevFlush (
  IN BLOCK_DEV  *Dev
  )
{
  if (Dev == NULL || Dev->BlockIo == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  return Dev->BlockIo->FlushBlocks (Dev->BlockIo);
}

EFI_STATUS
BlockDevCheckMedia (
  IN BLOCK_DEV  *Dev
  )
{
  EFI_BLOCK_IO_MEDIA  *Media;
  UINT32               OldId;

  if (Dev == NULL || Dev->BlockIo == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  Media = Dev->BlockIo->Media;
  OldId = Media->MediaId;

  /* There is no explicit "refresh" call; ReadBlocks re-reads the media state
     as a side effect, so probe one block. A zero-size device would make this
     meaningless, hence the guard. */
  if (Media->LastBlock > 0 && Media->BlockSize > 0) {
    UINT8 *Probe = AllocatePool (Media->BlockSize);
    if (Probe != NULL) {
      Dev->BlockIo->ReadBlocks (Dev->BlockIo, Media->MediaId, 0, Media->BlockSize, Probe);
      FreePool (Probe);
    }
  }

  if (!Media->MediaPresent || Media->MediaId != OldId) {
    return EFI_MEDIA_CHANGED;
  }
  return EFI_SUCCESS;
}

/* -------------------------------------------------------------- formatting */

VOID
BlockDevFormatSize (
  IN  UINT64  Bytes,
  OUT CHAR16  *Out,
  IN  UINTN   OutChars
  )
{
  UINT64  Whole;
  UINT64  Tenth;

  if (Out == NULL || OutChars == 0) {
    return;
  }

  /* Binary units. One decimal place only for TB, where the extra digit
     actually tells the user something. */
  if (Bytes >= 1024ULL * 1024 * 1024 * 1024) {
    Whole = Bytes / (1024ULL * 1024 * 1024 * 1024);
    Tenth = ((Bytes % (1024ULL * 1024 * 1024 * 1024)) * 10) / (1024ULL * 1024 * 1024 * 1024);
    UnicodeSPrint (Out, OutChars * sizeof (CHAR16), L"%ld.%ld TB", Whole, Tenth);
  } else if (Bytes >= 1024ULL * 1024 * 1024) {
    Whole = (Bytes + (1024ULL * 1024 * 1024) / 2) / (1024ULL * 1024 * 1024);
    UnicodeSPrint (Out, OutChars * sizeof (CHAR16), L"%ld GB", Whole);
  } else if (Bytes >= 1024ULL * 1024) {
    Whole = (Bytes + (1024ULL * 1024) / 2) / (1024ULL * 1024);
    UnicodeSPrint (Out, OutChars * sizeof (CHAR16), L"%ld MB", Whole);
  } else if (Bytes >= 1024ULL) {
    Whole = (Bytes + 512) / 1024;
    UnicodeSPrint (Out, OutChars * sizeof (CHAR16), L"%ld KB", Whole);
  } else {
    UnicodeSPrint (Out, OutChars * sizeof (CHAR16), L"%ld B", Bytes);
  }
}
