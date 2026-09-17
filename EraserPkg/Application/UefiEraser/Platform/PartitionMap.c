/** @file
  Platform/PartitionMap.c - see PartitionMap.h.

  Copyright (c) 2026, Mike Wu. All rights reserved.
**/

#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseLib.h>        /* StrCpyS, ARRAY_SIZE, CompareGuid */
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/PrintLib.h>       /* UnicodeSPrint */

#include "PartitionMap.h"

#pragma pack(1)

typedef struct {
  UINT8   BootIndicator;
  UINT8   StartHead;
  UINT8   StartSector;
  UINT8   StartTrack;
  UINT8   OsType;
  UINT8   EndHead;
  UINT8   EndSector;
  UINT8   EndTrack;
  UINT32  StartingLba;
  UINT32  SizeInLba;
} MBR_ENTRY;

typedef struct {
  UINT8      BootCode[446];
  MBR_ENTRY  Partition[4];
  UINT16     Signature;
} MBR;

typedef struct {
  UINT64    Signature;          /* "EFI PART" */
  UINT32    Revision;
  UINT32    HeaderSize;
  UINT32    HeaderCrc32;
  UINT32    Reserved;
  UINT64    MyLba;
  UINT64    AlternateLba;
  UINT64    FirstUsableLba;
  UINT64    LastUsableLba;
  EFI_GUID  DiskGuid;
  UINT64    PartitionEntryLba;
  UINT32    NumberOfPartitionEntries;
  UINT32    SizeOfPartitionEntry;
  UINT32    PartitionEntryArrayCrc32;
} GPT_HEADER;

typedef struct {
  EFI_GUID  TypeGuid;
  EFI_GUID  UniqueGuid;
  UINT64    StartingLba;
  UINT64    EndingLba;
  UINT64    Attributes;
  CHAR16    PartitionName[36];
} GPT_ENTRY;

#pragma pack()

#define GPT_SIGNATURE  (0x5452415020494645ULL)   /* "EFI PART" little-endian */
#define MBR_SIGNATURE  0xAA55u
#define MBR_TYPE_GPT   0xEEu

/* ----------------------------------------------------------- type names */

typedef struct {
  EFI_GUID      Guid;
  CONST CHAR16  *Name;
} GUID_NAME;

/* The handful of types a user is actually likely to meet. Anything else shows
   as "unknown" with its raw GUID available in the detail line. */
STATIC CONST GUID_NAME mGptTypes[] = {
  { { 0xC12A7328, 0xF81F, 0x11D2, { 0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B } }, L"EFI 系统分区" },
  { { 0xEBD0A0A2, 0xB9E5, 0x4433, { 0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7 } }, L"基本数据分区" },
  { { 0xE3C9E316, 0x0B5C, 0x4DB8, { 0x81, 0x7D, 0xF9, 0x2D, 0xF0, 0x02, 0x15, 0xAE } }, L"Microsoft 保留" },
  { { 0xDE94BBA4, 0x06D1, 0x4D40, { 0xA1, 0x6A, 0xBF, 0xD5, 0x01, 0x79, 0xD6, 0xAC } }, L"Windows 恢复" },
  { { 0x21686148, 0x6449, 0x6E6F, { 0x74, 0x4E, 0x65, 0x6E, 0x65, 0x65, 0x64, 0x00 } }, L"BIOS 引导分区" },
  { { 0x0FC63DAF, 0x8483, 0x4772, { 0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4 } }, L"Linux 文件系统" },
  { { 0x0657FD6D, 0xA4AB, 0x43C4, { 0x84, 0xE5, 0x09, 0x33, 0xC8, 0x4B, 0x4F, 0x4F } }, L"Linux swap" },
  { { 0x48465300, 0x0000, 0x11AA, { 0xAA, 0x11, 0x00, 0x30, 0x65, 0x43, 0xEC, 0xAC } }, L"Apple HFS+" },
};

CONST CHAR16 *
PartMapTypeName (
  IN CONST EFI_GUID  *Type
  )
{
  UINTN I;

  if (Type == NULL) {
    return NULL;
  }
  for (I = 0; I < ARRAY_SIZE (mGptTypes); I++) {
    if (CompareGuid (Type, &mGptTypes[I].Guid)) {
      return mGptTypes[I].Name;
    }
  }
  return NULL;
}

