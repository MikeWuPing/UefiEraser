/** @file
  DebugPrintErrorLevelLib instance with a fixed print mask.

  Always reports DEBUG_ERROR|DEBUG_INIT|DEBUG_WARN|DEBUG_LOAD|DEBUG_INFO
  (the usual EDK II DEBUG-build mask 0x80000047) so the serial log works
  the same under any firmware PCD database. Set is a no-op.

  Copyright (c) 2026, Mike Wu. All rights reserved.
**/

#include <Base.h>
#include <Library/DebugLib.h>
#include <Library/DebugPrintErrorLevelLib.h>

/**
  Returns the fixed debug print error level mask.

  @retval  Fixed mask: DEBUG_ERROR|DEBUG_INIT|DEBUG_WARN|DEBUG_LOAD|DEBUG_INFO.
**/
UINT32
EFIAPI
GetDebugPrintErrorLevel (
  VOID
  )
{
  return DEBUG_ERROR | DEBUG_INIT | DEBUG_WARN | DEBUG_LOAD | DEBUG_INFO;
}

/**
  Stub: the mask is fixed, so setting always "succeeds" without effect.

  @param  ErrorLevel  Ignored.

  @retval TRUE  Always.
**/
BOOLEAN
EFIAPI
SetDebugPrintErrorLevel (
  UINT32  ErrorLevel
  )
{
  return TRUE;
}
