/** @file
  Platform/LogPersist.c - see LogPersist.h.

  Copyright (c) 2026, Mike Wu. All rights reserved.
**/

#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/PrintLib.h>
#include <Library/DebugLib.h>

#include <Protocol/SimpleFileSystem.h>
#include <Protocol/LoadedImage.h>
#include <Guid/FileInfo.h>

#include "LogPersist.h"
#include "BlockDevice.h"
#include "TextConv.h"
#include "../Version.h"

#define LOG_NAME     L"UefiEraser.log"
#define LOG_HEADER   "iso_timestamp,target_name,mode,algo,passes,result,bytes_written\n"
#define REPORT_NAME  L"UefiEraser-report.txt"

/** Format EFI_TIME as ISO 8601 YYYY-MM-DDTHH:MM:SS (CHAR8). */
STATIC
VOID
FormatIsoTime (
  IN  EFI_TIME  *T,
  OUT CHAR8     *Buf,
  IN  UINTN      BufSize
  )
{
  AsciiSPrint (
    Buf, BufSize,
    "%04d-%02d-%02dT%02d:%02d:%02d",
    (INT32)T->Year, (INT32)T->Month, (INT32)T->Day,
    (INT32)T->Hour, (INT32)T->Minute, (INT32)T->Second
    );
}

/** Simple UTF-16 LE to UTF-8 conversion, limited to ASCII+Printable.
    Non-ASCII chars are emitted as '?' so the CSV stays valid. */
STATIC
VOID
Utf16ToUtf8Csv (
  IN  CONST CHAR16  *In,
  OUT CHAR8         *Out,
  IN  UINTN          OutBytes
  )
{
  UINTN  I = 0;
  UINTN  O = 0;

  while (In[I] != 0 && O + 1 < OutBytes) {
    CHAR16  C = In[I];
    if (C < 0x80 && C != L'"' && C != L'\n' && C != L'\r' && C != L',') {
      Out[O++] = (CHAR8)C;
    } else if (C < 0x80) {
      /* quote commas and quotes */
      if (O + 2 < OutBytes) {
        Out[O++] = '"';
        Out[O++] = (CHAR8)C;
        Out[O++] = '"';
      }
    } else {
      if (O + 1 < OutBytes) {
        Out[O++] = '?';
      }
    }
    I++;
  }
  if (O < OutBytes) {
    Out[O] = 0;
  } else if (OutBytes > 0) {
    Out[OutBytes - 1] = 0;
  }
}

