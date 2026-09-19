/** @file
  Ui/Dialogs.c - modal dialogs and the destruction chain.

  The confirmation design is the point of this file. Destroying a disk by
  mis-click is unrecoverable, so the path from "I want to erase" to "bytes are
  being written" has four gates:

    1. read-only devices are not selectable at all (enforced in
       MainWindowToggleRow, not here)
    2. the boot volume is flagged and excluded from 全选
    3. a summary dialog: how many targets, how many bytes, which algorithm,
       how many passes - the numbers, not a yes/no
    4. type-to-confirm: the literal string ERASE, and when a WHOLE DISK is
       selected, additionally that disk's capacity in GiB

  Gate 4 exists because a button can be clicked reflexively and a text field
  cannot. The GiB gate is the one that forces the user to look at the actual
  size of what they are about to destroy.

  Copyright (c) 2026, Mike Wu. All rights reserved.
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/PrintLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/LvglLib.h>
#include <Library/LvglUefiPort.h>
#include <Library/LvglTrans.h>
#include "GlassChrome.h"

#include "../Version.h"
#include "../Platform/TextConv.h"
#include "AppCtx.h"
#include "Dialogs.h"
#include "MainWindow.h"
#include "../Platform/AtaErase.h"
#include "../Platform/NvmeErase.h"
#include "../Platform/LogPersist.h"

LV_FONT_DECLARE (lv_font_simsun_16_cjk);

typedef enum {
  DLG_NONE = 0,
  DLG_ALGO,
  DLG_SUMMARY,
  DLG_TYPED,
  DLG_PROGRESS,
  DLG_REPORT,
  DLG_ABOUT
} DLG_STATE;

/* Single-window application: one modal at a time, so file-static state is the
   honest representation. */
STATIC DLG_STATE  sState = DLG_NONE;
STATIC CHAR8      sTyped[32];
STATIC CHAR8      sExpected[32];       /**< what must be typed to proceed */
STATIC UINT32     sGateIndex;          /**< 0 = ERASE, 1 = capacity */
STATIC lv_obj_t  *sTypedLabel;
STATIC lv_obj_t  *sProgressBar;
STATIC lv_obj_t  *sProgressLabel;
STATIC lv_obj_t  *sConfirmBtn;
STATIC lv_obj_t  *sExportLabel;
/** Hint line under the typed field: what is wrong with the input, in red until
    the text matches. "Direct 确认 did nothing" was a real report. */
STATIC lv_obj_t  *sHintLabel;
/** The button Enter activates. Without it a keyboard-only user reaches the
    summary dialog and stops: LVGL hands the key to a group that has nothing
    focused inside a freshly built modal, so Enter would be swallowed. */
STATIC lv_obj_t  *sDefaultBtn;
/** The modal card (the child of the overlay). Transitions animate this, not
    the full-screen scrim, and the exit callback deletes the overlay. */
STATIC lv_obj_t  *sCard;
/** The effect each dialog enters and leaves with. Stored so the exit is the
    mirror of the entry even if the default changes later. */
STATIC LV_TRANS_EFFECT  mTransEff = LV_TRANS_SHEET_UP;

/* ---- dialog focus ring -------------------------------------------------
   LVGL only delivers a key press to the focused object of the keypad group
   (lv_indev.c returns early when the group has no focus), and a freshly built
   modal has nothing focused. The dialogs therefore keep their own ring: every
   button registers here, Tab and Shift+Tab walk it, Enter presses the focused
   entry, and the focus is drawn with the FOCUSED style these buttons already
   carry. Before this, Tab inside a modal was swallowed and did nothing. */
#define MODAL_FOCUS_MAX  24
STATIC lv_obj_t      *sFocusBtn[MODAL_FOCUS_MAX];
STATIC CONST CHAR8   *sFocusName[MODAL_FOCUS_MAX];
STATIC UINT32         sFocusCount;
STATIC UINT32         sFocusIdx;

/** A saved copy of the ring, so a popup can borrow the ring and give it back. */
typedef struct {
  lv_obj_t      *Btn[MODAL_FOCUS_MAX];
  CONST CHAR8   *Name[MODAL_FOCUS_MAX];
  UINT32         Count;
  UINT32         Idx;
} FOCUS_SAVE;

/** In-card notice popup. Deliberately NOT a second overlay: it is a child of
    the dialog card, so dismissing it leaves the dialog exactly as it was
    (including its ring, which is saved and restored around it). */
STATIC lv_obj_t   *sNoticePanel;
STATIC BOOLEAN     sNoticeKeepsTyping;   /**< notice over the typed gate */
STATIC FOCUS_SAVE  sRingSave;

STATIC
VOID
FocusAdd (
  IN lv_obj_t       *Obj,
  IN CONST CHAR8    *Name
  )
{
  if ((Obj == NULL) || (sFocusCount >= MODAL_FOCUS_MAX)) {
    return;
  }
  sFocusBtn[sFocusCount]  = Obj;
  sFocusName[sFocusCount] = Name;
  sFocusCount++;
}

STATIC
VOID
ModalFocusApply (
  IN UINT32  Index
  )
{
  UINT32  I;

  if (sFocusCount == 0) {
    return;
  }
  sFocusIdx = Index % sFocusCount;
  for (I = 0; I < sFocusCount; I++) {
    if (I == sFocusIdx) {
      lv_obj_add_state (sFocusBtn[I], LV_STATE_FOCUSED);
    } else {
      lv_obj_remove_state (sFocusBtn[I], LV_STATE_FOCUSED);
    }
  }
  DEBUG ((DEBUG_INFO, "[Ui] dialog focus %u/%u '%a'\n",
          sFocusIdx + 1, sFocusCount,
          (sFocusName[sFocusIdx] != NULL) ? sFocusName[sFocusIdx] : "?"));
}

/** Move the ring by ±1 (wraps). */
STATIC
VOID
ModalFocusStep (
  IN INT32  Delta
  )
{
  if (sFocusCount == 0) {
    return;
  }
  ModalFocusApply (
    (UINT32)(((INT32)sFocusIdx + Delta + (INT32)sFocusCount)
             % (INT32)sFocusCount)
    );
}

/** Focus the dialog's default button if it is registered, else the first. */
STATIC
VOID
ModalFocusDefault (
  VOID
  )
{
  UINT32  I;

  if (sFocusCount == 0) {
    return;
  }
  for (I = 0; I < sFocusCount; I++) {
    if (sFocusBtn[I] == sDefaultBtn) {
      ModalFocusApply (I);
      return;
    }
  }
  ModalFocusApply (0);
}
STATIC UINT32     sRunIndex;           /**< which selected target is running */
/** Last progress value that was logged, so the serial log carries a coarse
    trace of the erasure's progress. This is the cheap, assertable counterpart
    of the progress dialog: the bug it replaces froze the screen instead. */
STATIC UINT32     sProgLogPct = 0xFFFFFFFFu;
STATIC UINT32     sRunTotal;           /**< how many will run */

/** The last run's summary, kept so the export button can write the same
    numbers the report dialog is showing. */
STATIC ERASE_REPORT_SUMMARY  sReportSummary;
STATIC CHAR16                sReportTargets[1024];

/* Map ERASE_STATUS to a short result string for the audit log. */
STATIC
CONST CHAR8 *
ResultFromStatus (
  IN ERASE_STATUS  Status,
  IN BOOLEAN       Cancelled
  )
{
  if (Cancelled) {
    return "CANCELLED";
  }
  switch (Status) {
    case ERASE_OK:            return "OK";
    case ERASE_ERR_VERIFY:    return "VERIFY_FAIL";
    case ERASE_ERR_IO:        return "IO_ERROR";
    case ERASE_ERR_PARAM:     return "PARAM_ERROR";
    case ERASE_ERR_CANCELLED: return "CANCELLED";
    case ERASE_ERR_MEDIA_CHANGED: return "MEDIA_CHANGED";
    default:                  return "UNKNOWN";
  }
}

/* Map UI ERASE_MODE to a string for the log. */
STATIC
CONST CHAR8 *
ModeNameForLog (
  IN ERASE_MODE  Mode
  )
{
  switch (Mode) {
    case ERASE_MODE_OVERWRITE:      return "overwrite";
    case ERASE_MODE_ATA:            return "ata";
    case ERASE_MODE_NVME_FORMAT:    return "nvme-format";
    case ERASE_MODE_NVME_SANITIZE:  return "nvme-sanitize";
    default:                        return "overwrite";
  }
}

/* Aggregated across all targets of one run, for the report. */
STATIC UINT64     sAggBytes;
STATIC UINT64     sAggPlanned;
STATIC UINT32     sAggPasses;
STATIC UINT32     sAggMismatches;
STATIC UINT32     sAggTargets;
STATIC UINT32     sAggFailed;
STATIC BOOLEAN    sAggCancelled;
STATIC BOOLEAN    sAggVerified;
STATIC ERASE_STATUS sAggStatus;
/* Why the run failed, when the platform layer could say (device-level modes:
   "no ATA Pass-Thru", "no Security feature set", "FROZEN"...). First failure
   wins - the dialog has one line for it and the first one is what to act on. */
STATIC CONST CHAR8 *sAggReason;

/* ------------------------------------------------------------- helpers */

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

