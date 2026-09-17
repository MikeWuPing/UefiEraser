/** @file
  Core/TargetModel.h - what the user can point the eraser at.

  Deliberately free of UEFI protocol types so the Ui layer can hold a plain
  array of these without dragging in <Uefi.h> details; the Platform layer fills
  them in. EFI_HANDLE is opaque, so it is carried as VOID *.

  Copyright (c) 2026, Mike Wu. All rights reserved.
**/
#ifndef ERASE_CORE_TARGET_MODEL_H
#define ERASE_CORE_TARGET_MODEL_H

#include "Port.h"

typedef enum {
  TARGET_KIND_DISK = 0,   /**< a whole physical device, LBA 0..LastBlock */
  TARGET_KIND_PARTITION,  /**< one partition, its own LBA range */
  TARGET_KIND_FREESPACE   /**< unused space on a mounted volume */
} TARGET_KIND;

/** Longest display name we keep (CHAR16 units, so plenty for CJK). */
#define TARGET_NAME_MAX    80
/** Second line: model/serial for disks, type/offset for partitions. */
#define TARGET_DETAIL_MAX  128
#define TARGET_PATH_MAX    256

typedef struct {
  TARGET_KIND  Kind;
  VOID        *Handle;        /**< EFI_HANDLE carrying BlockIo (or the volume) */
  VOID        *ParentHandle;  /**< for partitions: the whole-disk handle */
  E_U64        Offset;        /**< first byte of the target region */
  E_U64        Length;        /**< bytes covered */
  E_U32        BlockSize;     /**< Media->BlockSize */
  E_U32        PartitionNo;   /**< 0 for a whole disk */
  E_BOOL       ReadOnly;      /**< Media->ReadOnly */
  E_BOOL       Removable;     /**< Media->RemovableMedia */
  E_BOOL       IsBootVolume;  /**< the app itself runs from here */
  E_BOOL       Selected;      /**< UI selection state */
  E_BOOL       FreespaceTarget; /**< only meaningful for TARGET_KIND_FREESPACE */
  CHAR16       Name[TARGET_NAME_MAX];
  CHAR16       Detail[TARGET_DETAIL_MAX];
  CHAR16       PathText[TARGET_PATH_MAX];
} ERASE_TARGET;

/** A list of targets plus its capacity. */
typedef struct {
  ERASE_TARGET *Items;
  E_U32         Count;
  E_U32         Capacity;
} ERASE_TARGET_LIST;

/** Kind name for the UI/logs. */
const char *
TargetKindName (
  TARGET_KIND  Kind
  );

#endif /* ERASE_CORE_TARGET_MODEL_H */
