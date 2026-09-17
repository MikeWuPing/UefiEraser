/** @file
  Ui/MainWindow.c - window chrome, target list, focus and key routing.

  Layout is fixed at 1280x800 (the GOP mode the QEMU harness uses). The
  target list is a flat, indented list rather than a real tree: a whole disk is
  a row, and its partitions follow it indented. That keeps multi-select
  (Ctrl+click, Shift+click ranges, arrow keys, space) exactly as simple as it
  is in a file manager, which is the interaction the brief asked for.

  Copyright (c) 2026, Mike Wu. All rights reserved.
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/PrintLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/LvglLib.h>
#include <Library/LvglUefiPort.h>

#include "../Version.h"
#include "../Platform/TextConv.h"
#include "AppCtx.h"
#include "Dialogs.h"
#include "MainWindow.h"
#include "GlassChrome.h"

LV_FONT_DECLARE (lv_font_simsun_16_cjk);
/* The icon font. The UI text font is the simsun CJK subset and carries no
   symbols at all, so the LV_SYMBOL_* glyphs have to come from a font that has
   them: the built-in Montserrat does (lv_font_montserrat_16.c's sparse cmap
   covers 0xF00B - 0xF2ED, i.e. LIST/OK/CLOSE/SETTINGS/WARNING/TRASH/SAVE...). */
LV_FONT_DECLARE (lv_font_montserrat_16);

APP_CTX  *gApp = NULL;

/* Forward declaration: RefreshList registers this on every row it creates. */
STATIC VOID RowClickCb (IN lv_event_t *e);

/* ---- palette (dark) ---- */
#define C_BG        0x1E1E1E
#define C_CHROME    0x2D2D30
#define C_TEXT      0xE0E0E0
#define C_DIM       0x9A9A9A
#define C_ROW       0x252526
#define C_ROW_SEL   0x0E4A6B
#define C_ROW_FOCUS 0x3A3D41
/* A row that cannot be selected (read-only media, the boot volume) is drawn
   greyed and flat: the same colour as an ordinary row would make the refusal
   look like a bug, which is exactly how it was reported. */
#define C_ROW_OFF   0x191A1B
#define C_ROW_OFF_FOCUS 0x26282A
#define C_TEXT_OFF  0x6E6E6E
#define C_DIM_OFF   0x5E5E5E
#define C_DANGER    0xC0392B
#define C_ACCENT    0x2D7D9A
/* Hover / pressed shades. A flat rectangle that only changes colour on click
   is what made the buttons read as plain; a gradient, a shadow and a 1px
   press-down give them an actual surface. */
#define C_ACCENT_HI 0x3E9CC0
#define C_ACCENT_LO 0x1F5F78
#define C_DANGER_HI 0xD8503F
#define C_DANGER_LO 0x8E2A1F
#define C_FOCUS_RING 0x7FC4FF

/** How long a hint stays on screen (ms). */
#define UI_HINT_MS  5000

/* Panels are translucent when the glass card is behind them, so the blurred
   wallpaper reads through; opaque on the fallback (no-glass) path, which is
   what every screenshot before glass was taken against. */
STATIC BOOLEAN  mGlassActive = FALSE;

STATIC
lv_opa_t
PanelOpa (
  IN UINT32  Translucent
  )
{
  return mGlassActive ? (lv_opa_t)Translucent : LV_OPA_COVER;
}

VOID
UiFont (
  IN lv_obj_t  *Obj
  )
{
  lv_obj_set_style_text_font (Obj, &lv_font_simsun_16_cjk, 0);
}

/** Keep a widget out of LVGL's default keypad group.

    lv_obj_class_init_obj () adds "group-def" widgets to the default group on
    creation, and indev_keypad_proc does two things with that group: it requires
    a focused member before it delivers a key at all, and on ENTER it sends
    PRESSED/CLICKED to that member. So any real widget left in the group is a
    second actor for the same keystroke - which is exactly how the 操作 menu
    opened by itself in the capture run, and then sat over the list swallowing
    every later key. Only the inert sink (see MainWindowCreate) belongs there.
    lv_group_remove_obj () is a no-op for an object that is not a member. */
VOID
UiGroupDetach (
  IN lv_obj_t  *Obj
  )
{
  if ((Obj != NULL) && (lv_group_get_default () != NULL)) {
    lv_group_remove_obj (Obj);
  }
}

/** The companion of UiFont for icon glyphs. */
VOID
IconFont (
  IN lv_obj_t  *Obj
  )
{
  lv_obj_set_style_text_font (Obj, &lv_font_montserrat_16, 0);
}

/* Background colour and shadow width ease instead of snapping, so hover and
   press read as one surface changing state rather than two repaints. */
STATIC CONST lv_style_prop_t  kBtnTransProps[] = {
  LV_STYLE_BG_COLOR, LV_STYLE_BG_GRAD_COLOR, LV_STYLE_SHADOW_WIDTH, 0
};
STATIC lv_style_transition_dsc_t  sBtnTrans;
STATIC BOOLEAN                    sBtnTransReady = FALSE;

STATIC
VOID
EnsureButtonTransition (
  VOID
  )
{
  if (!sBtnTransReady) {
    lv_style_transition_dsc_init (&sBtnTrans, kBtnTransProps,
                                  lv_anim_path_ease_out, 150, 0, NULL);
    sBtnTransReady = TRUE;
  }
}

/* ---- transient hints --------------------------------------------------
   A target that cannot be selected (read-only device, boot volume) ignores
   the click, and 全选 skips such rows on purpose. Both are correct, and both
   were silent - which is indistinguishable from a broken UI. Every refusal
   now says why, in a bar just above the status bar, and logs it so the
   capture run can assert on it. */
STATIC VOID MainWindowHint (IN OUT APP_CTX *A, IN CONST CHAR8 *Text);

STATIC lv_timer_t  *sHintTimer = NULL;

STATIC
VOID
HintExpireCb (
  IN lv_timer_t  *T
  )
{
  APP_CTX  *A = (APP_CTX *)lv_timer_get_user_data (T);

  if ((A != NULL) && (A->HintBar != NULL)) {
    lv_obj_add_flag (A->HintBar, LV_OBJ_FLAG_HIDDEN);
  }
  lv_timer_pause (T);
}

/** Take the hint down early.

    The hint has a 5 s life of its own, but it also has to go the moment its
    reason does: selecting a valid target after clicking a refused one used to
    leave the warning up, which reads as "this selection was refused too". */
STATIC
VOID
MainWindowHintClear (
  IN OUT APP_CTX  *A
  )
{
  if (A->HintBar != NULL) {
    lv_obj_add_flag (A->HintBar, LV_OBJ_FLAG_HIDDEN);
  }
  if (sHintTimer != NULL) {
    lv_timer_pause (sHintTimer);
  }
}

STATIC
VOID
MainWindowHint (
  IN OUT APP_CTX     *A,
  IN     CONST CHAR8 *Text
  )
{
  if ((A->HintBar == NULL) || (A->HintLabel == NULL)) {
    return;
  }
  lv_label_set_text (A->HintLabel, Text);
  lv_obj_clear_flag (A->HintBar, LV_OBJ_FLAG_HIDDEN);
  if (sHintTimer == NULL) {
    sHintTimer = lv_timer_create (HintExpireCb, UI_HINT_MS, A);
    lv_timer_pause (sHintTimer);
  }
  lv_timer_reset (sHintTimer);
  lv_timer_resume (sHintTimer);
  DEBUG ((DEBUG_INFO, "[Ui] hint: %a\n", Text));
}

/* ------------------------------------------------------- model helpers */

CONST CHAR8 *
AppModeName (
  IN CONST APP_CTX  *A
  )
{
  switch (A->Mode) {
    case ERASE_MODE_OVERWRITE:      return "覆盖擦除";
    case ERASE_MODE_ATA:            return "ATA Secure Erase";
    case ERASE_MODE_NVME_FORMAT:    return "NVMe Format";
    case ERASE_MODE_NVME_SANITIZE:  return "NVMe Sanitize";
    default:                        return "覆盖擦除";
  }
}

CONST ERASE_ALGORITHM *
AppCurrentAlgo (
  IN CONST APP_CTX  *A
  )
{
  CONST ERASE_ALGORITHM  *Algo;

  Algo = EraseAlgoAt (A->AlgoIndex);
  if (Algo == NULL) {
    Algo = EraseAlgoAt (0);
  }
  return Algo;
}

UINT32
AppCurrentPassCount (
  IN CONST APP_CTX  *A
  )
{
  CONST ERASE_ALGORITHM  *Algo = AppCurrentAlgo (A);

  if (Algo->PassCount != 0) {
    return Algo->PassCount;
  }
  return (A->CustomPasses == 0) ? 1 : A->CustomPasses;
}

UINT32
AppSelectedCount (
  IN CONST APP_CTX  *A
  )
{
  UINT32 I;
  UINT32 N = 0;

  for (I = 0; I < A->TargetCount; I++) {
    if (A->Targets[I].Selected) {
      N++;
    }
  }
  return N;
}