STATIC
VOID
CloseModal (
  IN OUT APP_CTX  *A
  );

STATIC
VOID
RingSaveNow (
  VOID
  )
{
  UINT32  I;

  for (I = 0; (I < sFocusCount) && (I < MODAL_FOCUS_MAX); I++) {
    sRingSave.Btn[I]  = sFocusBtn[I];
    sRingSave.Name[I] = sFocusName[I];
  }
  sRingSave.Count = sFocusCount;
  sRingSave.Idx   = sFocusIdx;
}

STATIC
VOID
RingRestoreNow (
  VOID
  )
{
  UINT32  I;
  UINT32  N = 0;

  sFocusCount = 0;
  for (I = 0; (I < sRingSave.Count) && (I < MODAL_FOCUS_MAX); I++) {
    if ((sRingSave.Btn[I] != NULL) && lv_obj_is_valid (sRingSave.Btn[I])) {
      sFocusBtn[N]  = sRingSave.Btn[I];
      sFocusName[N] = sRingSave.Name[I];
      N++;
    }
  }
  sFocusCount = N;
  sFocusIdx   = 0;
  if (N > 0) {
    ModalFocusApply ((sRingSave.Idx < N) ? sRingSave.Idx : 0);
  }
}

/** The single deletion point for an exited modal: LvglTransHide calls this
    exactly once, and it owns the overlay the animated card lives in. */
STATIC
VOID
ModalDeleteDone (
  IN lv_obj_t  *Obj,
  IN VOID      *UserData
  )
{
  lv_obj_t  *Overlay = (lv_obj_t *)UserData;

  (VOID)Obj;
  if ((Overlay != NULL) && lv_obj_is_valid (Overlay)) {
    lv_obj_delete (Overlay);
  }
}

STATIC
VOID
CloseModal (
  IN OUT APP_CTX  *A
  )
{
  if (A->Overlay != NULL) {
    lv_obj_t  *Overlay = A->Overlay;
    lv_obj_t  *Card    = sCard;

    A->Overlay = NULL;
    sCard      = NULL;
    if ((Card != NULL) && LvglTransIsEnabled ()) {
      /* The outgoing scrim must not swallow clicks on its way out. */
      lv_obj_remove_flag (Overlay, LV_OBJ_FLAG_CLICKABLE);
      if (LvglTransHide (Card, mTransEff, ModalDeleteDone, Overlay)
          != LV_RESULT_OK) {
        /* No animation (degenerate path, or the object is gone): delete now,
           otherwise the DoneCb that owns the deletion never runs and the
           overlay leaks. */
        lv_obj_delete (Overlay);
      }
    } else {
      lv_obj_delete (Overlay);
    }
  }
  sState         = DLG_NONE;
  sTypedLabel    = NULL;
  sProgressBar   = NULL;
  sProgressLabel = NULL;
  sConfirmBtn    = NULL;
  sDefaultBtn    = NULL;
  sExportLabel   = NULL;
  sHintLabel     = NULL;
  sNoticePanel   = NULL;
  sNoticeKeepsTyping = FALSE;
  sTyped[0]      = 0;
}