/** Open the boot volume's root directory for the log file. */
STATIC
EFI_FILE_PROTOCOL *
OpenLogDir (
  IN EFI_HANDLE  ImageHandle
  )
{
  EFI_STATUS                        Status;
  EFI_LOADED_IMAGE_PROTOCOL        *Li = NULL;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs = NULL;
  EFI_FILE_PROTOCOL                *Root = NULL;

  Status = gBS->OpenProtocol (
                  ImageHandle,
                  &gEfiLoadedImageProtocolGuid,
                  (VOID **)&Li,
                  ImageHandle,
                  NULL,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  if (EFI_ERROR (Status) || Li == NULL || Li->DeviceHandle == NULL) {
    return NULL;
  }

  Status = gBS->HandleProtocol (
                  Li->DeviceHandle,
                  &gEfiSimpleFileSystemProtocolGuid,
                  (VOID **)&Fs
                  );
  if (EFI_ERROR (Status) || Fs == NULL) {
    return NULL;
  }

  Status = Fs->OpenVolume (Fs, &Root);
  if (EFI_ERROR (Status)) {
    return NULL;
  }
  return Root;
}

/** Write a buffer to a file, appending. Returns FALSE on failure. */
STATIC
BOOLEAN
AppendUtf8 (
  IN EFI_FILE_PROTOCOL  *File,
  IN CONST CHAR8        *Text
  )
{
  EFI_STATUS  Status;
  UINTN       Len = AsciiStrLen (Text);

  if (Len == 0) {
    return TRUE;
  }
  Status = File->Write (File, &Len, (VOID *)Text);
  return (!EFI_ERROR (Status) && Len == AsciiStrLen (Text));
}

VOID
LogPersistRecord (
  IN CONST CHAR16   *TargetName,
  IN CONST CHAR8    *ModeName,
  IN CONST CHAR8    *AlgoName,
  IN UINT32          Passes,
  IN CONST CHAR8    *Result,
  IN UINT64          BytesWritten
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL *Root = NULL;
  EFI_FILE_PROTOCOL *File = NULL;
  EFI_TIME           Time;
  CHAR8              TimeStr[24];
  CHAR8              TargetUtf8[128];
  CHAR8              Line[512];
  BOOLEAN            Exists = FALSE;

  /* Get the wall-clock time. Some UEFI implementations return garbage on the
     first call, so retry once. */
  Status = gRT->GetTime (&Time, NULL);
  if (EFI_ERROR (Status)) {
    Status = gRT->GetTime (&Time, NULL);
  }
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "[LogPersist] GetTime failed: %r\n", Status));
    /* Fall through with zeroed time so the log still gets written. */
    ZeroMem (&Time, sizeof (Time));
  }

  Root = OpenLogDir (gImageHandle);
  if (Root == NULL) {
    DEBUG ((DEBUG_WARN, "[LogPersist] cannot open boot volume root\n"));
    return;
  }

  /* Try to open existing log for append. */
  Status = Root->Open (Root, &File, LOG_NAME,
                       EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
  if (!EFI_ERROR (Status) && File != NULL) {
    Exists = TRUE;
    /* Seek to end for append. */
    Status = File->SetPosition (File, 0xFFFFFFFFFFFFFFFFULL);
    if (EFI_ERROR (Status)) {
      File->Close (File);
      Root->Close (Root);
      return;
    }
  } else {
    /* Create new. */
    Status = Root->Open (Root, &File, LOG_NAME,
                         EFI_FILE_MODE_CREATE
                         | EFI_FILE_MODE_READ
                         | EFI_FILE_MODE_WRITE,
                         0);
    if (EFI_ERROR (Status) || File == NULL) {
      DEBUG ((DEBUG_WARN, "[LogPersist] cannot create log: %r\n", Status));
      Root->Close (Root);
      return;
    }
  }

  if (!Exists) {
    (void)AppendUtf8 (File, LOG_HEADER);
  }

  FormatIsoTime (&Time, TimeStr, sizeof (TimeStr));
  Utf16ToUtf8Csv (TargetName, TargetUtf8, sizeof (TargetUtf8));

  AsciiSPrint (
    Line, sizeof (Line),
    "%a,%a,%a,%a,%d,%a,%lu\n",
    TimeStr,
    TargetUtf8,
    ModeName,
    AlgoName,
    (INT32)Passes,
    Result,
    (INT64)BytesWritten
    );

  (void)AppendUtf8 (File, Line);

  File->Close (File);
  Root->Close (Root);

  DEBUG ((DEBUG_INFO, "[LogPersist] wrote: %a", Line));
}

/** Create or overwrite a UTF-8 text file on the boot volume. Returns TRUE when
    the whole buffer reached the file. */
STATIC
BOOLEAN
WriteUtf8File (
  IN CONST CHAR16  *FileName,
  IN CONST CHAR8   *Text
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL *Root;
  EFI_FILE_PROTOCOL *File;
  EFI_FILE_INFO     *Info;
  UINTN              InfoSize;
  UINTN              Len;
  BOOLEAN            Ok;

  Root = OpenLogDir (gImageHandle);
  if (Root == NULL) {
    DEBUG ((DEBUG_WARN, "[LogPersist] cannot open boot volume root\n"));
    return FALSE;
  }

  Status = Root->Open (
                  Root,
                  &File,
                  (CHAR16 *)FileName,
                  EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE,
                  0
                  );
  if (EFI_ERROR (Status) || File == NULL) {
    DEBUG ((DEBUG_WARN, "[LogPersist] cannot create report file: %r\n", Status));
    Root->Close (Root);
    return FALSE;
  }

  /* The file is rewritten from scratch, so anything a previous export left
     behind must go first. EFI_FILE_PROTOCOL has no truncate call and
     EFI_FILE_MODE_CREATE is not required to shorten an existing file; setting
     FileSize to 0 through SetInfo is the portable way (edk2's FAT driver
     implements it as FatTruncateOFile). */
  InfoSize = sizeof (EFI_FILE_INFO) + 256;
  Info     = AllocateZeroPool (InfoSize);
  if (Info != NULL) {
    Status = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, Info);
    if (!EFI_ERROR (Status) && Info->FileSize != 0) {
      Info->FileSize = 0;
      Status = File->SetInfo (File, &gEfiFileInfoGuid, InfoSize, Info);
      if (EFI_ERROR (Status)) {
        DEBUG ((DEBUG_WARN, "[LogPersist] truncate failed: %r\n", Status));
      }
    }
    FreePool (Info);
  }

  Len = AsciiStrLen (Text);
  Status = File->Write (File, &Len, (VOID *)Text);
  Ok = (BOOLEAN)(!EFI_ERROR (Status) && Len == AsciiStrLen (Text));

  File->Close (File);
  Root->Close (Root);

  DEBUG ((DEBUG_INFO, "[LogPersist] report export: %a (%u bytes)\n",
          Ok ? "ok" : "failed", (UINT32)Len));
  return Ok;
}

