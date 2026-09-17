/** @file
  Ui/GlassChrome.c - see GlassChrome.h.

  Copyright (c) 2026, Mike Wu. All rights reserved.
**/

#include "GlassChrome.h"

#include <Uefi.h>
#include <Library/DebugLib.h>
#include <Library/LvglGlass.h>
#include <Library/LvglTheme.h>

STATIC LVGL_GLASS  mGlass;
STATIC BOOLEAN     mGlassReady = FALSE;
STATIC lv_obj_t   *mCard       = NULL;

lv_obj_t *
GlassChromeInit (
  IN lv_obj_t  *Scr
  )
{
  if (Scr == NULL) {
    return NULL;
  }

  /* The library theme table must exist before any other theme-layer call
     (LvglThemeAttachGlass / LvglThemeSetMode). Idempotent. */
  LvglThemeInit ();

  if (!LvglGlassInit (&mGlass)) {
    DEBUG ((DEBUG_WARN, "[Glass] disabled (init failed), falling back to solid theme\n"));
    return Scr;
  }
  LvglGlassApplyScreen (Scr, &mGlass, FALSE);

  /* Keep the default inset rectangle (on 1280x800: card 24,24-1255,775,
     radius 16, pad 8): the window floats on the wallpaper and the ring
     between the card edge and its content is what makes the glass read as
     glass. Overriding CardArea to full-screen zeroes that ring. */
  mCard = LvglGlassCardCreate (Scr, &mGlass);
  if (mCard == NULL) {
    DEBUG ((DEBUG_WARN, "[Glass] card create failed, host = screen\n"));
    return Scr;
  }
  /* Key events must bubble from the subtree to the screen handler. */
  lv_obj_add_flag (mCard, LV_OBJ_FLAG_EVENT_BUBBLE);

  LvglThemeAttachGlass (&mGlass, Scr, mCard);
  mGlassReady = TRUE;

  /* Dark is the palette this UI was designed against. */
  LvglThemeSetMode (LVGL_THEME_MODE_DARK);

  DEBUG ((
    DEBUG_INFO,
    "[Glass] chrome ready screen=%dx%d card=%d,%d-%d,%d radius=%d pad=%d margin=%d\n",
    (UINT32)mGlass.W, (UINT32)mGlass.H,
    (INT32)mGlass.CardArea.x1, (INT32)mGlass.CardArea.y1,
    (INT32)mGlass.CardArea.x2, (INT32)mGlass.CardArea.y2,
    (INT32)mGlass.Radius, (INT32)mGlass.Pad, (INT32)mGlass.Margin
    ));
  return mCard;
}

VOID
GlassChromeFree (
  VOID
  )
{
  if (!mGlassReady) {
    return;
  }
  LvglGlassFree (&mGlass);
  mGlassReady = FALSE;
  mCard       = NULL;
}

BOOLEAN
GlassChromeActive (
  VOID
  )
{
  return (BOOLEAN)(mGlassReady && (mCard != NULL));
}

VOID
GlassChromeDecor (
  IN BOOLEAN  On
  )
{
  if (!GlassChromeActive ()) {
    return;
  }
  LvglGlassCardDecor (mCard, &mGlass, On);
}
