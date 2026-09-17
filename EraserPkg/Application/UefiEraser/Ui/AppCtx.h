/** @file
  Ui/AppCtx.h - the application context.

  One struct owns everything the UI and the erasure session need: the
  enumerated devices, the flattened target list, the current algorithm and
  options, the LVGL object handles, and the focus state.

  Copyright (c) 2026, Mike Wu. All rights reserved.
**/
#ifndef ERASE_UI_APP_CTX_H
#define ERASE_UI_APP_CTX_H

#include <Uefi.h>
#include <Library/LvglLib.h>

#include "../Core/Port.h"
#include "../Core/TargetModel.h"
#include "../Core/EraseAlgo.h"
#include "../Platform/BlockDevice.h"
#include "../Platform/PartitionMap.h"
#include "../Platform/OverwriteErase.h"

/** Keyboard focus zones. Tab cycles through them; each zone remembers its own
    focused row so returning to it lands where the user left off. */
typedef enum {
  FZ_TARGETS = 0,
  FZ_BUTTONS,
  FZ_COUNT
} FOCUS_ZONE;

/** How the erase is performed. OVERWRITE uses the algorithm engine;
    the device-level modes hand control to the drive's own firmware. */
typedef enum {
  ERASE_MODE_OVERWRITE = 0,
  ERASE_MODE_ATA,
  ERASE_MODE_NVME_FORMAT,
  ERASE_MODE_NVME_SANITIZE,
  ERASE_MODE_COUNT
} ERASE_MODE;

#define APP_MAX_ROWS    (BLOCK_DEV_MAX * 2 + 4)
#define APP_MAX_BUTTONS 8

typedef struct {
  /* ---- enumerated model ---- */
  BLOCK_DEV_LIST  Devs;
  PARTMAP         Maps[BLOCK_DEV_MAX];      /**< per whole-disk device */
  ERASE_TARGET    Targets[APP_MAX_ROWS];    /**< flattened, display order */
  UINT32          TargetCount;

  /* ---- options ---- */
  ERASE_MODE      Mode;
  UINT32          AlgoIndex;
  UINT32          CustomPasses;
  BOOLEAN         Verify;

  /* ---- erasure ---- */
  ERASE_SESSION   Session;
  BOOLEAN         SessionActive;
  ERASE_STATUS    LastStatus;               /**< from the last run, for the report */
  UINT64          LastBytesWritten;
  UINT32          LastPassesDone;
  UINT32          LastVerifyMismatches;
  BOOLEAN         LastCancelled;
  BOOLEAN         LastVerified;
  CHAR16          LastTargetName[TARGET_NAME_MAX];

  /* ---- LVGL ---- */
  lv_obj_t       *Scr;
  lv_obj_t       *MenuBar;
  lv_obj_t       *MenuPopup;      /**< open dropdown, or NULL */
  lv_obj_t       *ToolBar;
  lv_obj_t       *ListBox;        /**< target list container */
  lv_obj_t       *Rows[APP_MAX_ROWS];
  lv_obj_t       *Buttons[APP_MAX_BUTTONS];
  UINT32          ButtonCount;
  lv_obj_t       *StatusLabel;
  lv_obj_t       *AlgoLabel;
  lv_obj_t       *Overlay;        /**< modal root, or NULL when none */
  /** Transient hint bar above the status bar. Clicking a target that cannot be
      selected (read-only device, boot volume) used to do nothing at all, and
      "全选" silently skipped those rows: the user could not tell an impossible
      target from a broken click. Anything refused says why here, for a few
      seconds. */
  lv_obj_t       *HintBar;
  lv_obj_t       *HintLabel;
  /** The only object this app leaves in LVGL's default keypad group. LVGL both
      requires a focused member for a key to be delivered at all AND turns ENTER
      into PRESSED/CLICKED on it, so a real widget there fires a second action
      behind the ring's back. See the comment in MainWindowCreate. */
  lv_obj_t       *KeySink;

  /* ---- focus / keys ---- */
  FOCUS_ZONE      Zone;
  UINT32          RowFocus;
  UINT32          ButtonFocus;
  UINT32          LastClickedRow;  /**< anchor for Shift+click ranges */
  UINT32          SelCount;

  /* ---- misc ---- */
  /** The image handle the app was loaded as. Kept so the target list can be
      enumerated again after an erasure (BlockDevEnumerate needs it to detect
      the boot volume). */
  EFI_HANDLE      ImageHandle;
  BOOLEAN         Quit;
  /** Set by the last confirmation gate, served by the main loop.

      The erasure CANNOT run from the gate's click handler: that handler is an
      LVGL event callback, so it runs inside lv_timer_handler () - the indev read
      timer dispatches the click. A synchronous multi-second erasure there keeps
      LVGL's re-entrancy guard set for its whole duration, and every
      lv_timer_handler () the progress pump calls then returns instantly without
      running a single timer. Timers are what drive animations AND the display
      refresh, so the progress dialog never painted at all (measured 2026-09-17:
      every pump logged next=1 ms and 12 screendumps over 38 s were byte
      identical). The job is queued here instead and served from the main loop,
      outside the timer handler. */
  BOOLEAN         EraseRequested;
} APP_CTX;