UINT64
AppSelectedBytes (
  IN CONST APP_CTX  *A
  )
{
  UINT32  I;
  UINT64  Total = 0;
  UINT32  Passes = AppCurrentPassCount (A);

  for (I = 0; I < A->TargetCount; I++) {
    if (A->Targets[I].Selected) {
      if (A->Targets[I].Length > (0xFFFFFFFFFFFFFFFFULL - Total)) {
        return 0xFFFFFFFFFFFFFFFFULL;
      }
      Total += A->Targets[I].Length;
    }
  }
  if (Passes != 0 && Total > (0xFFFFFFFFFFFFFFFFULL / Passes)) {
    return 0xFFFFFFFFFFFFFFFFULL;
  }
  return Total * Passes;
}

/** Convert a UTF-16 string to a UTF-8 buffer for LVGL.

    NOT UnicodeStrToAsciiStrS: that asserts on any code unit >= 0x100 and
    truncates, which silently drops every Chinese character (the row would read
    "[ ] 0" instead of "[ ] 磁盘 0"). See Platform/TextConv.h. */
STATIC
UINTN
ToUtf8 (
  IN  CONST CHAR16  *In,
  OUT CHAR8         *Out,
  IN  UINTN          OutBytes
  )
{
  return Utf16ToUtf8 (In, Out, OutBytes);
}

/** Does the freshly-read partition table still describe this partition device?

    The firmware's partition handles are built once, from whatever the table
    said at the time, and they outlive it: after a disk is erased the handle
    database goes on offering BlockIo for partitions that are gone. So the media
    decides. Two ways to match, and the first is the good one:

      - by LBA range, taken from the partition's own device path node. A table
        describes regions, and this is layout-independent: a logical partition
        inside an MBR extended region (partition numbers 5 and up, which appear
        in no primary entry) is matched by the extended entry that contains it.
      - by partition number, when the device path carries no range.

    A partition that cannot be judged is KEPT. Hiding a target that exists is
    the worse failure of the two: the user can still erase the whole disk, but a
    partition they cannot see is a partition they cannot erase.

    @retval TRUE  the media still describes it, or could not be asked
    @retval FALSE the media describes no such partition: the row is a ghost
*/
STATIC
BOOLEAN
PartitionInTable (
  IN CONST PARTMAP    *Map,
  IN CONST BLOCK_DEV  *Part
  )
{
  UINT32  E;
  UINT64  Last;

  /* Nothing to judge by: no table was read, or the disk has more partitions
     than PARTMAP_MAX_ENTRIES and the entries past the cap were never read. */
  if (Map == NULL || !Map->Authoritative || Map->Count >= PARTMAP_MAX_ENTRIES) {
    return TRUE;
  }
  if (Map->Count == 0) {
    return FALSE;                      /* no usable table on the media */
  }

  if (Part->Blocks > 0) {
    Last = Part->FirstLba + Part->Blocks - 1;
    for (E = 0; E < Map->Count; E++) {
      if (Map->Entries[E].FirstLba <= Part->FirstLba &&
          Map->Entries[E].LastLba >= Last)
      {
        return TRUE;
      }
    }
    return FALSE;
  }

  if (Part->PartitionNo != 0) {
    for (E = 0; E < Map->Count; E++) {
      if (Map->Entries[E].Index == Part->PartitionNo) {
        return TRUE;
      }
    }
    return FALSE;
  }

  return TRUE;                         /* no range and no number: cannot ask */
}

/** Build the flattened target list: each whole disk, then its partitions,
    indented. Selection is preserved across rebuilds by matching on handle +
    partition number.

    Naming is index-based ("磁盘 0", "分区 3") rather than device-path-based:
    a full device path string is 80+ characters of PciRoot(...)/Sata(...) that
    tells the user nothing at a glance. The raw path is shown in the
    confirmation dialog, where it actually matters. */
VOID
AppBuildTargets (
  IN OUT APP_CTX  *A,
  OUT    UINT32   *GhostsDropped
  )
{
  UINT32       D;
  UINT32       T;
  BOOLEAN      WasSelected[APP_MAX_ROWS];
  VOID        *WasHandle[APP_MAX_ROWS];
  UINT32       WasPart[APP_MAX_ROWS];
  UINT32       WasCount = A->TargetCount;
  UINT32       DiskIndex = 0;
  UINT32       Ghosts = 0;

  if (GhostsDropped != NULL) {
    *GhostsDropped = 0;
  }

  for (T = 0; T < A->TargetCount && T < APP_MAX_ROWS; T++) {
    WasSelected[T] = A->Targets[T].Selected;
    WasHandle[T]   = A->Targets[T].Handle;
    WasPart[T]     = A->Targets[T].PartitionNo;
  }

  ZeroMem (A->Targets, sizeof (A->Targets));
  A->TargetCount = 0;

  for (D = 0; D < A->Devs.Count && A->TargetCount < APP_MAX_ROWS; D++) {
    BLOCK_DEV     *Dev = &A->Devs.Items[D];
    ERASE_TARGET  *Tgt;
    PARTMAP       *Map;

    if (Dev->LogicalPartition) {
      continue;                       /* emitted under its parent disk */
    }

    Map = (D < BLOCK_DEV_MAX) ? &A->Maps[D] : NULL;

    Tgt = &A->Targets[A->TargetCount];
    ZeroMem (Tgt, sizeof (*Tgt));
    Tgt->Kind         = TARGET_KIND_DISK;
    Tgt->Handle       = Dev->Handle;
    Tgt->ParentHandle = NULL;
    Tgt->Offset       = 0;
    Tgt->Length       = Dev->Length;
    Tgt->BlockSize    = Dev->BlockSize;
    Tgt->PartitionNo  = 0;
    Tgt->ReadOnly     = Dev->ReadOnly;
    Tgt->Removable    = Dev->Removable;
    Tgt->IsBootVolume = Dev->IsBootVolume;
    UnicodeSPrint (Tgt->Name, sizeof (Tgt->Name), L"磁盘 %d", DiskIndex);
    if (Map != NULL && Map->Valid) {
      UnicodeSPrint (
        Tgt->Detail, sizeof (Tgt->Detail),
        (Map->IsGpt) ? L"GPT  %d 个分区" : L"MBR  %d 个分区",
        Map->Count
        );
    } else if (Map != NULL && Map->Authoritative) {
      /* Read, and there is no table there: a blank disk, or one whose table
         this run just erased. "Unknown" would be the wrong kind of humble - we
         know exactly what is on it, which is nothing. */
      StrCpyS (Tgt->Detail, TARGET_DETAIL_MAX, L"无分区表");
    } else {
      StrCpyS (Tgt->Detail, TARGET_DETAIL_MAX, L"分区表未知");
    }
    A->TargetCount++;
    DiskIndex++;

    /* Then this disk's partitions, in partition-number order. */
    {
      UINT32 P;
      for (P = 0; P < A->Devs.Count && A->TargetCount < APP_MAX_ROWS; P++) {
        BLOCK_DEV     *Pd = &A->Devs.Items[P];
        ERASE_TARGET  *Pt;
        UINT32         E;

        if (!Pd->LogicalPartition || Pd->ParentHandle != Dev->Handle) {
          continue;
        }

        /* The media decides. A stale handle here is a row that would lie: it
           claims a partition the disk no longer has, and its BlockIo describes
           an LBA range the current table assigns to nothing. */
        if (!PartitionInTable (Map, Pd)) {
          Ghosts++;
          DEBUG ((
            DEBUG_INFO,
            "[Ui] disk %d: dropped partition #%u (%lu bytes) - the media does "
            "not describe it\n",
            DiskIndex - 1,
            Pd->PartitionNo,
            (UINT64)Pd->Length
            ));
          continue;
        }

        Pt = &A->Targets[A->TargetCount];
        ZeroMem (Pt, sizeof (*Pt));
        Pt->Kind         = TARGET_KIND_PARTITION;
        Pt->Handle       = Pd->Handle;
        Pt->ParentHandle = Dev->Handle;
        Pt->Offset       = 0;
        Pt->Length       = Pd->Length;
        Pt->BlockSize    = Pd->BlockSize;
        Pt->PartitionNo  = Pd->PartitionNo;
        Pt->ReadOnly     = Pd->ReadOnly;
        Pt->Removable    = Pd->Removable;
        Pt->IsBootVolume = Pd->IsBootVolume;
        UnicodeSPrint (Pt->Name, sizeof (Pt->Name), L"分区 %d", Pd->PartitionNo);

        /* Prefer the partition table's own type name / GPT name. */
        Pt->Detail[0] = 0;
        if (Map != NULL && Map->Valid) {
          for (E = 0; E < Map->Count; E++) {
            if (Map->Entries[E].Index == Pd->PartitionNo) {
              if (Map->Entries[E].Name[0] != 0) {
                StrCpyS (Pt->Detail, TARGET_DETAIL_MAX, Map->Entries[E].Name);
              } else {
                StrCpyS (Pt->Detail, TARGET_DETAIL_MAX, Map->Entries[E].Type);
              }
              break;
            }
          }
        }
        if (Pt->Detail[0] == 0) {
          StrCpyS (Pt->Detail, TARGET_DETAIL_MAX, L"未知类型");
        }

        A->TargetCount++;
      }
    }
  }

  /* Restore selection where the same handle+partition still exists. */
  for (T = 0; T < A->TargetCount; T++) {
    UINT32 W;
    for (W = 0; W < WasCount; W++) {
      if (WasHandle[W] == A->Targets[T].Handle &&
          WasPart[W] == A->Targets[T].PartitionNo)
      {
        A->Targets[T].Selected = WasSelected[W];
        break;
      }
    }
  }

  if (GhostsDropped != NULL) {
    *GhostsDropped = Ghosts;
  }
  if (Ghosts > 0) {
    DEBUG ((
      DEBUG_INFO,
      "[Ui] ghost: %u partition row(s) dropped, the media no longer describes "
      "them\n",
      Ghosts
      ));
  }
  DEBUG ((DEBUG_INFO, "[Ui] %u target row(s)\n", (UINT32)A->TargetCount));
}