BOOLEAN
LogPersistWriteReport (
  IN CONST ERASE_REPORT_SUMMARY  *S
  )
{
  CHAR8     Text[4096];
  CHAR8     TimeStr[32];
  CHAR8     SizeUtf8[48];
  CHAR16    SizeText[32];
  EFI_TIME  Time;
  UINTN     Off;
  UINTN     I;

  if (S == NULL) {
    return FALSE;
  }

  if (!EFI_ERROR (gRT->GetTime (&Time, NULL))) {
    AsciiSPrint (
      TimeStr, sizeof (TimeStr), "%04d-%02d-%02d %02d:%02d:%02d",
      (INT32)Time.Year, (INT32)Time.Month, (INT32)Time.Day,
      (INT32)Time.Hour, (INT32)Time.Minute, (INT32)Time.Second
      );
  } else {
    AsciiStrCpyS (TimeStr, sizeof (TimeStr), "(固件时钟不可用)");
  }

  BlockDevFormatSize (S->BytesWritten, SizeText, ARRAY_SIZE (SizeText));
  Utf16ToUtf8 (SizeText, SizeUtf8, sizeof (SizeUtf8));

  Off = 0;
  Off += AsciiSPrint (
           &Text[Off], sizeof (Text) - Off,
           "UefiEraser 擦除报告\n"
           "==================\n"
           "版本    : %a\n"
           "时间    : %a\n"
           "模式    : %a\n"
           "算法    : %a\n"
           "校验    : %a\n"
           "结果    : %a\n"
           "目标    : %d 个（失败 %d 个）\n"
           "已写入  : %a\n"
           "完成遍数: %d\n"
           "校验不一致字节: %d\n",
           UEFIERASER_VERSION_STR,
           TimeStr,
           S->ModeName,
           S->AlgoId,
           S->Verify ? "开" : "关",
           S->Result,
           (INT32)S->Targets,
           (INT32)S->Failed,
           SizeUtf8,
           (INT32)S->Passes,
           (INT32)S->Mismatches
           );

  if (S->TargetsText != NULL && S->TargetsText[0] != 0) {
    Off += AsciiSPrint (&Text[Off], sizeof (Text) - Off, "\n目标列表:\n");
    for (I = 0; S->TargetsText[I] != 0; I++) {
      CHAR8  LineUtf8[256];
      CHAR16 Line[128];
      UINTN  J;

      /* The list is one CHAR16 string with '\n' separators; convert one line
         at a time so a long list cannot overflow the line buffer. */
      for (J = 0; S->TargetsText[I] != 0 && S->TargetsText[I] != L'\n'; J++) {
        if (J + 1 >= ARRAY_SIZE (Line)) {
          break;
        }
        Line[J] = S->TargetsText[I];
        I++;
      }
      Line[J] = 0;
      Utf16ToUtf8 (Line, LineUtf8, sizeof (LineUtf8));
      if (Off + 256 >= sizeof (Text)) {
        break;
      }
      Off += AsciiSPrint (&Text[Off], sizeof (Text) - Off, "  %a\n", LineUtf8);
    }
  }

  if (Off + 1 < sizeof (Text)) {
    AsciiSPrint (
      &Text[Off], sizeof (Text) - Off,
      "\n本文件由 UefiEraser 生成，供审计留存。\n"
      "多遍覆写对机械盘有效；SSD 请使用设备级擦除。\n"
      );
  }

  return WriteUtf8File (REPORT_NAME, Text);
}
