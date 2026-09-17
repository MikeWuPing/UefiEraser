/** @file
  UefiEraser - secure disk data shredder for the UEFI Shell.

  Startup order matters:
    1. emit APP_VERSION to the serial channel FIRST, before anything that can
       fail, so a boot that dies later still produces the version evidence
       tools/Test-AppVersion.ps1 needs
    2. bring up the LVGL UEFI port (GOP display, keyboard/mouse indev, tick)
    3. enumerate block devices and their partition tables
    4. build the flattened target list and the window
    5. main loop: pump the port (input + lv_timer_handler)

  There is no selftest argument mode here on purpose: the Core layer is
  UEFI-free and is verified far more cheaply by tools/hosttests, which runs 360
  assertions in milliseconds instead of booting an emulator.

  Copyright (c) 2026, Mike Wu. All rights reserved.
**/

#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DebugLib.h>
#include <Library/LvglLib.h>
#include <Library/LvglUefiPort.h>
#include <Library/LvglTrans.h>

#include "Version.h"
#include "Core/EraseAlgo.h"
#include "Platform/BlockDevice.h"
#include "Platform/HeadlessErase.h"
#include "Platform/PartitionMap.h"
#include "Ui/AppCtx.h"
#include "Ui/MainWindow.h"
#include "Ui/Dialogs.h"
#include "Ui/GlassChrome.h"

EFI_STATUS
EFIAPI
UefiMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  APP_CTX     *A;
  lv_obj_t   *Host;
  UINT32      I;

  /* ---- 1. version evidence, before anything that can fail ---- */
  DEBUG ((DEBUG_INFO, "[UefiEraser] start\n"));
  DEBUG ((DEBUG_INFO, "APP_VERSION=%a\n", UEFIERASER_VERSION_STR));
  DEBUG ((DEBUG_INFO, "[UefiEraser] built %a %a\n", __DATE__, __TIME__));
  Print (L"UefiEraser %a\r\n", UEFIERASER_VERSION_STR);

  /* ---- 2. headless mode? ---- */
  /* The tick is initialised even for headless runs. It costs one 100 ms
     gBS->Stall calibration and is NOT optional: LvglTickGetMs returns 0 while
     mTscFreq is uncalibrated (the port guards against a divide-by-zero), and
     the erasure session's progress/ETA reporting is gated on that clock. A
     headless run with a frozen clock produces no progress output at all. */
  LvglTickInit ();

  A = AllocateZeroPool (sizeof (APP_CTX));
  if (A == NULL) {
    DEBUG ((DEBUG_ERROR, "[UefiEraser] out of memory for APP_CTX\n"));
    return EFI_OUT_OF_RESOURCES;
  }
  gApp = A;
  /* Remembered so the target list can be re-enumerated after an erasure: the
     boot-volume flag is derived from it. */
  A->ImageHandle = ImageHandle;

  if (HeadlessEraseRequested (ImageHandle)) {
    Status = BlockDevEnumerate (&A->Devs, ImageHandle);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "[UefiEraser] enumeration failed: %r\n", Status));
      FreePool (A);
      return Status;
    }
    Status = HeadlessEraseRun (ImageHandle, &A->Devs);
    DEBUG ((DEBUG_INFO, "[UefiEraser] headless exit: %r\n", Status));
    FreePool (A);
    gApp = NULL;
    return Status;
  }

  /* ---- 3. LVGL UEFI port ---- */
  Status = LvglPortInit ();
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[UefiEraser] LvglPortInit failed: %r\n", Status));
    FreePool (A);
    return Status;
  }

  /* Safe defaults: the DoD 3-pass method, and no read-back verification (which
     doubles the I/O and is off by default in Eraser too). */
  A->AlgoIndex    = 1;
  A->CustomPasses = 3;
  A->Verify       = FALSE;

  /* ---- 3. enumerate ---- */
  Status = BlockDevEnumerate (&A->Devs, ImageHandle);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[UefiEraser] enumeration failed: %r\n", Status));
  }
  if (A->Devs.Overflowed) {
    DEBUG ((DEBUG_WARN, "[UefiEraser] more than %u block devices; list truncated\n",
            (UINT32)BLOCK_DEV_MAX));
  }

  /* Partition tables, for display names. Best-effort: a device whose table is
     unreadable still appears, just with an "unknown" detail line. */
  for (I = 0; I < A->Devs.Count && I < BLOCK_DEV_MAX; I++) {
    BLOCK_DEV *Dev = &A->Devs.Items[I];

    if (Dev->LogicalPartition) {
      continue;
    }
    ZeroMem (&A->Maps[I], sizeof (A->Maps[I]));
    if (Dev->BlockSize >= 512) {
      PartMapRead (Dev->BlockIo, Dev->Length, &A->Maps[I]);
    }
  }

  AppBuildTargets (A, NULL);

  /* ---- 4. window ---- */
  /* Transitions first: their self-test builds throwaway objects, so it has to
     run after the display exists and before the UI does. Then the glass
     session; its card becomes the host of the whole UI (or the screen does,
     when glass is unavailable - the failure path is not fatal). */
  LvglTransInit ();
  Host = GlassChromeInit (lv_screen_active ());
  MainWindowCreate (A, Host);

  DEBUG ((
    DEBUG_INFO,
    "[UefiEraser] UI up: %u targets, algo=%a\n",
    (UINT32)A->TargetCount,
    AppCurrentAlgo (A)->Id
    ));

  /* ---- 5. main loop ----

     The erasure is queued by the last confirmation gate rather than run from
     its click handler, and it is served HERE - after LvglPortPoll () has
     returned, i.e. outside lv_timer_handler (). That separation is what lets
     the progress dialog live: LVGL's timer handler refuses to re-enter, so a
     long job running inside it freezes every timer, and timers are what drive
     both the animations and the display refresh. Pumping the UI from *inside*
     the loop is fine (that is not re-entrant); only the initial dispatch is.

     Order matters: LvglPortPoll () first, so the progress card gets a slice in
     which to start its entry transition before the loop takes over the CPU. */
  while (!A->Quit) {
    LvglPortPoll ();

    if (A->EraseRequested && !A->Quit) {
      A->EraseRequested = FALSE;
      DialogsRunErase (A);
    }
  }

  DEBUG ((DEBUG_INFO, "[UefiEraser] ESC -> quit\n"));

  /* Defensive: the erasure loop is synchronous, so by the time Quit is set the
     session has already released its buffer - but a failed init path could
     leave a partially set-up session behind. */
  EraseSessionFree (&A->Session);

  /* Glass owns two large draw buffers; the LvglGlass contract is to release
     them before lv_deinit (which LvglPortDeinit runs). */
  GlassChromeFree ();

  LvglPortDeinit ();
  FreePool (A);
  gApp = NULL;

  DEBUG ((DEBUG_INFO, "[UefiEraser] exit\n"));
  return EFI_SUCCESS;
}
