/** @file
  Ui/Dialogs.h - modal dialogs: algorithm picker, confirmation gates, progress,
  report, about.

  Copyright (c) 2026, Mike Wu. All rights reserved.
**/
#ifndef ERASE_UI_DIALOGS_H
#define ERASE_UI_DIALOGS_H

#include "AppCtx.h"

/** Algorithm picker. Also lets the user set the custom pass count and toggle
    read-back verification. */
VOID
DialogsPickAlgorithm (
  IN OUT APP_CTX  *A
  );

/** The confirmation chain, then the erasure, then the report. This is the only
    entry point that can destroy data. */
VOID
DialogsConfirmAndRun (
  IN OUT APP_CTX  *A
  );

VOID
DialogsAbout (
  IN OUT APP_CTX  *A
  );

/** Key routing while a modal is open. Returns TRUE when the key was consumed;
    the caller must not fall through to the window when it is. */
BOOLEAN
DialogsKey (
  IN OUT APP_CTX  *A,
  IN     UINT32    Key
  );

/** Refresh the progress overlay. Called from the erasure pump. */
VOID
DialogsUpdateProgress (
  IN OUT APP_CTX  *A
  );

/** Run the erasure the confirmation gates have just authorised.

    Called from the main loop, NOT from the click handler - see the
    EraseRequested comment in AppCtx.h. Everything up to and including the
    progress dialog has already happened by the time this runs. */
VOID
DialogsRunErase (
  IN OUT APP_CTX  *A
  );

/** TRUE when a modal is on screen (the window behind must not react). */
BOOLEAN
DialogsIsOpen (
  VOID
  );

#endif /* ERASE_UI_DIALOGS_H */
