/** @file
  Platform/TextConv.h - UTF-16 (UEFI CHAR16) to UTF-8 (LVGL) conversion.

  LVGL takes UTF-8; UEFI strings are UTF-16. The obvious-looking shortcut,
  UnicodeStrToAsciiStrS, is WRONG for anything non-ASCII: it asserts on any
  code unit >= 0x100 (SafeString.c: "*Source < 0x100") and leaves the output
  truncated. A Chinese label built that way silently loses its Chinese and
  keeps only the digits, which reads like a formatting bug rather than an
  encoding bug.

  Copyright (c) 2026, Mike Wu. All rights reserved.
**/
#ifndef ERASE_PLATFORM_TEXT_CONV_H
#define ERASE_PLATFORM_TEXT_CONV_H

#include <Uefi.h>
#include "../Core/Port.h"

/** Convert a NUL-terminated UTF-16 string to UTF-8.

    Surrogate pairs are handled (so characters outside the BMP work), and the
    output is always NUL-terminated when OutBytes > 0. A character that would
    not fit is dropped rather than half-written, and the conversion stops there.

    @param[in]  In        source UTF-16 string
    @param[out] Out       destination buffer
    @param[in]  OutBytes  size of Out in BYTES (not characters)

    @return number of bytes written, excluding the terminator
*/
UINTN
Utf16ToUtf8 (
  IN  CONST CHAR16  *In,
  OUT CHAR8         *Out,
  IN  UINTN          OutBytes
  );

/** Convert a UTF-8 string to UTF-16. Used for the reverse direction (e.g. an
    ASCII constant into a CHAR16 buffer). */
UINTN
Utf8ToUtf16 (
  IN  CONST CHAR8   *In,
  OUT CHAR16        *Out,
  IN  UINTN          OutChars
  );

#endif /* ERASE_PLATFORM_TEXT_CONV_H */
