/** @file
  Platform/AtaErase.h - ATA Security feature set erasure (Secure Erase).

  This is the DEVICE-LEVEL path and it is the only correct one for an SSD.
  Multi-pass overwrite cannot erase an SSD reliably: the FTL remaps logical
  blocks to different physical pages, so writing LBA N does not overwrite the
  flash page that previously held LBA N. ATA Secure Erase is executed by the
  drive's own firmware, which resets the encryption key or erases every physical
  block including the over-provisioned and retired ones.

  Interface choice, worth recording because the obvious candidate is wrong:
  EFI_STORAGE_SECURITY_COMMAND_PROTOCOL looks like the right protocol (it even
  says "security"), but it only issues ATA TRUSTED SEND / TRUSTED RECEIVE. The
  SECURITY ERASE commands (0xF1 SET PASSWORD, 0xF3 ERASE PREPARE, 0xF4 ERASE
  UNIT) are a different ATA command family and are unreachable through it.
  EFI_ATA_PASS_THRU_PROTOCOL can issue any ATA command, so that is what this
  uses.

  Command sequence (ACS-3):
    1. IDENTIFY DEVICE          - verify the Security feature set is supported,
                                  enabled, and NOT frozen
    2. SECURITY SET PASSWORD    - install a temporary user password
    3. SECURITY ERASE PREPARE   - arm the erase
    4. SECURITY ERASE UNIT      - execute it; the Enhanced variant also erases
                                  the device's hidden areas where supported
    5. poll until the device reports ready

  The Frozen state matters: a device is frozen when the BIOS/firmware has locked
  the security interface, and SECURITY ERASE UNIT is then refused outright. That
  is reported as an error rather than silently downgraded to an overwrite, since
  the user asked for an erase that actually works on their media.

  Copyright (c) 2026, Mike Wu. All rights reserved.
**/
#ifndef ERASE_PLATFORM_ATA_ERASE_H
#define ERASE_PLATFORM_ATA_ERASE_H

#include <Uefi.h>
#include "../Core/Port.h"
#include "../Core/EraseEngine.h"
#include "OverwriteErase.h"

/** What IDENTIFY DEVICE reported about the Security feature set. */
typedef struct {
  BOOLEAN  Supported;
  BOOLEAN  Enabled;
  BOOLEAN  Locked;
  BOOLEAN  Frozen;
  BOOLEAN  EnhancedEraseSupported;
  UINT16   EraseTimeNormalMinutes;
  UINT16   EraseTimeEnhancedMinutes;
} ATA_SECURITY_INFO;

/** Find the ATA pass-thru port serving `DeviceHandle` and read IDENTIFY DEVICE.

    @retval EFI_SUCCESS      info filled in
    @retval EFI_NOT_FOUND    the device is not behind an ATA pass-thru instance
    @retval others           the IDENTIFY command failed
*/
EFI_STATUS
AtaSecurityQuery (
  IN  EFI_HANDLE          DeviceHandle,
  OUT ATA_SECURITY_INFO   *Info
  );

/** Run ATA Secure Erase on the device behind `DeviceHandle`.

    @param[in]  Enhanced   request the Enhanced variant when the device supports
                           it (falls back to Normal, and says so, when not)
    @param[in]  Pump       UI pump, may be NULL
    @param[in]  PumpCtx    context for Pump
    @param[out] Report     filled in; BytesWritten is 0 (no host writes happen)

    @retval EFI_SUCCESS       the device reported the erase completed
    @retval EFI_UNSUPPORTED   the Security feature set is unsupported or disabled
    @retval EFI_ACCESS_DENIED the device is Frozen - needs a power cycle
    @retval EFI_TIMEOUT       the device did not become ready
    @retval others            an ATA command failed
*/
EFI_STATUS
AtaSecureErase (
  IN  EFI_HANDLE      DeviceHandle,
  IN  BOOLEAN         Enhanced,
  IN  ERASE_PUMP_CB   Pump,
  IN  VOID           *PumpCtx,
  OUT ERASE_REPORT   *Report
  );

#endif /* ERASE_PLATFORM_ATA_ERASE_H */