/** The active context (single-window app). Set once in UefiMain. */
extern APP_CTX  *gApp;

/** What one re-scan of the world found. Informational only - by the time the
    caller sees it the list has already been rebuilt; it is what turns "nothing
    happened" and "three dead rows were dropped" into different messages. */
typedef struct {
  UINT32  Devs;         /**< block devices the firmware reported */
  UINT32  WasTargets;   /**< rows in the list before the re-scan */
  UINT32  Targets;      /**< rows after it */
  UINT32  Ghosts;       /**< partition devices the media no longer describes */
} RESCAN_INFO;

/** Rebuild the flattened target list from Devs/Maps, preserving selection
    where a device is still present.

    @param[out] GhostsDropped  optional (may be NULL): how many partition
                               devices were skipped because the media no longer
                               describes them
*/
VOID
AppBuildTargets (
  IN OUT APP_CTX  *A,
  OUT    UINT32   *GhostsDropped
  );

/** Recompute the selection count and refresh the status bar. */
VOID
AppRefreshStatus (
  IN OUT APP_CTX  *A
  );

/** Human-readable name for the current erase mode. */
CONST CHAR8 *
AppModeName (
  IN CONST APP_CTX  *A
  );

/** Current algorithm (never NULL: index is clamped). */
CONST ERASE_ALGORITHM *
AppCurrentAlgo (
  IN CONST APP_CTX  *A
  );

/** Number of passes the current options imply. */
UINT32
AppCurrentPassCount (
  IN CONST APP_CTX  *A
  );

/** Total bytes the current selection would write, i.e. sum(selected target
    lengths) * passes. Saturates. */
UINT64
AppSelectedBytes (
  IN CONST APP_CTX  *A
  );

/** Re-enumerate the block devices, re-read every partition table from the
    media, and rebuild the target list.

    Called after an erasure and from the 操作 menu's 重新扫描目标.

    Nothing is asked of the firmware beyond "what devices do you have": what
    still EXISTS on each of them is read back off the media. The alternative -
    disconnect/connect each disk so the firmware rebuilds its partition children
    (the scoped `reconnect -r`) - was tried, worked under OVMF, and was dropped
    anyway. It puts a storage driver's Stop()/Start() on the critical path of an
    erasure tool, where a driver that does not come back takes the disk out of
    the list until a reboot; and it is not needed, because the list can be
    derived from the same bytes the user cares about.

    Selection survives by (handle, partition) in AppBuildTargets, so a target
    that still exists keeps its state and one that has ceased to exist takes its
    selection with it.

    @param[out] Info  optional (may be NULL): counts for a log line or a hint
*/
VOID
AppRefreshTargets (
  IN OUT APP_CTX      *A,
  OUT    RESCAN_INFO  *Info
  );

/** Number of selected targets. */
UINT32
AppSelectedCount (
  IN CONST APP_CTX  *A
  );

#endif /* ERASE_UI_APP_CTX_H */
