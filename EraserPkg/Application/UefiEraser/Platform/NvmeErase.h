/** @file
  Platform/NvmeErase.h - NVMe Format NVM and Sanitize.

  The NVMe counterpart to ATA Secure Erase, and for the same reason: an SSD's
  FTL makes multi-pass overwrite meaningless, while Format/Sanitize are executed
  by the controller across every physical block, including over-provisioned and
  retired ones.

  Two commands, both Admin queue:

    Format NVM (0x80)  - per-namespace. SES (CDW10 bits 13:12):
                         1 = User Data Erase    (erase all user data)
                         2 = Cryptographic Erase (discard the media key; fastest)
                         Note this also rewrites the LBA format and metadata
                         settings, so it is a real format, not just a wipe.

    Sanitize (0x84)    - whole device. SANACT (CDW10 bits 2:0):
                         1 = Block Erase
                         2 = Overwrite
                         4 = Crypto Erase
                         Progress is reported through the Sanitize Status log
                         (LID 0x81, SPROG), which this polls.

  Which actions a given device supports is advertised in Identify Controller's
  SANICAP field, so the UI can grey out what would be refused instead of letting
  the user discover it by failing.

  Copyright (c) 2026, Mike Wu. All rights reserved.
**/
#ifndef ERASE_PLATFORM_NVME_ERASE_H
#define ERASE_PLATFORM_NVME_ERASE_H

#include <Uefi.h>
#include "../Core/Port.h"
#include "../Core/EraseEngine.h"
#include "OverwriteErase.h"

/* Format NVM - Secure Erase Setting (CDW10 bits 13:12). */
#define NVME_SES_USER_DATA_ERASE  1u
#define NVME_SES_CRYPTO_ERASE     2u

/* Sanitize - Sanitize Action (CDW10 bits 2:0). */
#define NVME_SANACT_BLOCK_ERASE   1u
#define NVME_SANACT_OVERWRITE     2u
#define NVME_SANACT_CRYPTO_ERASE  4u

/** What Identify Controller reported. */
typedef struct {
  UINT32   Sanicap;              /**< raw SANICAP field */
  BOOLEAN  SanitizeBlockErase;
  BOOLEAN  SanitizeOverwrite;
  BOOLEAN  SanitizeCryptoErase;
  BOOLEAN  NoDeallocateInSanitize; /**< SANICAP bit 2 */
  UINT8    FormatSesSupported;   /**< bitmask of SES values the device accepts */
  CHAR8    Model[41];            /**< ASCII, trimmed */
  CHAR8    Serial[21];
} NVME_ERASE_CAPS;

/** Locate the NVMe namespace behind `DeviceHandle` and read Identify Controller.

    @retval EFI_SUCCESS    caps filled in
    @retval EFI_NOT_FOUND  the device is not behind an NVMe pass-thru instance
*/
EFI_STATUS
NvmeEraseQuery (
  IN  EFI_HANDLE          DeviceHandle,
  OUT NVME_ERASE_CAPS     *Caps
  );

/** Format NVM with the given SES on the namespace behind `DeviceHandle`.

    @param[in]  Ses   NVME_SES_USER_DATA_ERASE or NVME_SES_CRYPTO_ERASE
*/
EFI_STATUS
NvmeFormat (
  IN  EFI_HANDLE      DeviceHandle,
  IN  UINT8           Ses,
  IN  ERASE_PUMP_CB   Pump,
  IN  VOID           *PumpCtx,
  OUT ERASE_REPORT   *Report
  );

/** Sanitize the whole device with the given action.

    Polls the Sanitize Status log while the controller works, so the UI can show
    real progress instead of a spinner.
*/
EFI_STATUS
NvmeSanitize (
  IN  EFI_HANDLE      DeviceHandle,
  IN  UINT8           Action,
  IN  ERASE_PUMP_CB   Pump,
  IN  VOID           *PumpCtx,
  OUT ERASE_REPORT   *Report
  );

#endif /* ERASE_PLATFORM_NVME_ERASE_H */