STATIC
CONST CHAR16 *
MbrTypeName (
  IN UINT8  Type
  )
{
  switch (Type) {
    case 0x07: return L"NTFS / exFAT";
    case 0x0B:
    case 0x0C: return L"FAT32";
    case 0x01: return L"FAT12";
    case 0x04:
    case 0x06:
    case 0x0E: return L"FAT16";
    case 0x82: return L"Linux swap";
    case 0x83: return L"Linux";
    case 0xEE: return L"GPT 保护分区";
    default:   return NULL;
  }
}

/* ----------------------------------------------------------------- read */

STATIC
EFI_STATUS
ReadLba (
  IN  EFI_BLOCK_IO_PROTOCOL  *Bio,
  IN  UINT64                  Lba,
  IN  UINTN                   Len,
  OUT VOID                   *Buf
  )
{
  return Bio->ReadBlocks (Bio, Bio->Media->MediaId, Lba, Len, Buf);
}

/** Copy a NUL-terminated UTF-16 GPT name into the entry, trimming trailing
    spaces. GPT names are NOT guaranteed NUL-terminated within 36 chars, which
    is why this copies a bounded count rather than using StrCpyS. */
STATIC
VOID
CopyGptName (
  IN  CONST CHAR16  *Src,
  IN  UINTN          SrcChars,
  OUT CHAR16        *Dst,
  IN  UINTN          DstChars
  )
{
  UINTN Len = 0;
  UINTN I;

  for (I = 0; I < SrcChars && Src[I] != 0; I++) {
    Len++;
  }
  while (Len > 0 && Src[Len - 1] == L' ') {
    Len--;
  }
  if (Len > DstChars - 1) {
    Len = DstChars - 1;
  }
  for (I = 0; I < Len; I++) {
    Dst[I] = Src[I];
  }
  Dst[Len] = 0;
}

