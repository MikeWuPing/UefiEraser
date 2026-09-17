/** @file
  Ui/GlassChrome.h - the LvglPkg frosted-glass session, app side.

  One call does the whole assembly (mirroring the sibling projects):
  install the library theme table -> init the glass session (wallpaper +
  downsampled blur) -> lay the wallpaper on the screen -> create the glass
  card -> attach it to the library theme layer so a later mode switch
  regenerates and re-lays everything.

  Failure is never fatal: if any step fails the caller gets the screen back as
  the host and the UI comes up on the solid palette instead (the LvglGlass
  contract).

  Host contract: the returned object is the parent of the whole app UI (the
  glass card, or the screen when glass is off). It carries EVENT_BUBBLE so key
  events from the subtree still reach the screen handler - the same trap the
  siblings hit (without it ESC silently stops working).

  Copyright (c) 2026, Mike Wu. All rights reserved.
**/
#ifndef ERASER_UI_GLASS_CHROME_H
#define ERASER_UI_GLASS_CHROME_H

#include <Library/LvglLib.h>

/** Build the glass session and return the host for the app UI (the glass card,
    or Scr when glass is unavailable). */
lv_obj_t *
GlassChromeInit (
  IN lv_obj_t  *Scr
  );

/** Release the glass buffers (idempotent). Must run before lv_deinit. */
VOID
GlassChromeFree (
  VOID
  );

/** TRUE when the glass card is live. */
BOOLEAN
GlassChromeActive (
  VOID
  );

/** Turn the card's glass layer off (solid base, no shadow) or back on.

    Used around the erasure: the progress dialog invalidates the whole screen on
    every sampling window, and with the glass layer on each of those redraws a
    full-screen blur stretch plus a shadow. With it off the card is opaque, so
    LVGL starts drawing at the card and the wallpaper behind is never touched -
    the erasure runs at the same cost as before glass existed. */
VOID
GlassChromeDecor (
  IN BOOLEAN  On
  );

#endif /* ERASER_UI_GLASS_CHROME_H */
