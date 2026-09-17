/** @file
  Ui/MainWindow.h - window chrome, target list, focus and key routing.

  Copyright (c) 2026, Mike Wu. All rights reserved.
**/
#ifndef ERASE_UI_MAIN_WINDOW_H
#define ERASE_UI_MAIN_WINDOW_H

#include "AppCtx.h"

/** Text font for the CJK UI strings (defined in MainWindow.c, shared with the
    dialogs). Declared here rather than in each .c so the two cannot drift. */
VOID
UiFont (
  IN lv_obj_t  *Obj
  );

/** Keep a widget OUT of LVGL's default keypad group. Only the window's inert
    key sink may sit in it: a real widget there gets an extra CLICKED from
    LVGL's own ENTER handling, on top of the ring's action. */
VOID
UiGroupDetach (
  IN lv_obj_t  *Obj
  );

/** Icon font: the LV_SYMBOL_* glyphs live in Montserrat, not in the CJK subset. */
VOID
IconFont (
  IN lv_obj_t  *Obj
  );

#define UI_MENU_H    24
#define UI_TOOL_H    40
#define UI_STATUS_H  24

/** Build the whole window: menu bar, toolbar, target list, status bar and the
    version watermark. */
VOID
MainWindowCreate (
  IN OUT APP_CTX  *A,
  IN     lv_obj_t *Scr
  );

/** Rebuild the target rows (after enumeration or a selection change). */
VOID
MainWindowRefreshList (
  IN OUT APP_CTX  *A
  );

/** Refresh the algorithm / selection summary in the status bar. */
VOID
MainWindowRefreshAlgo (
  IN OUT APP_CTX  *A
  );

/** Move the keyboard focus ring to `Zone` (Tab / Shift+Tab). */
VOID
MainWindowSetZone (
  IN OUT APP_CTX  *A,
  IN     FOCUS_ZONE Zone
  );

/** Screen-level key dispatch. Returns TRUE when the key was consumed. */
BOOLEAN
MainWindowKey (
  IN OUT APP_CTX  *A,
  IN     UINT32    Key
  );

/** Toggle one row's selection, optionally applying Ctrl/Shift semantics. */
VOID
MainWindowToggleRow (
  IN OUT APP_CTX  *A,
  IN     UINT32    Row,
  IN     BOOLEAN   Ctrl,
  IN     BOOLEAN   Shift
  );

/** The UI pump handed to the erasure session: redraws the progress overlay and
    lets the user press Cancel. Called from inside the erasure loop. */
VOID
MainWindowPump (
  IN VOID  *Ctx
  );

#endif /* ERASE_UI_MAIN_WINDOW_H */