EFI_STATUS
PartMapRead (
  IN  EFI_BLOCK_IO_PROTOCOL  *Bio,
  IN  UINT64                  DiskLength,
  OUT PARTMAP                *Map
  )
{
  EFI_STATUS  Status;
  UINT32      Bs;
  MBR        *Mbr = NULL;
  GPT_HEADER *Hdr = NULL;
  UINT8      *Entries = NULL;
  UINT32      EntryArrayBytes;
  UINT32      Crc;
  UINTN       I;

  if (Bio == NULL || Map == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  ZeroMem (Map, sizeof (*Map));

  Bs = Bio->Media->BlockSize;
  if (Bs < 512 || DiskLength < (UINT64)Bs * 2) {
    return EFI_UNSUPPORTED;
  }

  /* ---- LBA 0: protective MBR ---- */
  Mbr = AllocateZeroPool (Bs);
  if (Mbr == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  Status = ReadLba (Bio, 0, Bs, Mbr);
  if (EFI_ERROR (Status)) {
    FreePool (Mbr);
    return Status;
  }

  /* LBA 0 was read, so from here on the media has spoken and every exit below
     is a verdict rather than a guess - including "the signature is gone, there
     is no table". Only a failed read leaves this FALSE. */
  Map->Authoritative = TRUE;

  if (Mbr->Signature != MBR_SIGNATURE) {
    FreePool (Mbr);
    return EFI_NOT_FOUND;              /* no partition table at all */
  }

  /* ---- MBR only? ---- */
  if (Mbr->Partition[0].OsType != MBR_TYPE_GPT) {
    for (I = 0; I < 4 && Map->Count < PARTMAP_MAX_ENTRIES; I++) {
      MBR_ENTRY    *E = &Mbr->Partition[I];
      PARTMAP_ENTRY *D;
      CONST CHAR16  *Tn;

      if (E->SizeInLba == 0 || E->OsType == 0) {
        continue;
      }
      D = &Map->Entries[Map->Count++];
      D->Index    = (UINT32)I + 1;
      D->FirstLba = E->StartingLba;
      D->LastLba  = (UINT64)E->StartingLba + (UINT64)E->SizeInLba - 1;
      D->Name[0]  = 0;                 /* MBR has no partition names */
      Tn = MbrTypeName (E->OsType);
      if (Tn != NULL) {
        StrCpyS (D->Type, PARTMAP_TYPE_CHARS, Tn);
      } else {
        UnicodeSPrint (D->Type, sizeof (D->Type), L"类型 0x%02x", E->OsType);
      }
    }
    Map->IsGpt = FALSE;
    Map->Valid = TRUE;
    FreePool (Mbr);
    return EFI_SUCCESS;
  }

  FreePool (Mbr);

  /* ---- GPT ---- */
  Hdr = AllocateZeroPool (Bs);
  if (Hdr == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  Status = ReadLba (Bio, 1, Bs, Hdr);
  if (EFI_ERROR (Status)) {
    FreePool (Hdr);
    return Status;
  }

  if (Hdr->Signature != GPT_SIGNATURE ||
      Hdr->HeaderSize < 92 || Hdr->HeaderSize > Bs)
  {
    DEBUG ((DEBUG_WARN, "[PartMap] GPT header signature/size invalid\n"));
    Map->IsGpt = TRUE;
    Map->Valid = FALSE;
    FreePool (Hdr);
    return EFI_SUCCESS;
  }

  /* Header CRC32 covers HeaderSize bytes with the CRC field zeroed. */
  {
    UINT32 Saved = Hdr->HeaderCrc32;
    Hdr->HeaderCrc32 = 0;
    gBS->CalculateCrc32 (Hdr, Hdr->HeaderSize, &Crc);
    Hdr->HeaderCrc32 = Saved;
    if (Crc != Saved) {
      DEBUG ((DEBUG_WARN, "[PartMap] GPT header CRC mismatch\n"));
      Map->IsGpt = TRUE;
      Map->Valid = FALSE;
      FreePool (Hdr);
      return EFI_SUCCESS;
    }
  }

  if (Hdr->NumberOfPartitionEntries == 0 || Hdr->SizeOfPartitionEntry < 128) {
    Map->IsGpt = TRUE;
    Map->Valid = FALSE;
    FreePool (Hdr);
    return EFI_SUCCESS;
  }

  EntryArrayBytes = Hdr->NumberOfPartitionEntries * Hdr->SizeOfPartitionEntry;
  Entries = AllocateZeroPool (EntryArrayBytes);
  if (Entries == NULL) {
    FreePool (Hdr);
    return EFI_OUT_OF_RESOURCES;
  }

  /* The entry array can span several blocks; read it in one call (BlockIo
     accepts a multi-block length) at its own LBA. */
  Status = ReadLba (Bio, Hdr->PartitionEntryLba, EntryArrayBytes, Entries);
  if (EFI_ERROR (Status)) {
    FreePool (Entries);
    FreePool (Hdr);
    return Status;
  }

  gBS->CalculateCrc32 (Entries, EntryArrayBytes, &Crc);
  if (Crc != Hdr->PartitionEntryArrayCrc32) {
    DEBUG ((DEBUG_WARN, "[PartMap] GPT entry array CRC mismatch\n"));
    Map->IsGpt = TRUE;
    Map->Valid = FALSE;
    FreePool (Entries);
    FreePool (Hdr);
    return EFI_SUCCESS;
  }

  Map->IsGpt = TRUE;
  Map->Valid = TRUE;

  for (I = 0; I < Hdr->NumberOfPartitionEntries && Map->Count < PARTMAP_MAX_ENTRIES; I++) {
    GPT_ENTRY      *E = (GPT_ENTRY *)(Entries + I * Hdr->SizeOfPartitionEntry);
    PARTMAP_ENTRY  *D;
    CONST CHAR16   *Tn;

    if (IsZeroGuid (&E->TypeGuid)) {
      continue;                        /* unused slot */
    }

    D = &Map->Entries[Map->Count++];
    D->Index    = (UINT32)I + 1;
    D->FirstLba = E->StartingLba;
    D->LastLba  = E->EndingLba;
    CopyGptName (E->PartitionName, 36, D->Name, PARTMAP_NAME_CHARS);

    Tn = PartMapTypeName (&E->TypeGuid);
    if (Tn != NULL) {
      StrCpyS (D->Type, PARTMAP_TYPE_CHARS, Tn);
    } else {
      /* Show the first four GUID bytes so an unrecognised type is still
         identifiable in a screenshot. */
      UnicodeSPrint (
        D->Type,
        sizeof (D->Type),
        L"类型 %08x-...",
        E->TypeGuid.Data1
        );
    }
  }

  FreePool (Entries);
  FreePool (Hdr);
  return EFI_SUCCESS;
}
