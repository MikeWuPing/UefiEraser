/** @file
  Platform/LogPersist.h - append an erase-run record to a log file on the
  boot volume for audit / troubleshooting.

  The log is a plain CSV (UTF-8) written to the volume the app booted from:
    UefiEraser.log

  Fields (one line per target erased):
    iso_timestamp, target_name, mode, algo, passes, result, bytes_written

  If the file does not exist it is created. If it does exist the new record
  is appended. Failure to write the log is silently ignored: the erase itself
  is the critical operation and a missing audit trail must not abort it.

  Copyright (c) 2026, Mike Wu. All rights reserved.
**/
#ifndef ERASE_PLATFORM_LOG_PERSIST_H
#define ERASE_PLATFORM_LOG_PERSIST_H

#include <Uefi.h>

/** Write a single-line CSV record describing one erase run.

    Uses the global gImageHandle to locate the boot volume. The log is appended
    to UefiEraser.log on the volume the application was loaded from.

    @param[in]  TargetName   UTF-16 target name (converted to UTF-8)
    @param[in]  ModeName     "overwrite", "ata", "nvme-format", "nvme-sanitize"
    @param[in]  AlgoName     algorithm id, or "-" for device-level
    @param[in]  Passes       pass count (0 for device-level)
    @param[in]  Result       "OK", "CANCELLED", "IO_ERROR", "VERIFY_FAIL", ...
    @param[in]  BytesWritten host-side bytes written (0 for device-level)
*/
VOID
LogPersistRecord (
  IN CONST CHAR16   *TargetName,
  IN CONST CHAR8    *ModeName,
  IN CONST CHAR8    *AlgoName,
  IN UINT32          Passes,
  IN CONST CHAR8    *Result,
  IN UINT64          BytesWritten
  );

/** Everything the human-readable report needs. Both entry points fill this
    in: the UI after a run and the headless mode after each target. */
typedef struct {
  CONST CHAR8  *ModeName;      /**< "overwrite", "ata", "nvme-format", ... */
  CONST CHAR8  *AlgoId;        /**< algorithm id, or "-" for device-level */
  CONST CHAR8  *Result;        /**< "OK", "CANCELLED", "IO_ERROR", ... */
  BOOLEAN       Verify;        /**< read-back verification was requested */
  UINT32        Targets;       /**< targets attempted in this run */
  UINT32        Failed;        /**< how many of those failed */
  UINT64        BytesWritten;  /**< host-side bytes written */
  UINT32        Passes;        /**< passes completed */
  UINT32        Mismatches;    /**< bytes that failed read-back verification */
  CONST CHAR16 *TargetsText;   /**< '\n'-separated target list, or NULL */
} ERASE_REPORT_SUMMARY;

/** Write the human-readable report for one run to UefiEraser-report.txt on the
    boot volume, overwriting whatever a previous run left there.

    Kept next to the CSV logger on purpose: both are audit artifacts of the
    same run, and both are best-effort - a failure here must never turn a
    completed erasure into an error.

    @param[in]  S  the run summary

    @retval TRUE   the file was written in full
    @retval FALSE  the boot volume could not be opened, the file could not be
                   created, or a short write occurred
*/
BOOLEAN
LogPersistWriteReport (
  IN CONST ERASE_REPORT_SUMMARY  *S
  );

#endif /* ERASE_PLATFORM_LOG_PERSIST_H */
