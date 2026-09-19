/** @file
  Core/EraseAlgo.h - the industry-standard erasure algorithm table.

  The methods here are the published standards, not any particular program's
  implementation of them. Every pass sequence is a fact taken from the
  specification text for that method (DoD 5220.22-M, Gutmann 1996, HMG IS5,
  RCMP TSSIT OPS-II, GOST P50739-95, US Army AR 380-19, NAVSO P-5239-26).
  Where the common write-ups disagree with the specification, the
  specification wins. The C here was written for this project.

  A pass sequence is built from two primitives:
    WriteConstant(byte[] pattern)  - the pattern repeats across the buffer
    WriteRandom(prng)              - the buffer is filled from a PRNG stream

  A third case is needed for the methods that pick ONE random byte per run and
  then repeat it (RCMP's last pass, AR 380-19's 2nd/3rd, USAF 5020's three):
  that is PASS_CONST_RANDOM with a slot index, resolved once per erasure.

  Copyright (c) 2026, Mike Wu. All rights reserved.
**/
#ifndef ERASE_CORE_ERASE_ALGO_H
#define ERASE_CORE_ERASE_ALGO_H

#include "Port.h"
#include "Prng.h"

typedef enum {
  PASS_CONSTANT = 0,   /**< Pattern (PatternLen bytes) repeats across the buffer */
  PASS_RANDOM,         /**< filled from the PRNG stream */
  PASS_CONST_RANDOM    /**< one byte drawn per run (RandSlot), then repeated */
} ERASE_PASS_TYPE;

typedef struct {
  ERASE_PASS_TYPE Type;
  E_U8            Pattern[3];   /**< PASS_CONSTANT only */
  E_U8            PatternLen;   /**< 1..3 */
  E_U8            RandSlot;     /**< PASS_CONST_RANDOM: 0..2 */
  E_BOOL          Invert;       /**< PASS_CONST_RANDOM: use the bitwise NOT */
} ERASE_PASS;

typedef struct {
  const char        *Id;        /**< stable ASCII id, e.g. "dod522022m" */
  const char        *NameZh;    /**< UTF-8 display name (UI) */
  const char        *NameEn;    /**< ASCII name (logs, reports) */
  const ERASE_PASS  *Passes;
  E_U32              PassCount;
  E_BOOL             VerifyDefault;  /**< sensible default for the verify option */
} ERASE_ALGORITHM;

/** Number of algorithms in the table. */
E_U32
EraseAlgoCount (
  VOID
  );

/** Algorithm by index (0..EraseAlgoCount()-1), or NULL if out of range. */
const ERASE_ALGORITHM *
EraseAlgoAt (
  E_U32  Index
  );

/** Algorithm by stable id, or NULL if unknown. */
const ERASE_ALGORITHM *
EraseAlgoById (
  const char  *Id
  );

/** The bytes drawn once per erasure for the PASS_CONST_RANDOM slots. The
    engine fills this before the first pass; `Salt` keeps two erasures on the
    same media from producing identical constants. */
typedef struct {
  E_U8 B[3];
} ERASE_RAND_SLOTS;

VOID
EraseRandSlotsInit (
  ERASE_RAND_SLOTS  *Slots,
  ERASE_PRNG        *Prng
  );

/** Fill `Len` bytes of `Buf` for one pass.

    For PASS_RANDOM the PRNG stream advances; the caller is expected to reuse
    the same buffer across the whole pass (which is what Eraser does and what
    keeps the write pattern identical). */
VOID
ErasePassFill (
  const ERASE_PASS  *Pass,
  E_U8              *Buf,
  E_SIZE             Len,
  ERASE_PRNG        *Prng,
  const ERASE_RAND_SLOTS *Slots
  );

/** Render the pass list as a compact ASCII summary, e.g.
    "1:00 2:FF 3:random" - shown in the UI next to the algorithm name and
    written into the report. Returns the number of characters written
    (excluding the terminator); always NUL-terminates when OutLen > 0. */
E_SIZE
EraseAlgoDescribePasses (
  const ERASE_ALGORITHM  *Algo,
  char                   *Out,
  E_SIZE                  OutLen
  );

/** Total bytes written for one erasure of `TargetBytes` with this algorithm
    (i.e. TargetBytes * PassCount). Saturates rather than overflowing. */
E_U64
EraseAlgoTotalBytes (
  const ERASE_ALGORITHM  *Algo,
  E_U64                   TargetBytes
  );

#endif /* ERASE_CORE_ERASE_ALGO_H */