/** A centred modal card. Everything else in this file builds on it. */
STATIC
lv_obj_t *
ModalCreate (
  IN OUT APP_CTX  *A,
  IN     INT32     Width,
  IN     INT32     Height
  )
{
  lv_obj_t *Card;

  CloseModal (A);

  /* A full-screen scrim: it also swallows clicks so the window behind cannot be
     interacted with while a modal is up. */
  A->Overlay = lv_obj_create (A->Scr);
  lv_obj_remove_style_all (A->Overlay);
  lv_obj_set_size (A->Overlay, LV_PCT (100), LV_PCT (100));
  lv_obj_set_pos (A->Overlay, 0, 0);
  lv_obj_set_style_bg_color (A->Overlay, lv_color_hex (0x000000), 0);
  lv_obj_set_style_bg_opa (A->Overlay, LV_OPA_50, 0);
  lv_obj_add_flag (A->Overlay, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_clear_flag (A->Overlay, LV_OBJ_FLAG_SCROLLABLE);

  Card = lv_obj_create (A->Overlay);
  lv_obj_remove_style_all (Card);
  lv_obj_set_size (Card, Width, Height);
  lv_obj_center (Card);
  lv_obj_set_style_bg_color (Card, lv_color_hex (0x2D2D30), 0);
  /* Nearly opaque: text has to stay crisp, but a touch of the glass behind
     keeps the dialog in the same material language as the window. */
  lv_obj_set_style_bg_opa (Card, GlassChromeActive () ? 0xF0 : LV_OPA_COVER, 0);
  sCard       = Card;
  sFocusCount = 0;
  sFocusIdx   = 0;
  lv_obj_set_style_radius (Card, 6, 0);
  lv_obj_set_style_pad_all (Card, 14, 0);
  lv_obj_set_style_border_width (Card, 1, 0);
  lv_obj_set_style_border_color (Card, lv_color_hex (0x4E5154), 0);
  lv_obj_clear_flag (Card, LV_OBJ_FLAG_SCROLLABLE);
  /* Every level needs this or a key press stops at the card instead of
     reaching the screen handler - the same trap the list rows hit. It only
     showed up once the glass card became the UI host: with no object focused
     inside the modal, LVGL hands the key to the active screen, and the card
     has to pass it up. */
  lv_obj_add_flag (Card, LV_OBJ_FLAG_EVENT_BUBBLE);
  return Card;
}

STATIC
lv_obj_t *
ModalLabel (
  IN lv_obj_t     *Parent,
  IN CONST CHAR8  *Utf8,
  IN UINT32        Color,
  IN INT32         X,
  IN INT32         Y
  )
{
  lv_obj_t *L = lv_label_create (Parent);

  lv_label_set_text (L, Utf8);
  UiFont (L);
  lv_obj_set_style_text_color (L, lv_color_hex (Color), 0);
  lv_obj_set_pos (L, X, Y);
  return L;
}

/* Same transition as the toolbar buttons (MainWindow.c), for the same reason:
   state changes should ease, not snap. */
STATIC CONST lv_style_prop_t  kDlgTransProps[] = {
  LV_STYLE_BG_COLOR, LV_STYLE_SHADOW_WIDTH, 0
};
STATIC lv_style_transition_dsc_t  sDlgTrans;
STATIC BOOLEAN                    sDlgTransReady = FALSE;

STATIC
VOID
EnsureDlgTransition (
  VOID
  )
{
  if (!sDlgTransReady) {
    lv_style_transition_dsc_init (&sDlgTrans, kDlgTransProps,
                                  lv_anim_path_ease_out, 150, 0, NULL);
    sDlgTransReady = TRUE;
  }
}

/** The dialog button. `Icon` may be NULL; when present it is drawn with the
    symbol font at the left of the text (the CJK font has no symbols). */
STATIC
lv_obj_t *
ModalButtonEx (
  IN lv_obj_t     *Parent,
  IN CONST CHAR8  *Icon,
  IN CONST CHAR8  *Utf8,
  IN UINT32        Bg,
  IN INT32         X,
  IN INT32         Y,
  IN INT32         W,
  IN lv_event_cb_t Cb,
  IN UINT32        UserData
  )
{
  lv_obj_t *B = lv_button_create (Parent);
  lv_obj_t *L;

  UiGroupDetach (B);
  lv_obj_remove_style_all (B);
  EnsureDlgTransition ();
  lv_obj_set_size (B, W, 34);
  lv_obj_set_pos (B, X, Y);
  lv_obj_set_style_bg_color (B, lv_color_hex (Bg), 0);
  lv_obj_set_style_bg_grad_color (B, lv_color_hex (Bg), 0);
  lv_obj_set_style_bg_grad_dir (B, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_bg_opa (B, LV_OPA_COVER, 0);
  lv_obj_set_style_radius (B, 6, 0);
  lv_obj_set_style_shadow_width (B, 12, 0);
  lv_obj_set_style_shadow_opa (B, LV_OPA_40, 0);
  lv_obj_set_style_shadow_color (B, lv_color_hex (0x000000), 0);
  lv_obj_set_style_shadow_offset_y (B, 2, 0);
  lv_obj_set_style_transition (B, &sDlgTrans, 0);
  lv_obj_set_style_pad_column (B, 6, 0);
  lv_obj_set_flex_flow (B, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align (B, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                         LV_FLEX_ALIGN_CENTER);

  lv_obj_set_style_bg_color (B, lv_color_hex (0x4E5154), LV_STATE_HOVERED);
  lv_obj_set_style_shadow_width (B, 18, LV_STATE_HOVERED);
  lv_obj_set_style_bg_color (B, lv_color_hex (0x33383C), LV_STATE_PRESSED);
  lv_obj_set_style_shadow_width (B, 3, LV_STATE_PRESSED);
  lv_obj_set_style_shadow_offset_y (B, 0, LV_STATE_PRESSED);
  lv_obj_set_style_translate_y (B, 1, LV_STATE_PRESSED);
  /* The focus ring has to be unmistakable: it is the only indication of where
     Enter will land, and the dialog background is dark. */
  lv_obj_set_style_bg_color (B, lv_color_hex (0x5A5E62), LV_STATE_FOCUSED);
  lv_obj_set_style_border_color (B, lv_color_hex (0x7FC4FF), LV_STATE_FOCUSED);
  lv_obj_set_style_border_width (B, 2, LV_STATE_FOCUSED);
  lv_obj_set_style_border_opa (B, LV_OPA_COVER, LV_STATE_FOCUSED);

  if ((Icon != NULL) && (Icon[0] != 0)) {
    L = lv_label_create (B);
    lv_label_set_text (L, Icon);
    IconFont (L);
    lv_obj_set_style_text_color (L, lv_color_hex (0xFFFFFF), 0);
  }

  L = lv_label_create (B);
  lv_label_set_text (L, Utf8);
  UiFont (L);
  lv_obj_set_style_text_color (L, lv_color_hex (0xFFFFFF), 0);

  lv_obj_add_flag (B, LV_OBJ_FLAG_EVENT_BUBBLE);
  if (Cb != NULL) {
    lv_obj_add_event_cb (B, Cb, LV_EVENT_CLICKED, (VOID *)(UINTN)UserData);
  }
  FocusAdd (B, Utf8);
  return B;
}

STATIC
lv_obj_t *
ModalButton (
  IN lv_obj_t     *Parent,
  IN CONST CHAR8  *Utf8,
  IN UINT32        Bg,
  IN INT32         X,
  IN INT32         Y,
  IN INT32         W,
  IN lv_event_cb_t Cb,
  IN UINT32        UserData
  )
{
  return ModalButtonEx (Parent, NULL, Utf8, Bg, X, Y, W, Cb, UserData);
}

/* --------------------------------------------------------------- notice */

STATIC VOID NoticeDismiss (IN OUT APP_CTX *A);

STATIC
VOID
NoticeDeleteDone (
  IN lv_obj_t  *Obj,
  IN VOID      *UserData
  )
{
  (VOID)UserData;
  if (lv_obj_is_valid (Obj)) {
    lv_obj_delete (Obj);
  }
}

STATIC
VOID
NoticeOkCb (
  IN lv_event_t  *e
  )
{
  (VOID)e;
  NoticeDismiss (gApp);
}

/** A popup inside the current dialog.

    Two reports came from the same root cause: a dialog that answers only by
    changing a small line of grey text is indistinguishable from a dialog that
    ignores you. 导出报告 did write the file (the serial log says so) and the
    typed gate did refuse the wrong word, but neither said anything the user
    could see. Anything the user explicitly asked for - and anything refused -
    now gets this. It is a child of the dialog card rather than a second
    overlay, so dismissing it leaves the dialog exactly as it was. */
STATIC
VOID
ShowNotice (
  IN OUT APP_CTX     *A,
  IN     CONST CHAR8 *Title,
  IN     CONST CHAR8 *Line1,
  IN     CONST CHAR8 *Line2,
  IN     UINT32       Color,
  IN     BOOLEAN      KeepTyping
  )
{
  lv_obj_t *Panel;
  lv_obj_t *L;

  (VOID)A;
  if (sCard == NULL) {
    return;
  }
  if (sNoticePanel != NULL) {
    /* Replace. The saved ring still belongs to the dialog underneath, so it
       must not be overwritten. */
    lv_obj_delete (sNoticePanel);
    sNoticePanel = NULL;
  } else {
    RingSaveNow ();
  }

  Panel = lv_obj_create (sCard);
  lv_obj_remove_style_all (Panel);
  lv_obj_set_size (Panel, 520, 150);
  lv_obj_align (Panel, LV_ALIGN_CENTER, 0, -20);
  lv_obj_set_style_bg_color (Panel, lv_color_hex (0x212124), 0);
  lv_obj_set_style_bg_opa (Panel, LV_OPA_COVER, 0);
  lv_obj_set_style_radius (Panel, 8, 0);
  lv_obj_set_style_border_width (Panel, 2, 0);
  lv_obj_set_style_border_color (Panel, lv_color_hex (Color), 0);
  lv_obj_set_style_pad_all (Panel, 14, 0);
  lv_obj_set_style_shadow_width (Panel, 24, 0);
  lv_obj_set_style_shadow_opa (Panel, LV_OPA_60, 0);
  lv_obj_set_style_shadow_color (Panel, lv_color_hex (0x000000), 0);
  lv_obj_clear_flag (Panel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag (Panel, LV_OBJ_FLAG_EVENT_BUBBLE);

  L = lv_label_create (Panel);
  lv_label_set_text (L, (Color == 0x00CC66) ? LV_SYMBOL_OK : LV_SYMBOL_WARNING);
  IconFont (L);
  lv_obj_set_style_text_color (L, lv_color_hex (Color), 0);
  lv_obj_set_pos (L, 0, 2);

  L = lv_label_create (Panel);
  lv_label_set_text (L, Title);
  UiFont (L);
  lv_obj_set_style_text_color (L, lv_color_hex (Color), 0);
  lv_obj_set_pos (L, 28, 0);

  L = lv_label_create (Panel);
  lv_label_set_text (L, Line1);
  UiFont (L);
  lv_obj_set_style_text_color (L, lv_color_hex (0xE0E0E0), 0);
  lv_obj_set_pos (L, 28, 32);

  L = lv_label_create (Panel);
  lv_label_set_text (L, Line2);
  UiFont (L);
  lv_obj_set_style_text_color (L, lv_color_hex (0x9A9A9A), 0);
  lv_obj_set_pos (L, 28, 58);

  /* Borrow the ring: while the notice is up, Enter is its button. */
  sFocusCount = 0;
  sFocusIdx   = 0;
  ModalButtonEx (Panel, LV_SYMBOL_OK, "知道了", Color, 366, 88, 112,
                 NoticeOkCb, 0);
  ModalFocusApply (0);

  sNoticePanel       = Panel;
  sNoticeKeepsTyping = KeepTyping;
  (VOID)LvglTransShow (Panel, LV_TRANS_ALERT_POP);
  DEBUG ((DEBUG_INFO, "[Ui] notice '%a' color=0x%x\n", Title, Color));
}

STATIC
VOID
NoticeDismiss (
  IN OUT APP_CTX  *A
  )
{
  lv_obj_t *Panel = sNoticePanel;

  (VOID)A;
  if (Panel == NULL) {
    return;
  }
  sNoticePanel       = NULL;
  sNoticeKeepsTyping = FALSE;
  RingRestoreNow ();
  if (!LvglTransIsEnabled () ||
      (LvglTransHide (Panel, LV_TRANS_ALERT_POP, NoticeDeleteDone, NULL)
       != LV_RESULT_OK)) {
    lv_obj_delete (Panel);
  }
  DEBUG ((DEBUG_INFO, "[Ui] notice dismissed\n"));
}

/** Live verdict on the typed gate's input. */
STATIC
VOID
UpdateTypedHint (
  VOID
  )
{
  if (sHintLabel == NULL) {
    return;
  }
  if (AsciiStrCmp (sTyped, sExpected) == 0) {
    lv_label_set_text (sHintLabel, "输入匹配 —— 可以按「确认擦除」了。");
    lv_obj_set_style_text_color (sHintLabel, lv_color_hex (0x00CC66), 0);
  } else {
    lv_label_set_text (sHintLabel, "");
  }
}

BOOLEAN
DialogsIsOpen (
  VOID
  )
{
  return (BOOLEAN)(sState != DLG_NONE);
}

/* --------------------------------------------------------------- about */

STATIC
VOID
AboutCloseCb (
  IN lv_event_t  *e
  )
{
  CloseModal (gApp);
}

VOID
DialogsAbout (
  IN OUT APP_CTX  *A
  )
{
  lv_obj_t *Card = ModalCreate (A, 520, 240);
  CHAR8     Ver[96];

  sState = DLG_ABOUT;
  AsciiSPrint (Ver, sizeof (Ver), "版本 %a", UEFIERASER_VERSION_STR);

  ModalLabel (Card, "UefiEraser", 0xFFFFFF, 0, 0);
  ModalLabel (Card, "UEFI Shell 磁盘数据彻底粉碎工具", 0xCCCCCC, 0, 26);
  ModalLabel (Card, Ver, 0x9A9A9A, 0, 52);
  ModalLabel (Card, "Author : Mike Wu", 0x9A9A9A, 0, 78);
  ModalLabel (Card, "算法按各标准的公开规范实现（DoD / Gutmann / HMG IS5 /", 0x9A9A9A, 0, 112);
  ModalLabel (Card, "RCMP / VSITR / GOST / AR380-19 / USAF 5020 等）。", 0x9A9A9A, 0, 136);
  ModalLabel (Card, "覆写对 SSD 不可靠，固态盘请使用设备级擦除。", 0xE0A030, 0, 166);

  sDefaultBtn = ModalButtonEx (Card, LV_SYMBOL_CLOSE, "关闭", 0x2D7D9A, 420, 180, 80,
                               AboutCloseCb, 0);

  /* Enter with a transition; mTransEff is set here so the exit mirrors it. */
  mTransEff = LV_TRANS_ALERT_POP;
  (VOID)LvglTransShow (Card, mTransEff);
  ModalFocusDefault ();
}

/* ---------------------------------------------------- algorithm picker */

STATIC
VOID
AlgoPickCb (
  IN lv_event_t  *e
  )
{
  UINT32 Index = (UINT32)(UINTN)lv_event_get_user_data (e);

  gApp->Mode      = ERASE_MODE_OVERWRITE;
  gApp->AlgoIndex = Index;
  CloseModal (gApp);
  MainWindowRefreshAlgo (gApp);
  MainWindowRefreshList (gApp);
}

STATIC
VOID
DeviceModePickCb (
  IN lv_event_t  *e
  )
{
  ERASE_MODE Mode = (ERASE_MODE)(UINTN)lv_event_get_user_data (e);

  gApp->Mode = Mode;
  CloseModal (gApp);
  MainWindowRefreshAlgo (gApp);
  MainWindowRefreshList (gApp);
}

STATIC
VOID
AlgoToggleVerifyCb (
  IN lv_event_t  *e
  )
{
  gApp->Verify = (BOOLEAN)!gApp->Verify;
  MainWindowRefreshAlgo (gApp);
  DialogsPickAlgorithm (gApp);   /* rebuild so the label reflects the toggle */
}

VOID
DialogsPickAlgorithm (
  IN OUT APP_CTX  *A
  )
{
  lv_obj_t        *Card;
  lv_obj_t        *List;
  UINT32           I;
  UINT32           N = EraseAlgoCount ();
  CHAR8            Header[128];
  CONST ERASE_ALGORITHM *Cur = AppCurrentAlgo (A);

  Card = ModalCreate (A, 760, 620);
  sState = DLG_ALGO;

  if (A->Mode == ERASE_MODE_OVERWRITE) {
    AsciiSPrint (Header, sizeof (Header),
                 "选择擦除算法（当前: %a, %d 遍）", Cur->NameEn,
                 (UINT32)AppCurrentPassCount (A));
  } else {
    AsciiSPrint (Header, sizeof (Header),
                 "选择擦除模式（当前: %a）", AppModeName (A));
  }
  ModalLabel (Card, Header, 0xFFFFFF, 0, 0);

  List = lv_obj_create (Card);
  lv_obj_remove_style_all (List);
  lv_obj_set_pos (List, 0, 30);
  lv_obj_set_size (List, 732, 360);
  lv_obj_set_style_bg_color (List, lv_color_hex (0x252526), 0);
  lv_obj_set_style_bg_opa (List, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all (List, 4, 0);
  lv_obj_set_style_pad_row (List, 2, 0);
  lv_obj_set_flex_flow (List, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scrollbar_mode (List, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_add_flag (List, LV_OBJ_FLAG_EVENT_BUBBLE);

  for (I = 0; I < N; I++) {
    CONST ERASE_ALGORITHM  *Algo = EraseAlgoAt (I);
    lv_obj_t               *Row = lv_button_create (List);

    UiGroupDetach (Row);
    CHAR8                   Text[320];
    char                    Passes[256];
    CHAR8                   NameUtf8[192];

    AsciiStrCpyS (NameUtf8, sizeof (NameUtf8), Algo->NameZh);
    EraseAlgoDescribePasses (Algo, Passes, sizeof (Passes));

    if (Algo->PassCount == 0) {
      AsciiSPrint (Text, sizeof (Text), "%a    (N 遍, 随机)", NameUtf8);
    } else {
      AsciiSPrint (Text, sizeof (Text), "%a    (%d 遍)   %a",
                   NameUtf8, (UINT32)Algo->PassCount, Passes);
    }

    lv_obj_remove_style_all (Row);
    lv_obj_set_width (Row, LV_PCT (100));
    lv_obj_set_height (Row, 30);
    lv_obj_set_style_bg_color (
      Row,
      lv_color_hex ((A->Mode == ERASE_MODE_OVERWRITE && I == A->AlgoIndex)
                      ? 0x0E4A6B : 0x2A2A2C),
      0
      );
    lv_obj_set_style_bg_opa (Row, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color (Row, lv_color_hex (0x3A3D41), LV_STATE_FOCUSED);
    lv_obj_set_style_radius (Row, 3, 0);
    lv_obj_set_style_pad_hor (Row, 8, 0);
    lv_obj_clear_flag (Row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag (Row, LV_OBJ_FLAG_EVENT_BUBBLE);
    {
      lv_obj_t *L = lv_label_create (Row);
      lv_label_set_text (L, Text);
      UiFont (L);
      lv_obj_set_style_text_color (L, lv_color_hex (0xE0E0E0), 0);
      lv_obj_align (L, LV_ALIGN_LEFT_MID, 0, 0);
    }
    lv_obj_add_event_cb (Row, AlgoPickCb, LV_EVENT_CLICKED, (VOID *)(UINTN)I);
    FocusAdd (Row, (I < EraseAlgoCount ()) ? EraseAlgoAt (I)->Id : "device");
  }

  /* ---- device-level section ---- */
  {
    lv_obj_t *Sep = lv_label_create (Card);
    lv_label_set_text (Sep, "─── 设备级擦除（盘固件执行，需盘支持）───");
    UiFont (Sep);
    lv_obj_set_style_text_color (Sep, lv_color_hex (0x9A9A9A), 0);
    lv_obj_set_pos (Sep, 0, 400);
  }

  {
    lv_obj_t *B;
    B = ModalButtonEx (Card, LV_SYMBOL_DRIVE, "ATA Secure Erase",
                     (A->Mode == ERASE_MODE_ATA) ? 0x0E4A6B : 0x3A3D41,
                     0, 426, 230, DeviceModePickCb, ERASE_MODE_ATA);
    B = ModalButtonEx (Card, LV_SYMBOL_DRIVE, "NVMe Format NVM",
                     (A->Mode == ERASE_MODE_NVME_FORMAT) ? 0x0E4A6B : 0x3A3D41,
                     240, 426, 230, DeviceModePickCb, ERASE_MODE_NVME_FORMAT);
    B = ModalButtonEx (Card, LV_SYMBOL_TRASH, "NVMe Sanitize",
                     (A->Mode == ERASE_MODE_NVME_SANITIZE) ? 0x0E4A6B : 0x3A3D41,
                     480, 426, 230, DeviceModePickCb, ERASE_MODE_NVME_SANITIZE);
    (void)B;
  }

  {
    CHAR8 V[64];
    AsciiSPrint (V, sizeof (V), "读回校验末遍: %a", A->Verify ? "开" : "关");
    ModalButton (Card, V, 0x3A3D41, 0, 472, 220, AlgoToggleVerifyCb, 0);
  }
  {
    CHAR8 V[64];
    AsciiSPrint (V, sizeof (V), "自定义遍数: %d  （按 + / - 调整）", A->CustomPasses);
    ModalLabel (Card, V, 0x9A9A9A, 0, 518);
  }
  /* Single selection is deliberate, and it is the norm: an algorithm is the
     pass sequence for one run, so two of them cannot both be "the" pattern on
     the same bytes. Say so here rather than letting it read as a limitation. */
  ModalLabel (Card,
              "单选：一次运行一种算法，多遍请用「自定义」。设备级三项要求盘实现对应命令"
              "（ATA 需 Security feature set；NVMe 需 Format NVM / Sanitize），失败时报告会写明原因。",
              0x9A9A9A, 0, 544);
  sDefaultBtn = ModalButtonEx (Card, LV_SYMBOL_CLOSE, "关闭", 0x2D7D9A, 640, 556, 90,
                               AboutCloseCb, 0);

  /* Enter with a transition; mTransEff is set here so the exit mirrors it. */
  mTransEff = LV_TRANS_SHEET_UP;
  (VOID)LvglTransShow (Card, mTransEff);
  ModalFocusDefault ();
}

/* ------------------------------------------------ confirmation chain */

STATIC
VOID
SummaryContinueCb (
  IN lv_event_t  *e
  );

STATIC
VOID
RunEraseCb (
  IN lv_event_t  *e
  );

/** Build the "type this to continue" gate. */
STATIC
VOID
ShowTypedGate (
  IN OUT APP_CTX  *A,
  IN     UINT32    Gate
  )
{
  lv_obj_t *Card;
  CHAR8     Line1[192];
  CHAR8     Line2[192];

  sGateIndex = Gate;
  Card = ModalCreate (A, 700, 300);
  sState = DLG_TYPED;
  sTyped[0] = 0;

  if (Gate == 0) {
    AsciiStrCpyS (sExpected, sizeof (sExpected), "ERASE");
    AsciiSPrint (Line1, sizeof (Line1), "请输入 ERASE（全大写）以确认本次擦除：");
    AsciiSPrint (Line2, sizeof (Line2), "已选 %d 个目标，即将永久销毁其全部数据。",
                 (UINT32)AppSelectedCount (A));
  } else {
    UINT64       Bytes = 0;
    UINT64       Value;
    CONST CHAR8 *Unit;
    UINT32       I;
    for (I = 0; I < A->TargetCount; I++) {
      if (A->Targets[I].Selected && A->Targets[I].Kind == TARGET_KIND_DISK) {
        Bytes += A->Targets[I].Length;
      }
    }
    /* Whole GiB is the right unit for the disks this gate exists for, but
       integer GiB truncates anything under 1 GiB to "0" - and a gate whose
       answer is always 0 is not a gate. Below a GiB the same question is asked
       in MiB instead, which is exactly how a 512 MiB test disk should read. */
    if (Bytes < (1024ULL * 1024 * 1024)) {
      Value = Bytes / (1024ULL * 1024);
      Unit  = "MiB";
    } else {
      Value = Bytes / (1024ULL * 1024 * 1024);
      Unit  = "GiB";
    }
    AsciiSPrint (sExpected, sizeof (sExpected), "%ld", Value);
    AsciiSPrint (Line1, sizeof (Line1),
                 "选中的整盘容量合计 %ld %a —— 请输入该数字以确认：", Value, Unit);
    AsciiSPrint (Line2, sizeof (Line2),
                 "此操作将摧毁整块物理磁盘，包括分区表与所有分区。");
  }

  DEBUG ((DEBUG_INFO, "[Ui] typed gate %u expect='%a'\n",
          (UINT32)Gate, sExpected));
  ModalLabel (Card, "危险操作确认", 0xE05252, 0, 0);
  ModalLabel (Card, Line1, 0xFFFFFF, 0, 32);
  ModalLabel (Card, Line2, 0xE0A030, 0, 58);

  sTypedLabel = ModalLabel (Card, "", 0x00FF88, 0, 104);
  lv_obj_set_style_bg_color (sTypedLabel, lv_color_hex (0x1A1A1A), 0);
  lv_obj_set_style_bg_opa (sTypedLabel, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all (sTypedLabel, 6, 0);
  lv_obj_set_width (sTypedLabel, 640);
  lv_label_set_text (sTypedLabel, "");

  /* The verdict on what has been typed so far. Pressing 确认 with the wrong
     text used to do nothing at all - no message, no colour change. */
  sHintLabel = ModalLabel (Card, "", 0x9A9A9A, 0, 148);

  sConfirmBtn = ModalButtonEx (Card, LV_SYMBOL_WARNING, "确认擦除", 0xC0392B,
                               410, 180, 140, RunEraseCb, 0);
  sDefaultBtn = sConfirmBtn;
  ModalButtonEx (Card, LV_SYMBOL_CLOSE, "取消", 0x3A3D41, 560, 180, 110,
                 AboutCloseCb, 0);

  /* Enter with a transition; mTransEff is set here so the exit mirrors it. */
  mTransEff = LV_TRANS_ALERT_POP;
  (VOID)LvglTransShow (Card, mTransEff);
  ModalFocusDefault ();
}

STATIC
VOID
SummaryContinueCb (
  IN lv_event_t  *e
  )
{
  /* Gate 3 passed: move to gate 4. A whole-disk selection adds a second typed
     gate so the user has to acknowledge the capacity, not just the word. */
  ShowTypedGate (gApp, 0);
}

VOID
DialogsConfirmAndRun (
  IN OUT APP_CTX  *A
  )
{
  lv_obj_t        *Card;
  CONST ERASE_ALGORITHM *Algo = AppCurrentAlgo (A);
  UINT32           Sel = AppSelectedCount (A);
  UINT64           Bytes = AppSelectedBytes (A);
  CHAR16           SizeText[32];
  CHAR8            SizeUtf8[48];
  CHAR8            L1[192];
  CHAR8            L2[192];
  CHAR8            L3[192];
  CHAR8            L4[192];
  UINT32           I;
  UINT32           Disks = 0;
  UINT32           Parts = 0;

  if (Sel == 0) {
    Card = ModalCreate (A, 520, 160);
    sState = DLG_ABOUT;
    ModalLabel (Card, "尚未选择任何目标", 0xE0A030, 0, 0);
    ModalLabel (Card, "请先在列表中选择要粉碎的磁盘或分区。", 0xCCCCCC, 0, 30);
    ModalButton (Card, "关闭", 0x2D7D9A, 420, 90, 80, AboutCloseCb, 0);
    return;
  }

  for (I = 0; I < A->TargetCount; I++) {
    if (!A->Targets[I].Selected) {
      continue;
    }
    if (A->Targets[I].Kind == TARGET_KIND_DISK) {
      Disks++;
    } else {
      Parts++;
    }
  }

  /* Device-level modes only make sense on whole disks. */
  if (A->Mode != ERASE_MODE_OVERWRITE && Parts > 0) {
    Card = ModalCreate (A, 560, 160);
    sState = DLG_ABOUT;
    ModalLabel (Card, "设备级擦除只能用于整盘", 0xE0A030, 0, 0);
    ModalLabel (Card, "请取消选中分区，只保留整盘目标。", 0xCCCCCC, 0, 30);
    ModalButton (Card, "关闭", 0x2D7D9A, 420, 90, 80, AboutCloseCb, 0);
    return;
  }

  Card = ModalCreate (A, 760, 380);
  sState = DLG_SUMMARY;

  BlockDevFormatSize (Bytes, SizeText, ARRAY_SIZE (SizeText));
  ToUtf8 (SizeText, SizeUtf8, sizeof (SizeUtf8));

  AsciiSPrint (L1, sizeof (L1), "即将执行擦除，请核对以下信息：");
  AsciiSPrint (L2, sizeof (L2), "目标数量: %d 个（其中整盘 %d 个）",
               (UINT32)Sel, (UINT32)Disks);

  if (A->Mode == ERASE_MODE_OVERWRITE) {
    AsciiSPrint (L3, sizeof (L3), "待写入总量: %a  （目标容量 x %d 遍）",
                 SizeUtf8, (UINT32)AppCurrentPassCount (A));
    AsciiSPrint (L4, sizeof (L4), "算法: %a    读回校验: %a",
                 Algo->NameEn, A->Verify ? "开" : "关");
  } else {
    AsciiSPrint (L3, sizeof (L3), "模式: %a", AppModeName (A));
    AsciiSPrint (L4, sizeof (L4), "由设备固件执行，不可中断。请确保电源稳定。");
  }

  DEBUG ((DEBUG_INFO, "[Ui] summary: selected=%u disks=%u mode=%d\n",
          (UINT32)Sel, (UINT32)Disks, (UINT32)A->Mode));
  ModalLabel (Card, "确认擦除目标", 0xFFFFFF, 0, 0);
  ModalLabel (Card, L1, 0xCCCCCC, 0, 32);
  ModalLabel (Card, L2, 0xFFFFFF, 0, 62);
  ModalLabel (Card, L3, 0xFFFFFF, 0, 88);
  ModalLabel (Card, L4, 0xFFFFFF, 0, 114);

  ModalLabel (Card, "擦除后数据不可恢复。请确认以上设备就是你要销毁的设备。",
              0xE0A030, 0, 152);

  /* List the selected targets by name so the user reads the actual devices. */
  {
    lv_obj_t *Box = lv_obj_create (Card);
    lv_obj_remove_style_all (Box);
    lv_obj_set_pos (Box, 0, 184);
    lv_obj_set_size (Box, 732, 110);
    lv_obj_set_style_bg_color (Box, lv_color_hex (0x1A1A1A), 0);
    lv_obj_set_style_bg_opa (Box, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all (Box, 6, 0);
    lv_obj_set_scrollbar_mode (Box, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag (Box, LV_OBJ_FLAG_EVENT_BUBBLE);

    {
      UINT32 Shown = 0;
      CHAR8  Buf[1024];
      Buf[0] = 0;
      for (I = 0; I < A->TargetCount && Shown < 6; I++) {
        CHAR16  Line[192];
        CHAR8   LineUtf8[384];
        CHAR16  Sz[32];
        if (!A->Targets[I].Selected) {
          continue;
        }
        BlockDevFormatSize (A->Targets[I].Length, Sz, ARRAY_SIZE (Sz));
        UnicodeSPrint (Line, sizeof (Line), L"%s  %s  %s\n",
                       A->Targets[I].Name, Sz, A->Targets[I].Detail);
        ToUtf8 (Line, LineUtf8, sizeof (LineUtf8));
        AsciiStrCatS (Buf, sizeof (Buf), LineUtf8);
        Shown++;
      }
      if (Sel > 6) {
        CHAR8 More[64];
        AsciiSPrint (More, sizeof (More), "... 另有 %d 个目标\n", (UINT32)(Sel - 6));
        AsciiStrCatS (Buf, sizeof (Buf), More);
      }
      {
        lv_obj_t *L = lv_label_create (Box);
        lv_label_set_text (L, Buf);
        UiFont (L);
        lv_obj_set_style_text_color (L, lv_color_hex (0xCCCCCC), 0);
        lv_obj_align (L, LV_ALIGN_TOP_LEFT, 0, 0);
      }
    }
  }

  sDefaultBtn = ModalButtonEx (Card, LV_SYMBOL_RIGHT, "继续", 0xC0392B, 480, 312,
                               120, SummaryContinueCb, 0);
  ModalButtonEx (Card, LV_SYMBOL_CLOSE, "取消", 0x3A3D41, 620, 312, 110,
                 AboutCloseCb, 0);

  /* Enter with a transition; mTransEff is set here so the exit mirrors it. */
  mTransEff = LV_TRANS_ALERT_POP;
  (VOID)LvglTransShow (Card, mTransEff);
  ModalFocusDefault ();
}

/* ------------------------------------------------------------- progress */

STATIC
VOID
CancelEraseCb (
  IN lv_event_t  *e
  )
{
  /* The running session polls this flag from inside the erasure loop; the
     dialog itself stays up until the loop notices. */
  gApp->Session.CancelRequested = TRUE;
  DEBUG ((DEBUG_INFO, "[Ui] cancel requested by user\n"));
}

STATIC
VOID
ProgressCardCreate (
  IN OUT APP_CTX  *A
  )
{
  lv_obj_t *Card = ModalCreate (A, 760, 260);

  sState = DLG_PROGRESS;
  sProgLogPct = 0xFFFFFFFFu;
  DEBUG ((DEBUG_INFO, "[Ui] progress dialog: targets=%u\n", (UINT32)sRunTotal));
  /* The progress dialog invalidates the whole screen on every sampling window.
     With the glass layer on, each of those is a full-screen blur stretch plus a
     shadow; switching it off makes the card opaque, so LVGL starts drawing at
     the card and the wallpaper behind is never touched while data is moving. */
  GlassChromeDecor (FALSE);
  ModalLabel (Card, "正在擦除 —— 请勿断电", 0xE05252, 0, 0);
  ModalLabel (Card, "擦除过程中可以取消，但已覆写的部分无法还原。",
              0x9A9A9A, 0, 30);

  sProgressLabel = ModalLabel (Card, "准备中…", 0xFFFFFF, 0, 70);

  sProgressBar = lv_bar_create (Card);
  lv_obj_set_size (sProgressBar, 720, 22);
  lv_obj_set_pos (sProgressBar, 0, 104);
  lv_bar_set_range (sProgressBar, 0, 100);
  lv_bar_set_value (sProgressBar, 0, LV_ANIM_OFF);
  lv_obj_set_style_bg_color (sProgressBar, lv_color_hex (0x1A1A1A), 0);
  lv_obj_set_style_bg_opa (sProgressBar, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color (sProgressBar, lv_color_hex (0xC0392B),
                             LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa (sProgressBar, LV_OPA_COVER, LV_PART_INDICATOR);

  ModalButtonEx (Card, LV_SYMBOL_CLOSE, "取消", 0x3A3D41, 610, 150, 110,
                 CancelEraseCb, 0);

  /* Enter with a transition; mTransEff is set here so the exit mirrors it. */
  mTransEff = LV_TRANS_ALERT_POP;
  (VOID)LvglTransShow (Card, mTransEff);
  ModalFocusDefault ();
}

VOID
DialogsUpdateProgress (
  IN OUT APP_CTX  *A
  )
{
  CHAR8  Text[320];
  CHAR16 Rate[32];
  CHAR8  RateUtf8[48];

  if (sState != DLG_PROGRESS || sProgressBar == NULL) {
    return;
  }

  if (A->Mode == ERASE_MODE_OVERWRITE) {
    lv_bar_set_value (sProgressBar, (INT32)A->Session.Progress.Percent, LV_ANIM_OFF);

    if (A->Session.Progress.BytesPerSec > 0) {
      BlockDevFormatSize (A->Session.Progress.BytesPerSec, Rate, ARRAY_SIZE (Rate));
      ToUtf8 (Rate, RateUtf8, sizeof (RateUtf8));
    } else {
      AsciiStrCpyS (RateUtf8, sizeof (RateUtf8), "--");
    }

    AsciiSPrint (
      Text,
      sizeof (Text),
      "目标 %d/%d    第 %d/%d 遍    %d%%    %a/s    剩余约 %d 秒",
      (UINT32)(sRunIndex + 1),
      (UINT32)sRunTotal,
      (UINT32)A->Session.Progress.Pass,
      (UINT32)A->Session.Progress.PassCount,
      (UINT32)A->Session.Progress.Percent,
      RateUtf8,
      (UINT32)A->Session.Progress.EtaSec
      );
  } else {
    /* Device-level: the drive firmware works in the background. No per-pass
       progress from the host's point of view, but the pump still fires so
       the UI stays responsive and the Cancel button works. */
    lv_bar_set_value (sProgressBar, 0, LV_ANIM_OFF);
    AsciiSPrint (
      Text,
      sizeof (Text),
      "目标 %d/%d    %a 进行中…    请勿断电",
      (UINT32)(sRunIndex + 1),
      (UINT32)sRunTotal,
      AppModeName (A)
      );
  }
  lv_label_set_text (sProgressLabel, Text);

  /* One line per 10% (and one for the first sample), so a log alone proves the
     progress moved while the erasure was in flight. */
  if (A->Mode == ERASE_MODE_OVERWRITE) {
    UINT32  Pct = A->Session.Progress.Percent;

    if ((sProgLogPct == 0xFFFFFFFFu) || ((Pct / 10) != (sProgLogPct / 10))) {
      DEBUG ((
        DEBUG_INFO,
        "[Ui] progress %u%% pass=%u/%u\n",
        Pct,
        (UINT32)A->Session.Progress.Pass,
        (UINT32)A->Session.Progress.PassCount
        ));
      sProgLogPct = Pct;
    }
  }
}

/* --------------------------------------------------------------- report */

STATIC
VOID
ReportExportCb (
  IN lv_event_t  *e
  );

STATIC
VOID
ReportCloseCb (
  IN lv_event_t  *e
  )
{
  CloseModal (gApp);
  MainWindowRefreshList (gApp);
}

/** Fill the UI-level summary for the file export. The target list is rendered
    as one CHAR16 string with '\n' separators, which is what
    ERASE_REPORT_SUMMARY takes. */
STATIC
VOID
BuildReportSummary (
  IN OUT APP_CTX  *A
  )
{
  UINT32  I;

  sReportTargets[0] = 0;
  for (I = 0; I < A->TargetCount; I++) {
    if (!A->Targets[I].Selected) {
      continue;
    }
    if ((StrLen (sReportTargets) + StrLen (A->Targets[I].Name) + 2)
        >= ARRAY_SIZE (sReportTargets)) {
      break;
    }
    if (sReportTargets[0] != 0) {
      StrCatS (sReportTargets, ARRAY_SIZE (sReportTargets), L"\n");
    }
    StrCatS (sReportTargets, ARRAY_SIZE (sReportTargets), A->Targets[I].Name);
  }

  sReportSummary.ModeName     = ModeNameForLog (A->Mode);
  sReportSummary.AlgoId       = (A->Mode == ERASE_MODE_OVERWRITE)
                                  ? AppCurrentAlgo (A)->Id : "-";
  sReportSummary.Result       = ResultFromStatus (sAggStatus, sAggCancelled);
  sReportSummary.Verify       = A->Verify;
  sReportSummary.Targets      = sAggTargets;
  sReportSummary.Failed       = sAggFailed;
  sReportSummary.BytesWritten = sAggBytes;
  sReportSummary.Passes       = sAggPasses;
  sReportSummary.Mismatches   = sAggMismatches;
  sReportSummary.TargetsText  = sReportTargets;
}

STATIC
VOID
ReportExportCb (
  IN lv_event_t  *e
  )
{
  BOOLEAN  Ok;

  Ok = LogPersistWriteReport (&sReportSummary);
  if (sExportLabel != NULL) {
    lv_label_set_text (
      sExportLabel,
      Ok ? "已导出到启动卷 UefiEraser-report.txt"
         : "导出失败：启动卷不可写"
      );
  }
  DEBUG ((DEBUG_INFO, "[Ui] report export %a\n", Ok ? "ok" : "failed"));

  /* The file was written all along - what was missing was any sign of it. */
  if (Ok) {
    ShowNotice (gApp, "导出成功",
                "已写入启动卷：UefiEraser-report.txt",
                "文件为 UTF-8 文本，可在任意系统中打开查看。",
                0x00CC66, FALSE);
  } else {
    ShowNotice (gApp, "导出失败",
                "启动卷不可写（写保护或只读状态）。",
                "请检查启动盘的写保护开关后重试。",
                0xE05252, FALSE);
  }
}

STATIC
VOID
ShowReport (
  IN OUT APP_CTX  *A
  )
{
  lv_obj_t *Card = ModalCreate (A, 760, 360);
  CHAR8     L1[192];
  CHAR8     L2[192];
  CHAR8     L3[192];
  CHAR8     L4[192];
  CHAR16    SizeText[32];
  CHAR8     SizeUtf8[48];
  UINT32    Color;

  sState = DLG_REPORT;
  DEBUG ((DEBUG_INFO, "[Ui] report: result=%a targets=%u failed=%u bytes=%lu\n",
          ResultFromStatus (sAggStatus, sAggCancelled), (UINT32)sAggTargets,
          (UINT32)sAggFailed, (UINT64)sAggBytes));

  /* The erasure is over: give the glass back. */
  GlassChromeDecor (TRUE);

  BlockDevFormatSize (sAggBytes, SizeText, ARRAY_SIZE (SizeText));
  ToUtf8 (SizeText, SizeUtf8, sizeof (SizeUtf8));

  if (sAggCancelled) {
    Color = 0xE0A030;
    AsciiStrCpyS (L1, sizeof (L1), "已取消 —— 目标可能未完全清除");
  } else if (sAggStatus == ERASE_OK) {
    Color = 0x00CC66;
    AsciiStrCpyS (L1, sizeof (L1), "擦除完成");
  } else if (sAggStatus == ERASE_ERR_VERIFY) {
    Color = 0xE0A030;
    AsciiStrCpyS (L1, sizeof (L1), "擦除完成，但末遍读回校验未通过");
  } else {
    Color = 0xE05252;
    AsciiSPrint (L1, sizeof (L1), "擦除失败: %a", EraseStatusName (sAggStatus));
  }

  AsciiSPrint (L2, sizeof (L2), "目标: %d 个（失败 %d 个）",
               (UINT32)sAggTargets, (UINT32)sAggFailed);

  if (A->Mode == ERASE_MODE_OVERWRITE) {
    AsciiSPrint (L3, sizeof (L3), "已写入: %a / 计划 %ld 字节", SizeUtf8, sAggPlanned);
    AsciiSPrint (L4, sizeof (L4), "完成遍数: %d    校验不一致字节: %d",
                 (UINT32)sAggPasses, (UINT32)sAggMismatches);
  } else {
    AsciiSPrint (L3, sizeof (L3), "设备级擦除由固件执行，无主机写入量统计。");
    L4[0] = 0;
  }

  ModalLabel (Card, "擦除结果", 0xFFFFFF, 0, 0);
  ModalLabel (Card, L1, Color, 0, 34);
  ModalLabel (Card, L2, 0xCCCCCC, 0, 74);
  ModalLabel (Card, L3, 0xCCCCCC, 0, 100);
  if (L4[0] != 0) {
    ModalLabel (Card, L4, 0xCCCCCC, 0, 126);
  }
  if (sAggReason != NULL) {
    /* "I/O error" alone names the symptom, not the cause, and for the
       device-level modes the cause is the whole message: an ATA disk whose
       firmware has no Security feature set and one that is FROZEN fail the
       same way and are fixed in completely different ways. */
    ModalLabel (Card, sAggReason, 0xE0A030, 0, 150);
  }

  if (A->Mode == ERASE_MODE_OVERWRITE) {
    ModalLabel (Card,
                "提示：多遍覆写对机械盘有效；SSD 因磨损均衡需使用设备级擦除。",
                0x9A9A9A, 0, 172);
    ModalLabel (Card,
                "覆写期间如有写入缓存或坏块重映射，个别扇区可能未被真正覆盖。",
                0x9A9A9A, 0, 196);
  } else {
    ModalLabel (Card,
                "设备级擦除由磁盘固件执行，覆盖包括预留区和退役块在内的全部物理区域。",
                0x9A9A9A, 0, 172);
  }

  BuildReportSummary (A);

  sExportLabel = ModalLabel (Card, "", 0x9A9A9A, 0, 224);

  ModalButtonEx (Card, LV_SYMBOL_SAVE, "导出报告", 0x2D7D9A, 460, 288, 150,
                 ReportExportCb, 0);
  sDefaultBtn = ModalButtonEx (Card, LV_SYMBOL_CLOSE, "关闭", 0x2D7D9A, 620, 288,
                               110, ReportCloseCb, 0);

  /* Enter with a transition; mTransEff is set here so the exit mirrors it. */
  mTransEff = LV_TRANS_ALERT_POP;
  (VOID)LvglTransShow (Card, mTransEff);
  ModalFocusDefault ();
}

/* --------------------------------------------------------------- runner */

/** Find the BLOCK_DEV backing a target. */
STATIC
BLOCK_DEV *
DevForTarget (
  IN OUT APP_CTX        *A,
  IN     ERASE_TARGET   *T
  )
{
  UINT32 I;

  for (I = 0; I < A->Devs.Count; I++) {
    if (A->Devs.Items[I].Handle == T->Handle) {
      return &A->Devs.Items[I];
    }
  }
  return NULL;
}

STATIC
VOID
RunEraseCb (
  IN lv_event_t  *e
  )
{
  APP_CTX  *A = gApp;
  UINT32   I;
  BOOLEAN  AnyDisk = FALSE;

  (VOID)e;

  /* Gate 4a: the literal word. */
  if (AsciiStrCmp (sTyped, sExpected) != 0) {
    CHAR8  Detail[192];
    CHAR8  Need[192];
    CHAR8  Step2[192];

    DEBUG ((DEBUG_WARN, "[Ui] typed '%a' != expected '%a'\n", sTyped, sExpected));

    if (sTyped[0] == 0) {
      AsciiStrCpyS (Detail, sizeof (Detail),
                     "输入框还是空的：还没有键入任何字符。");
    } else {
      AsciiSPrint (Detail, sizeof (Detail),
                   "当前输入 %a 与要求不符。", sTyped);
    }
    if (sGateIndex == 0) {
      AsciiStrCpyS (Need, sizeof (Need),
                     "请键入 ERASE（5 个大写英文字母）。");
    } else {
      AsciiSPrint (Need, sizeof (Need),
                   "请键入选中整盘的容量数字：%a", sExpected);
    }
    AsciiStrCpyS (Step2, sizeof (Step2),
                   "输入匹配后按「确认擦除」才会开始写盘。");

    if (sHintLabel != NULL) {
      lv_label_set_text (sHintLabel, Detail);
      lv_obj_set_style_text_color (sHintLabel, lv_color_hex (0xE05252), 0);
    }
    ShowNotice (A, (sTyped[0] == 0) ? "还没有输入" : "输入不正确",
                Need, Step2, 0xE05252, TRUE);
    DEBUG ((DEBUG_INFO, "[Ui] gate %u refused, notice shown\n",
            (UINT32)sGateIndex));
    return;
  }

  /* Gate 4b: a whole disk in the selection adds the capacity gate. */
  for (I = 0; I < A->TargetCount; I++) {
    if (A->Targets[I].Selected && A->Targets[I].Kind == TARGET_KIND_DISK) {
      AnyDisk = TRUE;
      break;
    }
  }
  if (AnyDisk && sGateIndex == 0) {
    ShowTypedGate (A, 1);
    return;
  }

  /* All gates passed. Show the progress card, then QUEUE the job.

     sRunTotal is set here rather than in DialogsRunErase so the dialog's own
     opening log line states the real target count (it read "targets=0" when the
     assignment happened after the card was built).

     This handler is an LVGL event callback, so it runs inside
     lv_timer_handler () - the indev read timer is what dispatches the click.
     A synchronous multi-second erasure here leaves LVGL's re-entrancy guard
     set for its entire duration, and every lv_timer_handler () the progress
     pump then calls returns immediately without running one timer. Timers are
     what drive animations AND the display refresh, so the progress dialog was
     never painted at all: measured 2026-09-17 on build 0.1.0.61, every pump
     logged next=1 ms and 12 screendumps taken 3 s apart over 38 s were byte
     identical, the screen still showing the confirmation gate.

     So the erasure runs where a long job belongs - from the main loop, outside
     the timer handler (see the EraseRequested comment in AppCtx.h). */
  sRunTotal = AppSelectedCount (A);
  ProgressCardCreate (A);
  A->EraseRequested = TRUE;
  DEBUG ((
    DEBUG_INFO,
    "[Ui] erase queued (served by the main loop, outside lv_timer_handler)\n"
    ));
}

VOID
DialogsRunErase (
  IN OUT APP_CTX  *A
  )
{
  UINT32                I;
  CONST ERASE_ALGORITHM *Algo = AppCurrentAlgo (A);
  UINT64                Started;

  sAggBytes        = 0;
  sAggPlanned      = 0;
  sAggPasses       = 0;
  sAggMismatches   = 0;
  sAggTargets      = 0;
  sAggFailed       = 0;
  sAggCancelled    = FALSE;
  sAggVerified     = TRUE;
  sAggStatus       = ERASE_OK;
  sAggReason       = NULL;
  sRunIndex        = 0;
  sRunTotal        = AppSelectedCount (A);

  Started = LvglTickGetMs ();

  DEBUG ((
    DEBUG_INFO,
    "[Ui] ERASE START algo=%a targets=%u verify=%d\n",
    Algo->Id,
    (UINT32)sRunTotal,
    A->Verify
    ));

  for (I = 0; I < A->TargetCount; I++) {
    ERASE_TARGET  *T = &A->Targets[I];
    BLOCK_DEV     *Dev;
    EFI_STATUS     Status;
    ERASE_REPORT   DevReport;

    if (!T->Selected) {
      continue;
    }

    Dev = DevForTarget (A, T);
    if (Dev == NULL) {
      DEBUG ((DEBUG_ERROR, "[Ui] no block device for target %u\n", (UINT32)I));
      sAggFailed++;
      continue;
    }

    /* The boot volume is refused outright at run time as well as being
       excluded from 全选: erasing the volume the app is running from would
       destroy the running image mid-write. */
    if (T->IsBootVolume) {
      DEBUG ((DEBUG_ERROR, "[Ui] refusing to erase the boot volume\n"));
      sAggFailed++;
      continue;
    }

    StrCpyS (A->LastTargetName, TARGET_NAME_MAX, T->Name);

    if (A->Mode == ERASE_MODE_OVERWRITE) {
      ZeroMem (&A->Session, sizeof (A->Session));
      Status = EraseSessionInit (
                 &A->Session,
                 Dev,
                 Algo,
                 A->CustomPasses,
                 A->Verify,
                 MainWindowPump,
                 A
                 );
      if (EFI_ERROR (Status)) {
        DEBUG ((DEBUG_ERROR, "[Ui] EraseSessionInit failed: %r\n", Status));
        sAggFailed++;
        continue;
      }

      A->SessionActive = TRUE;
      Status = EraseSessionRun (&A->Session);
      A->SessionActive = FALSE;

      sAggBytes      += A->Session.Report.BytesWritten;
      sAggPlanned    += A->Session.Report.TotalBytesPlanned;
      sAggPasses     += A->Session.Report.PassesCompleted;
      sAggMismatches += A->Session.Report.VerifyMismatches;
      sAggTargets++;
      if (A->Session.Report.Cancelled) {
        sAggCancelled = TRUE;
      }
      if (!A->Session.Report.Verified && A->Verify) {
        sAggVerified = FALSE;
      }
      if (EFI_ERROR (Status)) {
        sAggFailed++;
        sAggStatus = A->Session.Report.Status;
      }

      {
        CONST CHAR8 *ModeStr = ModeNameForLog (A->Mode);
        CONST CHAR8 *AlgoId  = (A->Mode == ERASE_MODE_OVERWRITE) ? Algo->Id : "-";
        UINT32       Passes  = (A->Mode == ERASE_MODE_OVERWRITE)
                                 ? ((Algo->PassCount != 0) ? Algo->PassCount
                                                           : ((A->CustomPasses != 0) ? A->CustomPasses : 1))
                                 : 0;
        LogPersistRecord (
          T->Name, ModeStr, AlgoId, Passes,
          ResultFromStatus (A->Session.Report.Status, A->Session.Report.Cancelled),
          A->Session.Report.BytesWritten
          );
      }
      if (A->Session.Report.Cancelled) {
        EraseSessionFree (&A->Session);
        break;
      }
      EraseSessionFree (&A->Session);
    } else {
      /* ---- device-level path ---- */
      ZeroMem (&DevReport, sizeof (DevReport));
      if (A->Mode == ERASE_MODE_ATA) {
        Status = AtaSecureErase (Dev->Handle, FALSE, MainWindowPump, A, &DevReport);
      } else if (A->Mode == ERASE_MODE_NVME_FORMAT) {
        Status = NvmeFormat (Dev->Handle, NVME_SES_USER_DATA_ERASE,
                             MainWindowPump, A, &DevReport);
      } else {
        Status = NvmeSanitize (Dev->Handle, NVME_SANACT_BLOCK_ERASE,
                               MainWindowPump, A, &DevReport);
      }
      sAggTargets++;
      if (EFI_ERROR (Status)) {
        sAggFailed++;
        sAggStatus = DevReport.Status;
        if (DevReport.Reason != NULL && sAggReason == NULL) {
          sAggReason = DevReport.Reason;
        }
      } else {
        sAggPasses += DevReport.PassesCompleted;
      }
      {
        CONST CHAR8 *ModeStr = ModeNameForLog (A->Mode);
        LogPersistRecord (
          T->Name, ModeStr, "-", 0,
          ResultFromStatus (DevReport.Status, DevReport.Cancelled),
          DevReport.BytesWritten
          );
      }
      if (DevReport.Cancelled) {
        sAggCancelled = TRUE;
        break;
      }
    }
    sRunIndex++;
  }

  /* The list behind the report is rebuilt from the media, unconditionally: the
     firmware's partition handles for whatever we just wrote were built from a
     table that may no longer exist, so nothing it remembers about partitions
     can be trusted. Re-reading LBA 0 of every disk and dropping the rows the
     media does not describe is what makes a disk whose partitions were erased
     stop showing any.

     (The other way to get there - disconnect/connect each written disk, i.e. a
     scoped `reconnect -r` - was implemented and did work under OVMF: the handle
     database really did drop the three dead partitions. It was dropped in
     favour of the media read anyway, because it mutates global firmware state
     with the disk's own driver Stop()/Start() on the critical path of an
     erasure tool, and because the list does not need it.) */
  {
    RESCAN_INFO  Found;

    AppRefreshTargets (A, &Found);
    DEBUG ((
      DEBUG_INFO,
      "[Ui] list rebuilt from the media: %u target(s) (was %u), %u stale "
      "partition row(s) dropped\n",
      Found.Targets,
      Found.WasTargets,
      Found.Ghosts
      ));
  }

  DEBUG ((
    DEBUG_INFO,
    "[Ui] ERASE END targets=%u failed=%u written=%lu status=%a cancelled=%d "
    "elapsed_ms=%lu\n",
    (UINT32)sAggTargets,
    (UINT32)sAggFailed,
    (UINT64)sAggBytes,
    EraseStatusName (sAggStatus),
    sAggCancelled,
    LvglTickGetMs () - Started
    ));

  ShowReport (A);
}

/* ------------------------------------------------------------ key input */

BOOLEAN
DialogsKey (
  IN OUT APP_CTX  *A,
  IN     UINT32    Key
  )
{
  if (sState == DLG_NONE) {
    return FALSE;
  }

  /* The progress dialog swallows everything: the erasure is synchronous and
     the only legitimate action is Cancel, which is a mouse/Enter button. */
  if (sState == DLG_PROGRESS) {
    return TRUE;
  }

  /* A notice is on top of whatever dialog is open and owns Enter/ESC. Over the
     typed gate it does not swallow typing: the user can fix the input and the
     notice gets out of the way. */
  if (sNoticePanel != NULL) {
    if ((Key == LV_KEY_ENTER) || (Key == LV_KEY_ESC)) {
      NoticeDismiss (A);
      return TRUE;
    }
    if (!sNoticeKeepsTyping || (Key < 0x20) || (Key >= 0x7F)) {
      return TRUE;
    }
    NoticeDismiss (A);
    /* fall through: this key belongs to the typed field */
  }

  /* Tab / Shift+Tab walk this dialog's buttons. */
  if ((Key == LVGL_KEY_TAB) || (Key == LVGL_KEY_TAB_PREV)) {
    ModalFocusStep ((Key == LVGL_KEY_TAB) ? 1 : -1);
    return TRUE;
  }

  if ((Key == LV_KEY_ENTER) && (sFocusCount > 0)) {
    lv_obj_send_event (sFocusBtn[sFocusIdx], LV_EVENT_CLICKED, NULL);
    return TRUE;
  }

  if (sState == DLG_TYPED) {
    if (Key == LV_KEY_ESC) {
      CloseModal (A);
      return TRUE;
    }
    if (Key == LV_KEY_BACKSPACE) {
      UINTN Len = AsciiStrLen (sTyped);
      if (Len > 0) {
        sTyped[Len - 1] = 0;
        lv_label_set_text (sTypedLabel, sTyped);
        UpdateTypedHint ();
      }
      return TRUE;
    }
    /* Enter is handled above, by the focus ring: 确认擦除 is the default focus
       in this dialog, so plain Enter still confirms, and Tab+Enter can reach
       取消. */
    /* Printable ASCII only. Letters are upper-cased because the expected
       string is upper-case and QEMU's PS/2 layer reports Shift inconsistently
       (the same reason gufile relaxed Ctrl+Shift+N to Ctrl+N). */
    if (Key >= 0x20 && Key < 0x7F) {
      UINTN Len = AsciiStrLen (sTyped);
      if (Len + 1 < sizeof (sTyped)) {
        CHAR8 C = (CHAR8)Key;
        if (C >= 'a' && C <= 'z') {
          C = (CHAR8)(C - 'a' + 'A');
        }
        sTyped[Len]     = C;
        sTyped[Len + 1] = 0;
        lv_label_set_text (sTypedLabel, sTyped);
        UpdateTypedHint ();
        DEBUG ((DEBUG_INFO, "[Ui] typed now='%a' (expect '%a')\n",
                sTyped, sExpected));
      }
      return TRUE;
    }
    return TRUE;
  }

  if (Key == LV_KEY_ESC) {
    CloseModal (A);
    return TRUE;
  }

  /* F2 exports the report. The export button has no keyboard path - Tab does
     not move focus inside a modal - so without this a keyboard-only user can
     read the result but never save it. */
  if ((sState == DLG_REPORT) && ((Key == LVGL_KEY_F2) || (Key == 'e') ||
                                 (Key == 'E'))) {
    ReportExportCb (NULL);
    return TRUE;
  }

  /* Enter activates whatever the dialog named as its default button. A freshly
     built modal has nothing focused in the keyboard group, so without this a
     keyboard-only user reaches the summary dialog and stops there. */
  if ((Key == LV_KEY_ENTER) && (sDefaultBtn != NULL)) {
    lv_obj_send_event (sDefaultBtn, LV_EVENT_CLICKED, NULL);
    return TRUE;
  }

  /* Algorithm dialog: +/- adjusts the custom pass count. */
  if (sState == DLG_ALGO) {
    if (Key == '+' || Key == '=') {
      if (A->CustomPasses < 64) {
        A->CustomPasses++;
      }
      DialogsPickAlgorithm (A);
      return TRUE;
    }
    if (Key == '-' || Key == '_') {
      if (A->CustomPasses > 1) {
        A->CustomPasses--;
      }
      DialogsPickAlgorithm (A);
      return TRUE;
    }
  }

  return TRUE;
}
