/** @file
  Platform/TextConv.c - see TextConv.h.

  Copyright (c) 2026, Mike Wu. All rights reserved.
**/

#include "TextConv.h"

UINTN
Utf16ToUtf8 (
  IN  CONST CHAR16  *In,
  OUT CHAR8         *Out,
  IN  UINTN          OutBytes
  )
{
  UINTN O = 0;

  if (Out == NULL || OutBytes == 0) {
    return 0;
  }
  Out[0] = 0;
  if (In == NULL) {
    return 0;
  }

  while (*In != 0) {
    UINT32 Cp = (UINT32)*In++;
    UINTN  Need;
    UINT8  B[4];

    /* Combine a surrogate pair into one code point; an unpaired surrogate is
       passed through as-is rather than aborting the whole string. */
    if (Cp >= 0xD800 && Cp <= 0xDBFF && *In >= 0xDC00 && *In <= 0xDFFF) {
      Cp = 0x10000 + ((Cp - 0xD800) << 10) + ((UINT32)*In - 0xDC00);
      In++;
    }

    if (Cp < 0x80) {
      Need    = 1;
      B[0]    = (UINT8)Cp;
    } else if (Cp < 0x800) {
      Need    = 2;
      B[0]    = (UINT8)(0xC0 | (Cp >> 6));
      B[1]    = (UINT8)(0x80 | (Cp & 0x3F));
    } else if (Cp < 0x10000) {
      Need    = 3;
      B[0]    = (UINT8)(0xE0 | (Cp >> 12));
      B[1]    = (UINT8)(0x80 | ((Cp >> 6) & 0x3F));
      B[2]    = (UINT8)(0x80 | (Cp & 0x3F));
    } else {
      Need    = 4;
      B[0]    = (UINT8)(0xF0 | (Cp >> 18));
      B[1]    = (UINT8)(0x80 | ((Cp >> 12) & 0x3F));
      B[2]    = (UINT8)(0x80 | ((Cp >> 6) & 0x3F));
      B[3]    = (UINT8)(0x80 | (Cp & 0x3F));
    }

    /* Drop a character that would not fit rather than writing half of it: a
       partial sequence is invalid UTF-8 and LVGL would render garbage. */
    if (O + Need + 1 > OutBytes) {
      break;
    }
    {
      UINTN K;
      for (K = 0; K < Need; K++) {
        Out[O++] = (CHAR8)B[K];
      }
    }
  }

  Out[O] = 0;
  return O;
}

UINTN
Utf8ToUtf16 (
  IN  CONST CHAR8   *In,
  OUT CHAR16        *Out,
  IN  UINTN          OutChars
  )
{
  UINTN O = 0;

  if (Out == NULL || OutChars == 0) {
    return 0;
  }
  Out[0] = 0;
  if (In == NULL) {
    return 0;
  }

  while (*In != 0) {
    UINT32 Cp;
    UINT8  C = (UINT8)*In++;
    UINTN  Extra;

    if (C < 0x80) {
      Cp    = C;
      Extra = 0;
    } else if ((C & 0xE0) == 0xC0) {
      Cp    = C & 0x1F;
      Extra = 1;
    } else if ((C & 0xF0) == 0xE0) {
      Cp    = C & 0x0F;
      Extra = 2;
    } else if ((C & 0xF8) == 0xF0) {
      Cp    = C & 0x07;
      Extra = 3;
    } else {
      continue;                 /* invalid lead byte: skip it */
    }

    {
      UINTN K;
      for (K = 0; K < Extra; K++) {
        if (*In == 0 || ((UINT8)*In & 0xC0) != 0x80) {
          break;
        }
        Cp = (Cp << 6) | ((UINT8)*In & 0x3F);
        In++;
      }
    }

    if (Cp >= 0x10000) {
      if (O + 2 + 1 > OutChars) {
        break;
      }
      Cp -= 0x10000;
      Out[O++] = (CHAR16)(0xD800 + (Cp >> 10));
      Out[O++] = (CHAR16)(0xDC00 + (Cp & 0x3FF));
    } else {
      if (O + 1 + 1 > OutChars) {
        break;
      }
      Out[O++] = (CHAR16)Cp;
    }
  }

  Out[O] = 0;
  return O;
}