/** Can this target be armed at all? Gate 1: read-only media cannot be written,
    and the boot volume (the disk or partition the running image came from)
    would destroy itself mid-write. Used by the list renderer, by click handling
    and by 全选, so the three can never disagree. */
STATIC
BOOLEAN
TargetSelectable (
  IN CONST ERASE_TARGET  *T
  )
{
  return (BOOLEAN)(!T->ReadOnly && !T->IsBootVolume);
}

/* ------------------------------------------------------------ rendering */

STATIC
CONST CHAR16 *
TargetRowTitle (
  IN  CONST ERASE_TARGET  *T,
  OUT CHAR16              *Buf,
  IN  UINTN                BufChars
  )
{
  /* "[" + marker + "]" is ASCII on purpose: the marker must never depend on
     the CJK font subset covering a symbol glyph. */
  /* "-" instead of a checkbox for a row that can never be ticked. */
  if (T->Kind == TARGET_KIND_DISK) {
    UnicodeSPrint (Buf, BufChars * sizeof (CHAR16), L"[%a] %s",
                   TargetSelectable (T) ? (T->Selected ? "*" : " ") : "-",
                   T->Name);
  } else {
    UnicodeSPrint (Buf, BufChars * sizeof (CHAR16), L"    [%a] %s",
                   TargetSelectable (T) ? (T->Selected ? "*" : " ") : "-",
                   T->Name);
  }
  return Buf;
}

/** Re-scan the world and rebuild the target list.

    Two steps, and the second is what makes this honest:

      1. enumerate the block devices again. The firmware is the authority on
         what exists as hardware, and on nothing else;
      2. re-read every partition table OFF THE MEDIA and let AppBuildTargets
         drop the rows the media no longer describes. Partition handles outlive
         the tables they were built from, so the handle database keeps listing
         partitions that an erasure has already destroyed. Nothing has to be
         repaired in the firmware for the list to be right - it is rebuilt from
         the bytes, which is also the only version the user can check.

    AppRefreshTargets (A, &Info) then carries the counts.

    @param[out] Info  optional (may be NULL): counts for a log line or a hint */
VOID
AppRefreshTargets (
  IN OUT APP_CTX      *A,
  OUT    RESCAN_INFO  *Info
  )
{
  EFI_STATUS   Status;
  RESCAN_INFO  Found;
  UINT32       I;

  ZeroMem (&Found, sizeof (Found));
  Found.WasTargets = A->TargetCount;

  Status = BlockDevEnumerate (&A->Devs, A->ImageHandle);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[Ui] refresh: enumeration failed: %r\n", Status));
    if (Info != NULL) {
      *Info = Found;
    }
    return;
  }
  Found.Devs = (UINT32)A->Devs.Count;

  /* Every whole disk, read again from LBA 0. A partition device carries no
     table of its own, and A->Maps is indexed by device index so the parent's
     map is found by position. */
  ZeroMem (A->Maps, sizeof (A->Maps));
  for (I = 0; I < A->Devs.Count && I < BLOCK_DEV_MAX; I++) {
    BLOCK_DEV  *Dev = &A->Devs.Items[I];

    if (Dev->LogicalPartition) {
      continue;
    }
    if (Dev->BlockSize >= 512) {
      PartMapRead (Dev->BlockIo, Dev->Length, &A->Maps[I]);
    }
  }

  AppBuildTargets (A, &Found.Ghosts);
  Found.Targets = A->TargetCount;

  if (A->TargetCount == 0) {
    A->RowFocus = 0;
  } else if (A->RowFocus >= A->TargetCount) {
    A->RowFocus = A->TargetCount - 1;
  }

  MainWindowRefreshList (A);
  AppRefreshStatus (A);

  DEBUG ((
    DEBUG_INFO,
    "[Ui] refresh: devs=%u targets=%u (was %u) ghosts=%u\n",
    Found.Devs,
    Found.Targets,
    Found.WasTargets,
    Found.Ghosts
    ));

  if (Info != NULL) {
    *Info = Found;
  }
}

/** Fill the dim second line: size plus whatever identifies the device. */
STATIC
VOID
TargetRowDetail (
  IN  CONST ERASE_TARGET  *T,
  OUT CHAR16              *Buf,
  IN  UINTN                BufChars
  )
{
  CHAR16  Size[32];
  CHAR16  Flags[64];

  BlockDevFormatSize (T->Length, Size, ARRAY_SIZE (Size));

  Flags[0] = 0;
  if (T->ReadOnly) {
    StrCatS (Flags, ARRAY_SIZE (Flags), L"  只读");
  }
  if (T->Removable) {
    StrCatS (Flags, ARRAY_SIZE (Flags), L"  可移除");
  }
  if (T->IsBootVolume) {
    StrCatS (Flags, ARRAY_SIZE (Flags), L"  当前启动卷");
  }

  /* The reason is part of the row, not only of the hint that a click raises:
     a greyed row with no explanation just looks broken. */
  if (!TargetSelectable (T)) {
    if (T->ReadOnly) {
      StrCatS (Flags, ARRAY_SIZE (Flags), L"  （只读设备，不可选）");
    } else if (T->Kind == TARGET_KIND_PARTITION) {
      StrCatS (Flags, ARRAY_SIZE (Flags), L"  （本程序正在运行于此卷，不可选）");
    } else {
      StrCatS (Flags, ARRAY_SIZE (Flags), L"  （本程序正在运行于此盘，不可选）");
    }
  }

  if (T->Detail[0] != 0) {
    UnicodeSPrint (Buf, BufChars * sizeof (CHAR16), L"%s   %s%s",
                   Size, T->Detail, Flags);
  } else {
    UnicodeSPrint (Buf, BufChars * sizeof (CHAR16), L"%s%s", Size, Flags);
  }
}

