/** @file
  Core/Port.h - the only place the Core layer touches the platform.

  The Core layer (algorithm table, erasure engine, self-test) is deliberately
  free of UEFI types so the exact same translation units can be compiled and
  unit-tested on the host (see tools/hosttests). Algorithm correctness is
  therefore provable off-target, without booting QEMU.

  Two build flavours:
    ERASE_HOST_BUILD defined  -> stdint/stddef, plain C
    otherwise                 -> EDK II types via <Uefi.h>

  Copyright (c) 2026, Mike Wu. All rights reserved.
**/
#ifndef ERASE_CORE_PORT_H
#define ERASE_CORE_PORT_H

#ifdef ERASE_HOST_BUILD

#include <stdint.h>
#include <stddef.h>
#include <string.h>

typedef uint8_t   E_U8;
typedef uint16_t  E_U16;
typedef uint32_t  E_U32;
typedef uint64_t  E_U64;
typedef size_t    E_SIZE;
typedef int       E_BOOL;

/* EDK II spellings the Core layer uses, so that the same sources read the
   same in both flavours. */
typedef void VOID;
#define CONST   const
#define STATIC  static

#define E_TRUE   1
#define E_FALSE  0
#define E_NULL   NULL
#define E_MEMCPY(Dst, Src, Len)  memcpy ((Dst), (Src), (Len))
#define E_MEMSET(Dst, Val, Len)  memset ((Dst), (Val), (Len))

#else  /* EDK II build */

#include <Uefi.h>
#include <Library/BaseMemoryLib.h>

typedef UINT8   E_U8;
typedef UINT16  E_U16;
typedef UINT32  E_U32;
typedef UINT64  E_U64;
typedef UINTN   E_SIZE;
typedef BOOLEAN E_BOOL;

#define E_TRUE   TRUE
#define E_FALSE  FALSE
#define E_NULL   NULL
#define E_MEMCPY(Dst, Src, Len)  CopyMem ((Dst), (Src), (Len))
#define E_MEMSET(Dst, Val, Len)  SetMem ((Dst), (Len), (Val))

#endif /* ERASE_HOST_BUILD */

/** Status codes returned by the Core layer.

    Deliberately not EFI_STATUS: the Core layer must not know about UEFI, and
    the Platform layer is the one that maps these onto EFI_STATUS. */
typedef enum {
  ERASE_OK = 0,
  ERASE_ERR_PARAM,        /**< caller passed something impossible */
  ERASE_ERR_IO,           /**< a read or write callback failed */
  ERASE_ERR_CANCELLED,    /**< the caller's cancel callback asked to stop */
  ERASE_ERR_VERIFY,       /**< the post-pass verification did not match */
  ERASE_ERR_MEDIA_CHANGED /**< media changed under us mid-erase */
} ERASE_STATUS;

/** Human-readable name for an ERASE_STATUS (ASCII, for logs and reports).
    Plain `const char *` on purpose: the EDK II flavour maps to CHAR8 anyway,
    and the host flavour has no CHAR8. */
const char *EraseStatusName (ERASE_STATUS Status);

#endif /* ERASE_CORE_PORT_H */