VOID
MainWindowRefreshList (
  IN OUT APP_CTX  *A
  )
{
  UINT32  I;

  if (A->ListBox == NULL) {
    return;
  }

  lv_obj_clean (A->ListBox);
  ZeroMem (A->Rows, sizeof (A->Rows));

  for (I = 0; I < A->TargetCount; I++) {
    ERASE_TARGET  *T = &A->Targets[I];
    lv_obj_t      *Row;
    lv_obj_t      *Main;
    lv_obj_t      *Detail;
    CHAR16         Title[160];
    CHAR16         DetailText[200];
    CHAR8          TitleUtf8[320];
    CHAR8          DetailUtf8[400];

    BOOLEAN  Usable = TargetSelectable (T);

    Row = lv_obj_create (A->ListBox);
    UiGroupDetach (Row);
    lv_obj_remove_style_all (Row);
    lv_obj_set_width (Row, LV_PCT (100));
    lv_obj_set_height (Row, 40);
    lv_obj_set_style_bg_opa (Row, PanelOpa (Usable ? 0x8C : 0x60), 0);
    lv_obj_set_style_bg_color (
      Row,
      lv_color_hex (Usable ? (T->Selected ? C_ROW_SEL : C_ROW) : C_ROW_OFF),
      0
      );
    lv_obj_set_style_bg_color (
      Row,
      lv_color_hex (Usable ? C_ROW_FOCUS : C_ROW_OFF_FOCUS),
      LV_STATE_FOCUSED
      );
    lv_obj_set_style_pad_hor (Row, 8, 0);
    lv_obj_set_style_pad_ver (Row, 2, 0);
    lv_obj_set_style_radius (Row, 3, 0);
    lv_obj_clear_flag (Row, LV_OBJ_FLAG_SCROLLABLE);
    /* Every level needs the bubble flag or a key press stops here instead of
       reaching the screen handler (LVGL v9 checks the flag per parent). */
    lv_obj_add_flag (Row, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_user_data (Row, (VOID *)(UINTN)I);

    TargetRowTitle (T, Title, ARRAY_SIZE (Title));
    TargetRowDetail (T, DetailText, ARRAY_SIZE (DetailText));

    ToUtf8 (Title, TitleUtf8, sizeof (TitleUtf8));
    ToUtf8 (DetailText, DetailUtf8, sizeof (DetailUtf8));

    Main = lv_label_create (Row);
    lv_label_set_text (Main, TitleUtf8);
    UiFont (Main);
    lv_obj_set_style_text_color (Main, lv_color_hex (Usable ? C_TEXT : C_TEXT_OFF), 0);
    lv_obj_align (Main, LV_ALIGN_TOP_LEFT, 0, 0);

    Detail = lv_label_create (Row);
    lv_label_set_text (Detail, DetailUtf8);
    UiFont (Detail);
    lv_obj_set_style_text_color (Detail, lv_color_hex (Usable ? C_DIM : C_DIM_OFF), 0);
    lv_obj_align (Detail, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    /* Clicking a row toggles it. Ctrl/Shift come from the port's modifier
       query, which reports nothing under the plain SimpleTextIn fallback -
       accepted: without modifiers the click simply toggles one row. */
    lv_obj_add_event_cb (Row, RowClickCb, LV_EVENT_CLICKED, (VOID *)(UINTN)I);

    /* One line per row so the capture run can assert that the greyed rows are
       the intended ones (and not, say, every row). */
    DEBUG ((
      DEBUG_INFO,
      "[Ui] row=%u usable=%d boot=%d ro=%d\n",
      I, Usable, T->IsBootVolume, T->ReadOnly
      ));

    A->Rows[I] = Row;
  }

  AppRefreshStatus (A);
}

VOID
AppRefreshStatus (
  IN OUT APP_CTX  *A
  )
{
  CHAR8   Text[256];
  UINT64  Bytes;
  CHAR16  SizeText[32];
  CHAR8   SizeUtf8[48];
  UINT32  Sel;
  UINT32  Passes;

  if (A->StatusLabel == NULL) {
    return;
  }

  Sel   = AppSelectedCount (A);
  Bytes = AppSelectedBytes (A);
  BlockDevFormatSize (Bytes, SizeText, ARRAY_SIZE (SizeText));
  ToUtf8 (SizeText, SizeUtf8, sizeof (SizeUtf8));

  if (A->Mode == ERASE_MODE_OVERWRITE) {
    Passes = AppCurrentPassCount (A);
    AsciiSPrint (
      Text,
      sizeof (Text),
      "目标 %d 个   已选 %d 个   待写总量 %a (%d 遍)",
      (UINT32)A->TargetCount,
      (UINT32)Sel,
      SizeUtf8,
      (UINT32)Passes
      );
  } else {
    AsciiSPrint (
      Text,
      sizeof (Text),
      "目标 %d 个   已选 %d 个   模式: %a",
      (UINT32)A->TargetCount,
      (UINT32)Sel,
      AppModeName (A)
      );
  }
  lv_label_set_text (A->StatusLabel, Text);
}

VOID
MainWindowRefreshAlgo (
  IN OUT APP_CTX  *A
  )
{
  CONST ERASE_ALGORITHM  *Algo = AppCurrentAlgo (A);
  CHAR8                   Text[256];

  if (A->AlgoLabel == NULL) {
    return;
  }

  /* No pass list here. With it the string ran past the label's box and LVGL
     wrapped it to two lines, which the 24 px bar then clipped top and bottom -
     the "算法显示两行" report. The pass sequence is shown in the algorithm
     dialog (per row) and in the summary dialog, which is where it is read. */
  if (A->Mode == ERASE_MODE_OVERWRITE) {
    AsciiSPrint (
      Text,
      sizeof (Text),
      "算法: %a  (%d 遍)   校验: %a",
      Algo->NameEn,
      (UINT32)AppCurrentPassCount (A),
      A->Verify ? "ON" : "OFF"
      );
  } else {
    AsciiSPrint (
      Text,
      sizeof (Text),
      "模式: %a   (设备级擦除，由固件执行)",
      AppModeName (A)
      );
  }
  lv_label_set_text (A->AlgoLabel, Text);
}

/* -------------------------------------------------------------- chrome */

STATIC
VOID
CloseMenu (
  IN OUT APP_CTX  *A
  )
{
  if (A->MenuPopup != NULL) {
    lv_obj_delete (A->MenuPopup);
    A->MenuPopup = NULL;
  }
}

STATIC
VOID
MenuAction (
  IN OUT APP_CTX  *A,
  IN     UINT32    Action
  )
{
  DEBUG ((DEBUG_INFO, "[Ui] menu action %u\n", Action));
  CloseMenu (A);
  switch (Action) {
    case 0:   /* 全选 */
      {
        UINT32 I;
        UINT32 Skipped = 0;
        CHAR8  Msg[192];
        /* Same exclusions as a click: read-only media and the boot volume are
           never swept up by 全选, so the shortcut cannot arm a target the
           user would have to click twice to dismiss. The count of what was
           skipped is reported: "全选 didn't select the disk I wanted" is
           otherwise indistinguishable from a broken shortcut. */
        for (I = 0; I < A->TargetCount; I++) {
          if (TargetSelectable (&A->Targets[I])) {
            A->Targets[I].Selected = TRUE;
          } else {
            Skipped++;
          }
        }
        if (Skipped == 0) {
          AsciiSPrint (Msg, sizeof (Msg), "已全选全部目标（%d 个）。",
                       (UINT32)AppSelectedCount (A));
        } else {
          AsciiSPrint (
            Msg, sizeof (Msg),
            "已选 %d 个；另有 %d 个不可选（只读设备 / 当前启动卷），已自动跳过。",
            (UINT32)AppSelectedCount (A), Skipped
            );
        }
        MainWindowHint (A, Msg);
      }
      MainWindowRefreshList (A);
      break;
    case 1:   /* 全不选 */
      {
        UINT32 I;
        for (I = 0; I < A->TargetCount; I++) {
          A->Targets[I].Selected = FALSE;
        }
        MainWindowHint (A, "已清空全部选择。");
      }
      MainWindowRefreshList (A);
      break;
    case 2:   /* 选择算法 */
      DialogsPickAlgorithm (A);
      break;
    case 3:   /* 开始擦除 */
      DialogsConfirmAndRun (A);
      break;
    case 4:   /* 重新扫描目标 */
      /* Re-enumerate and re-read every partition table off the media. Worth
         having on its own (a disk swapped behind a removable bay appears here),
         and it is the same path the post-erasure refresh takes. The counts are
         in the hint because "nothing changed" and "three dead rows were
         dropped" otherwise look identical on screen. */
      {
        RESCAN_INFO  Found;
        CHAR8        Scan[192];

        AppRefreshTargets (A, &Found);
        if (Found.Ghosts > 0) {
          AsciiSPrint (
            Scan, sizeof (Scan),
            "已重新扫描：%d 个目标，丢弃了 %d 个介质上已不存在的分区行。",
            (UINT32)Found.Targets,
            (UINT32)Found.Ghosts
            );
        } else {
          AsciiSPrint (
            Scan, sizeof (Scan),
            "已重新扫描：%d 个块设备，%d 个目标。",
            (UINT32)Found.Devs,
            (UINT32)Found.Targets
            );
        }
        MainWindowHint (A, Scan);
      }
      break;
    case 5:   /* 关于 */
      DialogsAbout (A);
      break;
    case 6:   /* 退出 */
      /* Explicit rather than "default": this is the only way out of the
         program that the user can see, and Esc in the window does the same
         thing. Logged, so a capture run can tell a real quit from a crash. */
      DEBUG ((DEBUG_INFO, "[Ui] quit requested from the 操作 menu\n"));
      A->Quit = TRUE;
      break;
    default:
      break;
  }
}

STATIC
VOID
MenuItemCb (
  IN lv_event_t  *e
  )
{
  UINT32 Action = (UINT32)(UINTN)lv_event_get_user_data (e);
  MenuAction (gApp, Action);
}

/* ---- 操作 menu focus -------------------------------------------------
   The popup keeps its own index instead of leaning on LVGL's keypad group.
   Group membership is what made ENTER run two actions (LVGL also sends its own
   CLICKED to the focused member), and the same navigation is needed in every
   other part of this UI anyway - so it is done the same way here: highlight the
   row, act on ENTER, log the stop. */
#define MENU_ITEM_MAX  8
STATIC lv_obj_t  *sMenuRow[MENU_ITEM_MAX];
STATIC UINT32     sMenuCount;
STATIC UINT32     sMenuIdx;

STATIC
VOID
MenuHighlight (
  IN UINT32  Index
  )
{
  UINT32  I;

  if (sMenuCount == 0) {
    return;
  }
  sMenuIdx = Index % sMenuCount;
  for (I = 0; I < sMenuCount; I++) {
    if (I == sMenuIdx) {
      lv_obj_add_state (sMenuRow[I], LV_STATE_FOCUSED);
    } else {
      lv_obj_remove_state (sMenuRow[I], LV_STATE_FOCUSED);
    }
  }
  DEBUG ((DEBUG_INFO, "[Ui] menu focus %u/%u\n", sMenuIdx + 1, sMenuCount));
}

STATIC
VOID
MenuMove (
  IN INT32  Delta
  )
{
  if (sMenuCount == 0) {
    return;
  }
  MenuHighlight ((UINT32)(((INT32)sMenuIdx + Delta + (INT32)sMenuCount)
                          % (INT32)sMenuCount));
}

STATIC
VOID
MenuActivate (
  VOID
  )
{
  if (sMenuCount == 0) {
    return;
  }
  lv_obj_send_event (sMenuRow[sMenuIdx], LV_EVENT_CLICKED, NULL);
}

STATIC
VOID
OpenMenu (
  IN OUT APP_CTX  *A,
  IN     lv_obj_t *Title
  )
{
  /* Each row carries an icon, and 退出 is drawn like what it is: the one entry
     that ends the program, in the danger colour with a power glyph. It was
     already here and already worked, but as a plain grey word at the bottom of
     a plain grey list it read as decoration - it got reported as missing. */
  STATIC CONST CHAR8  *Icons[] = {
    LV_SYMBOL_OK, LV_SYMBOL_CLOSE, LV_SYMBOL_SETTINGS,
    LV_SYMBOL_WARNING, LV_SYMBOL_REFRESH, LV_SYMBOL_FILE, LV_SYMBOL_POWER
  };
  STATIC CONST CHAR8  *Items[] = {
    "全选", "全不选", "选择算法…", "开始擦除…", "重新扫描目标", "关于", "退出"
  };
  UINTN  I;

  CloseMenu (A);
  sMenuCount = 0;
  sMenuIdx   = 0;

  A->MenuPopup = lv_obj_create (A->Scr);
  lv_obj_remove_style_all (A->MenuPopup);
  lv_obj_set_width (A->MenuPopup, 200);
  lv_obj_set_height (A->MenuPopup, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color (A->MenuPopup, lv_color_hex (C_CHROME), 0);
  lv_obj_set_style_bg_opa (A->MenuPopup, PanelOpa (0xE6), 0);
  lv_obj_set_style_radius (A->MenuPopup, 6, 0);
  lv_obj_set_style_border_width (A->MenuPopup, 1, 0);
  lv_obj_set_style_border_color (A->MenuPopup, lv_color_hex (0x4E5154), 0);
  lv_obj_set_style_pad_all (A->MenuPopup, 4, 0);
  lv_obj_set_style_pad_row (A->MenuPopup, 2, 0);
  lv_obj_set_flex_flow (A->MenuPopup, LV_FLEX_FLOW_COLUMN);
  lv_obj_align (
    A->MenuPopup,
    LV_ALIGN_TOP_LEFT,
    lv_obj_get_x (Title),
    lv_obj_get_y (Title) + UI_MENU_H
    );
  lv_obj_add_flag (A->MenuPopup, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_clear_flag (A->MenuPopup, LV_OBJ_FLAG_SCROLLABLE);

  for (I = 0; I < ARRAY_SIZE (Items); I++) {
    BOOLEAN   Danger = (BOOLEAN)(I == 6);   /* 退出 */
    lv_obj_t *Item   = lv_button_create (A->MenuPopup);
    lv_obj_t *ILbl;
    lv_obj_t *Lbl;

    /* Out of the keypad group on purpose: the popup runs its own focus index
       (MenuMove / MenuActivate below), because a real widget left in the group
       also receives LVGL's own CLICKED on ENTER - one keypress, two actions. */
    UiGroupDetach (Item);
    lv_obj_remove_style_all (Item);
    lv_obj_set_width (Item, LV_PCT (100));
    lv_obj_set_height (Item, 30);
    lv_obj_set_style_radius (Item, 4, 0);
    lv_obj_set_style_bg_color (Item, lv_color_hex (C_CHROME), 0);
    lv_obj_set_style_bg_color (Item, lv_color_hex (C_ROW_FOCUS), LV_STATE_HOVERED);
    /* Focus has to be unmistakable: the popup is driven by the arrow keys, and
       a highlight you have to hunt for is the same as no highlight. */
    lv_obj_set_style_bg_color (Item, lv_color_hex (0x33506B), LV_STATE_FOCUSED);
    lv_obj_set_style_border_side (Item, LV_BORDER_SIDE_LEFT, LV_STATE_FOCUSED);
    lv_obj_set_style_border_width (Item, 3, LV_STATE_FOCUSED);
    lv_obj_set_style_border_color (Item, lv_color_hex (C_FOCUS_RING),
                                   LV_STATE_FOCUSED);
    lv_obj_set_style_border_opa (Item, LV_OPA_COVER, LV_STATE_FOCUSED);
    lv_obj_set_style_pad_hor (Item, 8, 0);
    lv_obj_set_style_pad_column (Item, 8, 0);
    lv_obj_set_flex_flow (Item, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align (Item, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                           LV_FLEX_ALIGN_CENTER);

    ILbl = lv_label_create (Item);
    lv_label_set_text (ILbl, Icons[I]);
    IconFont (ILbl);
    lv_obj_set_style_text_color (
      ILbl, lv_color_hex (Danger ? C_DANGER_HI : C_DIM), 0
      );

    Lbl = lv_label_create (Item);
    lv_label_set_text (Lbl, Items[I]);
    UiFont (Lbl);
    lv_obj_set_style_text_color (
      Lbl, lv_color_hex (Danger ? C_DANGER_HI : C_TEXT), 0
      );

    lv_obj_add_event_cb (Item, MenuItemCb, LV_EVENT_CLICKED, (VOID *)(UINTN)I);
    lv_obj_add_flag (Item, LV_OBJ_FLAG_EVENT_BUBBLE);
    sMenuRow[sMenuCount] = Item;
    sMenuCount++;
  }

  /* A one-line legend: the popup is keyboard-driven and there is no other way
     to learn that. */
  {
    lv_obj_t *Legend = lv_label_create (A->MenuPopup);
    lv_label_set_text (Legend, "上下键选择   Enter 执行   Esc 关闭");
    UiFont (Legend);
    lv_obj_set_style_text_color (Legend, lv_color_hex (C_DIM), 0);
    lv_obj_set_style_pad_left (Legend, 4, 0);
  }

  /* Open with the first entry highlighted, so the popup shows where the arrow
     keys are before the user presses one. */
  MenuHighlight (0);
  DEBUG ((DEBUG_INFO, "[Ui] menu open items=%u\n", (UINT32)sMenuCount));
}

STATIC
VOID
MenuTitleCb (
  IN lv_event_t  *e
  )
{
  lv_obj_t  *Title = lv_event_get_target_obj (e);

  if (gApp->MenuPopup != NULL) {
    CloseMenu (gApp);
    return;
  }
  OpenMenu (gApp, Title);
}

STATIC
VOID
ToolButtonCb (
  IN lv_event_t  *e
  )
{
  UINT32 Action = (UINT32)(UINTN)lv_event_get_user_data (e);
  MenuAction (gApp, Action);
}

STATIC
lv_obj_t *
ToolButton (
  IN lv_obj_t     *Parent,
  IN CONST CHAR8  *Icon,
  IN CONST CHAR8  *Text,
  IN UINT32        Action,
  IN BOOLEAN       Danger
  )
{
  lv_obj_t *Btn = lv_button_create (Parent);
  lv_obj_t *Lbl;
  UINT32    Base = Danger ? C_DANGER : C_ACCENT;
  UINT32    BaseHi = Danger ? C_DANGER_HI : C_ACCENT_HI;
  UINT32    BaseLo = Danger ? C_DANGER_LO : C_ACCENT_LO;

  lv_obj_remove_style_all (Btn);
  EnsureButtonTransition ();
  lv_obj_set_style_bg_color (Btn, lv_color_hex (Base), 0);
  lv_obj_set_style_bg_grad_color (Btn, lv_color_hex (BaseLo), 0);
  lv_obj_set_style_bg_grad_dir (Btn, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_bg_opa (Btn, LV_OPA_COVER, 0);
  lv_obj_set_style_radius (Btn, 8, 0);
  lv_obj_set_style_pad_hor (Btn, 14, 0);
  lv_obj_set_style_pad_ver (Btn, 8, 0);
  lv_obj_set_style_pad_column (Btn, 6, 0);
  lv_obj_set_style_shadow_width (Btn, 10, 0);
  lv_obj_set_style_shadow_opa (Btn, LV_OPA_40, 0);
  lv_obj_set_style_shadow_color (Btn, lv_color_hex (0x000000), 0);
  lv_obj_set_style_shadow_offset_y (Btn, 2, 0);
  lv_obj_set_style_transition (Btn, &sBtnTrans, 0);
  lv_obj_set_flex_flow (Btn, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align (Btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                         LV_FLEX_ALIGN_CENTER);

  /* Hover lifts it, press pushes it into the surface, focus rings it. */
  lv_obj_set_style_bg_color (Btn, lv_color_hex (BaseHi), LV_STATE_HOVERED);
  lv_obj_set_style_bg_grad_color (Btn, lv_color_hex (Base), LV_STATE_HOVERED);
  lv_obj_set_style_shadow_width (Btn, 16, LV_STATE_HOVERED);
  lv_obj_set_style_shadow_opa (Btn, LV_OPA_50, LV_STATE_HOVERED);
  lv_obj_set_style_bg_color (Btn, lv_color_hex (BaseLo), LV_STATE_PRESSED);
  lv_obj_set_style_bg_grad_color (Btn, lv_color_hex (BaseLo), LV_STATE_PRESSED);
  lv_obj_set_style_shadow_width (Btn, 3, LV_STATE_PRESSED);
  lv_obj_set_style_shadow_offset_y (Btn, 0, LV_STATE_PRESSED);
  lv_obj_set_style_translate_y (Btn, 1, LV_STATE_PRESSED);
  lv_obj_set_style_border_width (Btn, 2, LV_STATE_FOCUSED);
  lv_obj_set_style_border_color (Btn, lv_color_hex (C_FOCUS_RING), LV_STATE_FOCUSED);
  lv_obj_set_style_border_opa (Btn, LV_OPA_COVER, LV_STATE_FOCUSED);

  if ((Icon != NULL) && (Icon[0] != 0)) {
    lv_obj_t *ILbl = lv_label_create (Btn);
    lv_label_set_text (ILbl, Icon);
    IconFont (ILbl);
    lv_obj_set_style_text_color (ILbl, lv_color_hex (0xFFFFFF), 0);
  }

  Lbl = lv_label_create (Btn);
  lv_label_set_text (Lbl, Text);
  UiFont (Lbl);
  lv_obj_set_style_text_color (Lbl, lv_color_hex (0xFFFFFF), 0);

  lv_obj_add_event_cb (Btn, ToolButtonCb, LV_EVENT_CLICKED, (VOID *)(UINTN)Action);
  lv_obj_add_flag (Btn, LV_OBJ_FLAG_EVENT_BUBBLE);
  UiGroupDetach (Btn);

  if (gApp->ButtonCount < APP_MAX_BUTTONS) {
    gApp->Buttons[gApp->ButtonCount++] = Btn;
  }
  return Btn;
}

/* --------------------------------------------------------------- focus */

VOID
MainWindowSetZone (
  IN OUT APP_CTX     *A,
  IN     FOCUS_ZONE   Zone
  )
{
  UINT32  I;

  A->Zone = Zone;

  /* Evidence for the Tab walk: which zone holds the focus, and where inside it. */
  DEBUG ((DEBUG_INFO, "[Ui] zone=%u row=%u button=%u\n",
          (UINT32)Zone, (UINT32)A->RowFocus, (UINT32)A->ButtonFocus));

  for (I = 0; I < A->TargetCount && I < APP_MAX_ROWS; I++) {
    if (A->Rows[I] == NULL) {
      continue;
    }
    if (Zone == FZ_TARGETS && I == A->RowFocus) {
      lv_obj_add_state (A->Rows[I], LV_STATE_FOCUSED);
    } else {
      lv_obj_remove_state (A->Rows[I], LV_STATE_FOCUSED);
    }
  }
  for (I = 0; I < A->ButtonCount; I++) {
    if (A->Buttons[I] == NULL) {
      continue;
    }
    if (Zone == FZ_BUTTONS && I == A->ButtonFocus) {
      lv_obj_add_state (A->Buttons[I], LV_STATE_FOCUSED);
    } else {
      lv_obj_remove_state (A->Buttons[I], LV_STATE_FOCUSED);
    }
  }
}

/** Tab / Shift+Tab walk ONE flat ring: the target list is a stop, then every
    button in order (the 操作 menu title, then 全选 / 全不选 / 算法… / 开始擦除…),
    then back to the list.

    Previously Tab only swapped between the list and the button zone, and the
    zone kept whichever button it had last focused - so from the list Tab
    always landed on 全选 again and the other three buttons could not be
    reached with Tab at all (reported as "Tab 只能在全选和磁盘列表之间切换").
    Left/Right still move between buttons without leaving the row. */
STATIC
VOID
FocusStep (
  IN OUT APP_CTX  *A,
  IN     BOOLEAN   Forward
  )
{
  if (A->ButtonCount == 0) {
    MainWindowSetZone (A, FZ_TARGETS);
    return;
  }

  if (Forward) {
    if (A->Zone != FZ_BUTTONS) {
      A->ButtonFocus = 0;
    } else if (A->ButtonFocus + 1 < A->ButtonCount) {
      A->ButtonFocus++;
    } else {
      MainWindowSetZone (A, FZ_TARGETS);
      return;
    }
  } else {
    if (A->Zone != FZ_BUTTONS) {
      A->ButtonFocus = A->ButtonCount - 1;
    } else if (A->ButtonFocus > 0) {
      A->ButtonFocus--;
    } else {
      MainWindowSetZone (A, FZ_TARGETS);
      return;
    }
  }
  MainWindowSetZone (A, FZ_BUTTONS);
}

/** Scroll the focused row into view. */
STATIC
VOID
EnsureRowVisible (
  IN OUT APP_CTX  *A
  )
{
  if (A->Rows[A->RowFocus] != NULL) {
    lv_obj_scroll_to_view (A->Rows[A->RowFocus], LV_ANIM_OFF);
  }
}

VOID
MainWindowToggleRow (
  IN OUT APP_CTX  *A,
  IN     UINT32    Row,
  IN     BOOLEAN   Ctrl,
  IN     BOOLEAN   Shift
  )
{
  UINT32  Lo;
  UINT32  Hi;
  UINT32  I;

  if (Row >= A->TargetCount) {
    return;
  }

  /* Gate 1 lives here, not in the confirmation dialog: a target the user can
     never queue up is safer than one that is refused at the last moment.
     Read-only media cannot be written at all, and the boot volume (the disk
     the app is running from, and the partition on it) would destroy the
     running image mid-write. Every other partition on the boot disk is
     selectable - erasing those is safe. */
  if (!TargetSelectable (&A->Targets[Row])) {
    /* The row is greyed and says why, but the click still explains itself:
       the hint bar is the answer to "I clicked it and nothing happened". */
    if (A->Targets[Row].ReadOnly) {
      MainWindowHint (A, "该目标不可选：只读设备，固件报告其不可写。");
    } else if (A->Targets[Row].Kind == TARGET_KIND_PARTITION) {
      MainWindowHint (A, "该目标不可选：当前启动卷上的分区正在运行本程序。");
    } else {
      MainWindowHint (A, "该目标不可选：当前启动卷所在整盘，擦除会毁掉本程序。");
    }
    DEBUG ((
      DEBUG_INFO,
      "[Ui] refuse row=%u name=%a readOnly=%d boot=%d\n",
      Row,
      A->Targets[Row].ReadOnly ? "read-only" : "boot-volume",
      A->Targets[Row].ReadOnly,
      A->Targets[Row].IsBootVolume
      ));
    return;
  }

  /* Past the gate: this selection is legitimate, so any refusal hint still on
     screen is stale. */
  MainWindowHintClear (A);

  if (Shift) {
    Lo = (A->LastClickedRow < Row) ? A->LastClickedRow : Row;
    Hi = (A->LastClickedRow < Row) ? Row : A->LastClickedRow;
    for (I = Lo; I <= Hi; I++) {
      if (!A->Targets[I].ReadOnly && !A->Targets[I].IsBootVolume) {
        A->Targets[I].Selected = TRUE;
      }
    }
  } else if (Ctrl) {
    A->Targets[Row].Selected = (BOOLEAN)!A->Targets[Row].Selected;
    A->LastClickedRow = Row;
  } else {
    /* Plain click: toggle just this row. Not "select only this one" - with
       destructive targets, losing a selection the user built up is worse than
       requiring an explicit deselect. */
    A->Targets[Row].Selected = (BOOLEAN)!A->Targets[Row].Selected;
    A->LastClickedRow = Row;
  }

  A->RowFocus = Row;
  MainWindowRefreshList (A);
}

/* ------------------------------------------------------------- key input */

BOOLEAN
MainWindowKey (
  IN OUT APP_CTX  *A,
  IN     UINT32    Key
  )
{
  UINT32 Mods = LvglKbdGetModifiers ();

  /* Key routing is the one path that fails silently: a key swallowed one level
     short looks exactly like a key that never arrived. One line per key is
     cheap (keys are rare) and makes that distinguishable. */
  DEBUG ((DEBUG_INFO, "[Ui] key=0x%x overlay=%d zone=%d\n",
          Key, (A->Overlay != NULL) ? 1 : 0, (UINT32)A->Zone));

  /* A modal owns the keyboard first: it must be able to swallow ESC and Tab so
     they cannot reach the window behind it. */
  if (A->Overlay != NULL) {
    return DialogsKey (A, Key);
  }

  /* Menu open: ESC closes, arrows/Enter drive it. */
  if (A->MenuPopup != NULL) {
    if (Key == LV_KEY_ESC) {
      CloseMenu (A);
      return TRUE;
    }
    if (Key == LV_KEY_ENTER) {
      MenuActivate ();
      return TRUE;
    }
    if ((Key == LVGL_KEY_TAB) || (Key == LVGL_KEY_TAB_PREV)) {
      /* Tab moves inside the popup, exactly like the arrow keys. It used to
         close the menu and carry on round the window's focus ring, which read
         as "the keys do not work in here" - Tab is the natural next-item key,
         and a popup that vanishes instead of moving is a trap. */
      MenuMove ((Key == LVGL_KEY_TAB) ? 1 : -1);
      return TRUE;
    }
    if (Key == LV_KEY_DOWN || Key == LV_KEY_UP) {
      MenuMove ((Key == LV_KEY_DOWN) ? 1 : -1);
      return TRUE;
    }
    return TRUE;
  }

  switch (Key) {
    case LVGL_KEY_TAB:
      FocusStep (A, (BOOLEAN)((Mods & LVGL_KBD_MOD_SHIFT) == 0));
      return TRUE;
    case LVGL_KEY_TAB_PREV:
      FocusStep (A, FALSE);
      return TRUE;

    case LV_KEY_ESC:
      A->Quit = TRUE;
      return TRUE;

    default:
      break;
  }

  if (A->Zone == FZ_TARGETS) {
    switch (Key) {
      case LV_KEY_DOWN:
        if (A->TargetCount > 0 && A->RowFocus + 1 < A->TargetCount) {
          A->RowFocus++;
          MainWindowSetZone (A, FZ_TARGETS);
          EnsureRowVisible (A);
        }
        return TRUE;
      case LV_KEY_UP:
        if (A->RowFocus > 0) {
          A->RowFocus--;
          MainWindowSetZone (A, FZ_TARGETS);
          EnsureRowVisible (A);
        }
        return TRUE;
      case ' ':
        MainWindowToggleRow (A, A->RowFocus, FALSE, FALSE);
        return TRUE;
      case LV_KEY_ENTER:
        MainWindowToggleRow (
          A,
          A->RowFocus,
          (BOOLEAN)((Mods & LVGL_KBD_MOD_CTRL) != 0),
          (BOOLEAN)((Mods & LVGL_KBD_MOD_SHIFT) != 0)
          );
        return TRUE;
      default:
        break;
    }
  }

  if (A->Zone == FZ_BUTTONS && A->ButtonCount > 0) {
    switch (Key) {
      case LV_KEY_DOWN:
        /* Down leaves the toolbar for the target list right below it (and Up
           goes back to the 操作 menu button), so the arrow keys move between
           the zones as well as inside them - which also gives a keyboard user
           one deterministic way back to the list. */
        MainWindowSetZone (A, FZ_TARGETS);
        return TRUE;
      case LV_KEY_UP:
        A->ButtonFocus = 0;
        MainWindowSetZone (A, FZ_BUTTONS);
        return TRUE;
      case LV_KEY_RIGHT:
        A->ButtonFocus = (A->ButtonFocus + 1) % A->ButtonCount;
        MainWindowSetZone (A, FZ_BUTTONS);
        return TRUE;
      case LV_KEY_LEFT:
        A->ButtonFocus = (A->ButtonFocus + A->ButtonCount - 1) % A->ButtonCount;
        MainWindowSetZone (A, FZ_BUTTONS);
        return TRUE;
      case LV_KEY_ENTER:
        lv_obj_send_event (A->Buttons[A->ButtonFocus], LV_EVENT_CLICKED, NULL);
        return TRUE;
      default:
        break;
    }
  }

  /* Global shortcuts, available from any zone. */
  if (Mods & LVGL_KBD_MOD_CTRL) {
    switch (Key) {
      case 'a':
      case 'A':
        MenuAction (A, 0);
        return TRUE;
      default:
        break;
    }
  }
  switch (Key) {
    case LVGL_KEY_F1:
      DialogsAbout (A);
      return TRUE;
    case LVGL_KEY_F5:
      MenuAction (A, 2);          /* 选择算法 */
      return TRUE;
    case LV_KEY_DEL:
      /* 全不选, NOT 开始擦除: Del reads as "clear what I picked", and a key
         that opens the destruction chain is the last thing that belongs on
         the unmodified Del. */
      MenuAction (A, 1);
      return TRUE;
    default:
      break;
  }

  return FALSE;
}

STATIC
VOID
ScrKeyCb (
  IN lv_event_t  *e
  )
{
  UINT32 Key;

  if (lv_event_get_code (e) != LV_EVENT_KEY) {
    return;
  }
  Key = lv_indev_get_key (lv_indev_active ());
  MainWindowKey (gApp, Key);
}

STATIC
VOID
RowClickCb (
  IN lv_event_t  *e
  )
{
  UINT32     Row = (UINT32)(UINTN)lv_event_get_user_data (e);
  UINT32     Mods = LvglKbdGetModifiers ();

  MainWindowToggleRow (
    gApp,
    Row,
    (BOOLEAN)((Mods & LVGL_KBD_MOD_CTRL) != 0),
    (BOOLEAN)((Mods & LVGL_KBD_MOD_SHIFT) != 0)
    );
}

/* ---------------------------------------------------------------- pump */

VOID
MainWindowPump (
  IN VOID  *Ctx
  )
{
  APP_CTX  *A = (APP_CTX *)Ctx;
  UINT32   TillNext;

  STATIC BOOLEAN  sPumpLogged = FALSE;

  DialogsUpdateProgress (A);

  /* Give LVGL a slice: the erasure loop is synchronous, so this is the only
     place timers run and input is read while a job is in flight.

     One line, once per run, recording what lv_timer_handler () reports. It is
     worth keeping: when this returns 1 immediately on every pump it means LVGL
     refused the call as re-entrant, which is exactly how the progress dialog
     once went unpainted for a whole erasure - the erase was running inside the
     timer handler, so no timer (and therefore no animation or refresh) could
     run until it finished. If this line ever shows next=1 again, look at
     WHERE the erasure is being started from. */
  TillNext = lv_timer_handler ();

  if (!sPumpLogged) {
    sPumpLogged = TRUE;
    DEBUG ((
      DEBUG_INFO,
      "[Ui] pump: first slice at t=%u ms, next timer in %u ms\n",
      LvglTickGetMs (),
      TillNext
      ));
  }

  LvglPortPoll ();
}

/* --------------------------------------------------------------- create */

VOID
MainWindowCreate (
  IN OUT APP_CTX  *A,
  IN     lv_obj_t *Scr
  )
{
  /* CONTENT size, not object size. With the glass card as the host the two
     differ by the card's padding, and children are laid out (and clipped)
     inside the content box: laying out against the outer box put the status
     bar's lower half outside the card, which clipped it - the "字只有一半"
     report. Without glass they are the same and this is a no-op. */
  INT32     ScrW = lv_obj_get_content_width (Scr);
  INT32     ScrH = lv_obj_get_content_height (Scr);
  lv_obj_t *Menu;
  lv_obj_t *MenuTitle = NULL;
  lv_obj_t *Tool;
  lv_obj_t *Status;
  lv_obj_t *Ver;
  CHAR8     VerText[96];

  A->Scr = Scr;
  gApp   = A;

  /* ---- key sink ---------------------------------------------------------
     The window keeps exactly one object in LVGL's default keypad group and it
     does nothing: 1x1, styleless, never focused by the UI. Every real widget is
     removed from the group by UiGroupDetach (). Without this, one Enter press
     ran two actions - the group member's own CLICKED (LVGL's ENTER handling)
     plus this window's ring handler. The capture run showed the symptom: the
     操作 menu opened by itself and then ate the whole keyboard. */
  A->KeySink = lv_obj_create (Scr);
  lv_obj_remove_style_all (A->KeySink);
  lv_obj_set_size (A->KeySink, 1, 1);
  lv_obj_set_pos (A->KeySink, 0, 0);
  lv_obj_add_flag (A->KeySink, LV_OBJ_FLAG_EVENT_BUBBLE);
  if (lv_group_get_default () != NULL) {
    lv_group_add_obj (lv_group_get_default (), A->KeySink);
    lv_group_focus_obj (A->KeySink);
  }
  DEBUG ((DEBUG_INFO, "[Ui] key sink %s the default group\n",
          (lv_group_get_default () != NULL) ? "in" : "NOT in"));

  if (ScrW != 1280 || ScrH != 800) {
    DEBUG ((DEBUG_INFO,
            "[Ui] host content %dx%d (screen layout is tuned for 1280x800)\n",
            ScrW, ScrH));
  }

  lv_obj_set_user_data (Scr, A);
  /* With glass the host is the card, and painting it solid would hide the very
     thing it exists for. Without glass it is the screen and needs its colour. */
  mGlassActive = GlassChromeActive ();
  lv_obj_set_style_bg_opa (Scr, mGlassActive ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color (Scr, lv_color_hex (C_BG), 0);

  /* ---- menu bar ---- */
  Menu = lv_obj_create (Scr);
  lv_obj_remove_style_all (Menu);
  lv_obj_set_style_bg_color (Menu, lv_color_hex (C_CHROME), 0);
  lv_obj_set_style_bg_opa (Menu, PanelOpa (0x99), 0);
  lv_obj_set_pos (Menu, 0, 0);
  lv_obj_set_size (Menu, LV_PCT (100), UI_MENU_H);
  lv_obj_add_flag (Menu, LV_OBJ_FLAG_EVENT_BUBBLE);
  A->MenuBar = Menu;
  {
    lv_obj_t *Title  = lv_button_create (Menu);
    lv_obj_t *ILbl;
    lv_obj_t *Lbl;

    UiGroupDetach (Title);
    lv_obj_remove_style_all (Title);
    EnsureButtonTransition ();
    lv_obj_set_style_bg_color (Title, lv_color_hex (C_CHROME), 0);
    lv_obj_set_style_bg_opa (Title, LV_OPA_COVER, 0);
    lv_obj_set_style_radius (Title, 4, 0);
    lv_obj_set_style_bg_color (Title, lv_color_hex (C_ROW_FOCUS), LV_STATE_PRESSED);
    lv_obj_set_style_bg_color (Title, lv_color_hex (C_ROW_FOCUS), LV_STATE_HOVERED);
    lv_obj_set_style_border_width (Title, 2, LV_STATE_FOCUSED);
    lv_obj_set_style_border_color (Title, lv_color_hex (C_FOCUS_RING),
                                   LV_STATE_FOCUSED);
    lv_obj_set_style_border_opa (Title, LV_OPA_COVER, LV_STATE_FOCUSED);
    lv_obj_set_style_pad_hor (Title, 10, 0);
    lv_obj_set_style_pad_column (Title, 6, 0);
    lv_obj_set_pos (Title, 4, 2);
    lv_obj_set_height (Title, UI_MENU_H - 4);
    lv_obj_set_flex_flow (Title, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align (Title, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                           LV_FLEX_ALIGN_CENTER);
    ILbl = lv_label_create (Title);
    lv_label_set_text (ILbl, LV_SYMBOL_BARS);
    IconFont (ILbl);
    lv_obj_set_style_text_color (ILbl, lv_color_hex (C_DIM), 0);
    Lbl = lv_label_create (Title);
    lv_label_set_text (Lbl, "操作");
    UiFont (Lbl);
    lv_obj_set_style_text_color (Lbl, lv_color_hex (C_TEXT), 0);
    lv_obj_add_event_cb (Title, MenuTitleCb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag (Title, LV_OBJ_FLAG_EVENT_BUBBLE);
    MenuTitle = Title;
  }
  /* Author watermark, matching the sibling projects. */
  {
    lv_obj_t *ALbl = lv_label_create (Menu);
    lv_label_set_text (ALbl, "Author : Mike Wu");
    UiFont (ALbl);
    lv_obj_set_style_text_color (ALbl, lv_color_hex (C_DIM), 0);
    lv_obj_align (ALbl, LV_ALIGN_TOP_RIGHT, -8, (UI_MENU_H - 16) / 2);
  }

  /* ---- toolbar ---- */
  Tool = lv_obj_create (Scr);
  lv_obj_remove_style_all (Tool);
  lv_obj_set_style_bg_color (Tool, lv_color_hex (0x252526), 0);
  lv_obj_set_style_bg_opa (Tool, PanelOpa (0x99), 0);
  lv_obj_set_pos (Tool, 0, UI_MENU_H);
  lv_obj_set_size (Tool, LV_PCT (100), UI_TOOL_H);
  lv_obj_set_flex_flow (Tool, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align (Tool, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                         LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_left (Tool, 8, 0);
  lv_obj_set_style_pad_column (Tool, 8, 0);
  lv_obj_add_flag (Tool, LV_OBJ_FLAG_EVENT_BUBBLE);
  A->ToolBar = Tool;

  A->ButtonCount = 0;
  /* The menu title joins the same ring: it is a button too, and without this
     there was no keyboard path to the 操作 menu at all. */
  if (MenuTitle != NULL) {
    A->Buttons[A->ButtonCount++] = MenuTitle;
  }
  ToolButton (Tool, LV_SYMBOL_OK,       "全选",   0, FALSE);
  ToolButton (Tool, LV_SYMBOL_CLOSE,    "全不选", 1, FALSE);
  ToolButton (Tool, LV_SYMBOL_SETTINGS, "算法…",  2, FALSE);
  ToolButton (Tool, LV_SYMBOL_TRASH,    "开始擦除…", 3, TRUE);

  /* ---- target list ---- */
  A->ListBox = lv_obj_create (Scr);
  lv_obj_remove_style_all (A->ListBox);
  lv_obj_set_style_bg_color (A->ListBox, lv_color_hex (C_BG), 0);
  lv_obj_set_style_bg_opa (A->ListBox, PanelOpa (0x59), 0);
  lv_obj_set_pos (A->ListBox, 0, UI_MENU_H + UI_TOOL_H);
  lv_obj_set_size (
    A->ListBox,
    LV_PCT (100),
    ScrH - UI_MENU_H - UI_TOOL_H - UI_STATUS_H
    );
  lv_obj_set_style_pad_all (A->ListBox, 6, 0);
  lv_obj_set_style_pad_row (A->ListBox, 2, 0);
  lv_obj_set_flex_flow (A->ListBox, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scrollbar_mode (A->ListBox, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_add_flag (A->ListBox, LV_OBJ_FLAG_EVENT_BUBBLE);

  /* ---- transient hint bar (above the status bar, hidden until needed) ---- */
  A->HintBar = lv_obj_create (Scr);
  lv_obj_remove_style_all (A->HintBar);
  lv_obj_set_pos (A->HintBar, 8, ScrH - UI_STATUS_H - 34);
  lv_obj_set_size (A->HintBar, ScrW - 16, 30);
  lv_obj_set_style_bg_color (A->HintBar, lv_color_hex (0x3A3320), 0);
  lv_obj_set_style_bg_opa (A->HintBar, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width (A->HintBar, 1, 0);
  lv_obj_set_style_border_color (A->HintBar, lv_color_hex (0xE0A030), 0);
  lv_obj_set_style_radius (A->HintBar, 6, 0);
  lv_obj_set_style_pad_hor (A->HintBar, 10, 0);
  lv_obj_set_style_pad_column (A->HintBar, 8, 0);
  lv_obj_set_flex_flow (A->HintBar, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align (A->HintBar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                         LV_FLEX_ALIGN_CENTER);
  lv_obj_add_flag (A->HintBar, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_add_flag (A->HintBar, LV_OBJ_FLAG_HIDDEN);
  {
    lv_obj_t *ILbl = lv_label_create (A->HintBar);
    lv_label_set_text (ILbl, LV_SYMBOL_WARNING);
    IconFont (ILbl);
    lv_obj_set_style_text_color (ILbl, lv_color_hex (0xE0A030), 0);
  }
  A->HintLabel = lv_label_create (A->HintBar);
  lv_label_set_text (A->HintLabel, "");
  UiFont (A->HintLabel);
  lv_obj_set_style_text_color (A->HintLabel, lv_color_hex (0xF0D9A0), 0);

  /* ---- status bar ---- */
  Status = lv_obj_create (Scr);
  lv_obj_remove_style_all (Status);
  lv_obj_set_style_bg_color (Status, lv_color_hex (C_CHROME), 0);
  lv_obj_set_style_bg_opa (Status, PanelOpa (0x99), 0);
  lv_obj_set_pos (Status, 0, ScrH - UI_STATUS_H);
  lv_obj_set_size (Status, LV_PCT (100), UI_STATUS_H);
  /* Three cells in a flex row, not three absolutely-placed labels. Aligned by
     hand (left / centre / right) they overlapped as soon as the strings grew:
     a long algorithm name plus "待写总量 1.2 TB (35 遍)" plus the version
     watermark is wider than the bar. Flex gives each cell its own box and the
     dots mode clips inside that box instead of over its neighbour.

     The version cell keeps LV_SIZE_CONTENT and does not grow: the watermark is
     run evidence and must never be elided on a narrow screen. */
  lv_obj_set_flex_flow (Status, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align (Status, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                         LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_left (Status, 8, 0);
  lv_obj_set_style_pad_right (Status, 8, 0);
  lv_obj_set_style_pad_column (Status, 12, 0);

  A->AlgoLabel = lv_label_create (Status);
  UiFont (A->AlgoLabel);
  lv_obj_set_style_text_color (A->AlgoLabel, lv_color_hex (C_DIM), 0);
  lv_obj_set_width (A->AlgoLabel, 1);
  lv_obj_set_flex_grow (A->AlgoLabel, 2);
  lv_label_set_long_mode (A->AlgoLabel, LV_LABEL_LONG_DOT);
  /* Exactly one line high: a label that wraps grows to two lines and the bar
     clips both of them, and a box that is taller than the text makes LVGL draw
     the text at the top of the box instead of centring it in the bar. */
  lv_obj_set_height (A->AlgoLabel,
                     lv_font_get_line_height (&lv_font_simsun_16_cjk));

  A->StatusLabel = lv_label_create (Status);
  UiFont (A->StatusLabel);
  lv_obj_set_style_text_color (A->StatusLabel, lv_color_hex (C_TEXT), 0);
  lv_obj_set_width (A->StatusLabel, 1);
  lv_obj_set_flex_grow (A->StatusLabel, 3);
  lv_label_set_long_mode (A->StatusLabel, LV_LABEL_LONG_DOT);
  lv_obj_set_height (A->StatusLabel,
                     lv_font_get_line_height (&lv_font_simsun_16_cjk));
  lv_obj_set_style_text_align (A->StatusLabel, LV_TEXT_ALIGN_CENTER, 0);

  /* Version watermark: the run-evidence requirement. It must be readable in a
     screendump so a screenshot alone ties a frame to a build. */
  Ver = lv_label_create (Status);
  AsciiSPrint (VerText, sizeof (VerText), "%a", UEFIERASER_VERSION_STR);
  lv_label_set_text (Ver, VerText);
  UiFont (Ver);
  lv_obj_set_style_text_color (Ver, lv_color_hex (C_DIM), 0);
  lv_obj_set_width (Ver, LV_SIZE_CONTENT);
  lv_obj_set_style_text_align (Ver, LV_TEXT_ALIGN_RIGHT, 0);

  /* Row click handling is per-row; register on the container's children as they
     are created, which MainWindowRefreshList does. Hook the container once for
     clicks on empty space (deselect nothing, just move focus there). */
  /* Exactly one hook, and it goes on the active screen - not on the host.
     With the glass card as the host, a key pressed with nothing focused is
     delivered to the screen rather than the card, and anything the card does
     deliver bubbles up to the screen anyway (the card carries EVENT_BUBBLE).
     Hooking both ends processed every key twice: the capture run showed the
     second ESC quitting the app instead of closing a dialog. */
  lv_obj_add_event_cb (lv_screen_active (), ScrKeyCb, LV_EVENT_KEY, NULL);

  MainWindowRefreshList (A);
  MainWindowRefreshAlgo (A);
  MainWindowSetZone (A, FZ_TARGETS);
}
