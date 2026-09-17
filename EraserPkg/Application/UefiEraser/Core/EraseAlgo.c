/** @file
  Core/EraseAlgo.c - the algorithm table and the pass filler.

  Every pass sequence below was checked line by line against Eraser's sources
  (snapshot in docs/reference/eraser/). Several byte values differ from what
  the popular write-ups say, and Eraser is what this project reproduces:

    - Schneier starts 0x01, 0x00 (not 0xFF, 0x00)
    - German VSITR alternates 0x00 / 0x01 (not 0x00 / 0xFF)
    - RCMP TSSIT OPS-II alternates 0x00 / 0x01 and ends on ONE random byte
      repeated, not a random stream
    - British HMG IS5 (Enhanced) is 0x00, 0x01, random (second pass 0x01)
    - US DoD 5220.22-M (ECE) draws three random bytes R1/R2/R3 and uses their
      complements, giving  R1 ~R1 rand R2 R3 ~R3 rand  (7 passes)

  One DELIBERATE deviation: Eraser sets RandomizePasses = true for Gutmann and
  shuffles the 35 passes before running them. Gutmann's 27 patterns are ordered
  on purpose (each targets a specific encoding), so shuffling defeats the
  algorithm's intent. This implementation runs them in Gutmann's published
  order and does not shuffle. To match Eraser exactly instead, shuffle
  kGutmann[] before use.

  Copyright (c) 2026, Mike Wu. All rights reserved.
**/

#include "EraseAlgo.h"

#define ARRAY_SIZE_ALGO(a)  ((E_U32)(sizeof (a) / sizeof ((a)[0])))

const char *
EraseStatusName (
  ERASE_STATUS  Status
  )
{
  switch (Status) {
    case ERASE_OK:                 return "OK";
    case ERASE_ERR_PARAM:          return "bad parameter";
    case ERASE_ERR_IO:             return "I/O error";
    case ERASE_ERR_CANCELLED:      return "cancelled";
    case ERASE_ERR_VERIFY:         return "verify mismatch";
    case ERASE_ERR_MEDIA_CHANGED:  return "media changed";
    default:                       return "unknown";
  }
}

/* ------------------------------------------------------------------ passes */

/* US DoD 5220.22-M (8-306./E): 00, FF, random */
static const ERASE_PASS kDodE[] = {
  { PASS_CONSTANT, { 0x00 },       1, 0, E_FALSE },
  { PASS_CONSTANT, { 0xFF },       1, 0, E_FALSE },
  { PASS_RANDOM,   { 0 },          0, 0, E_FALSE },
};

/* US DoD 5220.22-M (8-306./E, C & E): R1, ~R1, random, R2, R3, ~R3, random.
   Eraser draws one 32-bit random and slices it into three bytes; the byte
   order (low, middle, high) is preserved here via slots 0/1/2. */
static const ERASE_PASS kDodEce[] = {
  { PASS_CONST_RANDOM, { 0 }, 0, 0, E_FALSE },
  { PASS_CONST_RANDOM, { 0 }, 0, 0, E_TRUE  },
  { PASS_RANDOM,       { 0 }, 0, 0, E_FALSE },
  { PASS_CONST_RANDOM, { 0 }, 0, 1, E_FALSE },
  { PASS_CONST_RANDOM, { 0 }, 0, 2, E_FALSE },
  { PASS_CONST_RANDOM, { 0 }, 0, 2, E_TRUE  },
  { PASS_RANDOM,       { 0 }, 0, 0, E_FALSE },
};

/* Gutmann, 35 passes, in the published order. */
static const ERASE_PASS kGutmann[] = {
  { PASS_RANDOM,       { 0 }, 0, 0, E_FALSE },  /*  1 */
  { PASS_RANDOM,       { 0 }, 0, 0, E_FALSE },  /*  2 */
  { PASS_RANDOM,       { 0 }, 0, 0, E_FALSE },  /*  3 */
  { PASS_RANDOM,       { 0 }, 0, 0, E_FALSE },  /*  4 */
  { PASS_CONSTANT, { 0x55 },             1, 0, E_FALSE },  /*  5 */
  { PASS_CONSTANT, { 0xAA },             1, 0, E_FALSE },  /*  6 */
  { PASS_CONSTANT, { 0x92, 0x49, 0x24 }, 3, 0, E_FALSE },  /*  7 */
  { PASS_CONSTANT, { 0x49, 0x24, 0x92 }, 3, 0, E_FALSE },  /*  8 */
  { PASS_CONSTANT, { 0x24, 0x92, 0x49 }, 3, 0, E_FALSE },  /*  9 */
  { PASS_CONSTANT, { 0x00 },             1, 0, E_FALSE },  /* 10 */
  { PASS_CONSTANT, { 0x11 },             1, 0, E_FALSE },  /* 11 */
  { PASS_CONSTANT, { 0x22 },             1, 0, E_FALSE },  /* 12 */
  { PASS_CONSTANT, { 0x33 },             1, 0, E_FALSE },  /* 13 */
  { PASS_CONSTANT, { 0x44 },             1, 0, E_FALSE },  /* 14 */
  { PASS_CONSTANT, { 0x55 },             1, 0, E_FALSE },  /* 15 */
  { PASS_CONSTANT, { 0x66 },             1, 0, E_FALSE },  /* 16 */
  { PASS_CONSTANT, { 0x77 },             1, 0, E_FALSE },  /* 17 */
  { PASS_CONSTANT, { 0x88 },             1, 0, E_FALSE },  /* 18 */
  { PASS_CONSTANT, { 0x99 },             1, 0, E_FALSE },  /* 19 */
  { PASS_CONSTANT, { 0xAA },             1, 0, E_FALSE },  /* 20 */
  { PASS_CONSTANT, { 0xBB },             1, 0, E_FALSE },  /* 21 */
  { PASS_CONSTANT, { 0xCC },             1, 0, E_FALSE },  /* 22 */
  { PASS_CONSTANT, { 0xDD },             1, 0, E_FALSE },  /* 23 */
  { PASS_CONSTANT, { 0xEE },             1, 0, E_FALSE },  /* 24 */
  { PASS_CONSTANT, { 0xFF },             1, 0, E_FALSE },  /* 25 */
  { PASS_CONSTANT, { 0x92, 0x49, 0x24 }, 3, 0, E_FALSE },  /* 26 */
  { PASS_CONSTANT, { 0x49, 0x24, 0x92 }, 3, 0, E_FALSE },  /* 27 */
  { PASS_CONSTANT, { 0x24, 0x92, 0x49 }, 3, 0, E_FALSE },  /* 28 */
  { PASS_CONSTANT, { 0x6D, 0xB6, 0xDB }, 3, 0, E_FALSE },  /* 29 */
  { PASS_CONSTANT, { 0xB6, 0xDB, 0x6D }, 3, 0, E_FALSE },  /* 30 */
  { PASS_CONSTANT, { 0xDB, 0x6D, 0xB6 }, 3, 0, E_FALSE },  /* 31 */
  { PASS_RANDOM,       { 0 }, 0, 0, E_FALSE },  /* 32 */
  { PASS_RANDOM,       { 0 }, 0, 0, E_FALSE },  /* 33 */
  { PASS_RANDOM,       { 0 }, 0, 0, E_FALSE },  /* 34 */
  { PASS_RANDOM,       { 0 }, 0, 0, E_FALSE },  /* 35 */
};

/* Schneier 7 pass: 01, 00, random x5 */
static const ERASE_PASS kSchneier[] = {
  { PASS_CONSTANT, { 0x01 }, 1, 0, E_FALSE },
  { PASS_CONSTANT, { 0x00 }, 1, 0, E_FALSE },
  { PASS_RANDOM,   { 0 },    0, 0, E_FALSE },
  { PASS_RANDOM,   { 0 },    0, 0, E_FALSE },
  { PASS_RANDOM,   { 0 },    0, 0, E_FALSE },
  { PASS_RANDOM,   { 0 },    0, 0, E_FALSE },
  { PASS_RANDOM,   { 0 },    0, 0, E_FALSE },
};

/* British HMG IS5 (Baseline): single zero pass */
static const ERASE_PASS kHmgBaseline[] = {
  { PASS_CONSTANT, { 0x00 }, 1, 0, E_FALSE },
};

/* British HMG IS5 (Enhanced): 00, 01, random */
static const ERASE_PASS kHmgEnhanced[] = {
  { PASS_CONSTANT, { 0x00 }, 1, 0, E_FALSE },
  { PASS_CONSTANT, { 0x01 }, 1, 0, E_FALSE },
  { PASS_RANDOM,   { 0 },    0, 0, E_FALSE },
};

/* RCMP TSSIT OPS-II: 00,01 x3 then ONE random byte repeated */
static const ERASE_PASS kRcmp[] = {
  { PASS_CONSTANT,     { 0x00 }, 1, 0, E_FALSE },
  { PASS_CONSTANT,     { 0x01 }, 1, 0, E_FALSE },
  { PASS_CONSTANT,     { 0x00 }, 1, 0, E_FALSE },
  { PASS_CONSTANT,     { 0x01 }, 1, 0, E_FALSE },
  { PASS_CONSTANT,     { 0x00 }, 1, 0, E_FALSE },
  { PASS_CONSTANT,     { 0x01 }, 1, 0, E_FALSE },
  { PASS_CONST_RANDOM, { 0 },    0, 0, E_FALSE },
};

/* German VSITR: 00,01 x3 then a random stream */
static const ERASE_PASS kVsitr[] = {
  { PASS_CONSTANT, { 0x00 }, 1, 0, E_FALSE },
  { PASS_CONSTANT, { 0x01 }, 1, 0, E_FALSE },
  { PASS_CONSTANT, { 0x00 }, 1, 0, E_FALSE },
  { PASS_CONSTANT, { 0x01 }, 1, 0, E_FALSE },
  { PASS_CONSTANT, { 0x00 }, 1, 0, E_FALSE },
  { PASS_CONSTANT, { 0x01 }, 1, 0, E_FALSE },
  { PASS_RANDOM,   { 0 },    0, 0, E_FALSE },
};

/* Russian GOST P50739-95: 00, random */
static const ERASE_PASS kGost[] = {
  { PASS_CONSTANT, { 0x00 }, 1, 0, E_FALSE },
  { PASS_RANDOM,   { 0 },    0, 0, E_FALSE },
};

/* US Army AR 380-19: random stream, then ONE random byte, then its NOT */
static const ERASE_PASS kAr38019[] = {
  { PASS_RANDOM,       { 0 }, 0, 0, E_FALSE },
  { PASS_CONST_RANDOM, { 0 }, 0, 0, E_FALSE },
  { PASS_CONST_RANDOM, { 0 }, 0, 0, E_TRUE  },
};

/* US Air Force 5020 (NAVSO P-5239-26): three distinct random bytes */
static const ERASE_PASS kUsaf5020[] = {
  { PASS_CONST_RANDOM, { 0 }, 0, 0, E_FALSE },
  { PASS_CONST_RANDOM, { 0 }, 0, 1, E_FALSE },
  { PASS_CONST_RANDOM, { 0 }, 0, 2, E_FALSE },
};

/* Single random stream */
static const ERASE_PASS kRandom[] = {
  { PASS_RANDOM, { 0 }, 0, 0, E_FALSE },
};

/* ------------------------------------------------------------------ table */

static const ERASE_ALGORITHM kAlgorithms[] = {
  {
    "random", "伪随机数据", "Pseudorandom Data",
    kRandom, ARRAY_SIZE_ALGO (kRandom), E_FALSE
  },
  {
    "dod522022m", "美国国防部 DoD 5220.22-M", "US DoD 5220.22-M",
    kDodE, ARRAY_SIZE_ALGO (kDodE), E_TRUE
  },
  {
    "dod522022m_ece", "DoD 5220.22-M (ECE)", "US DoD 5220.22-M (ECE)",
    kDodEce, ARRAY_SIZE_ALGO (kDodEce), E_TRUE
  },
  {
    "gutmann", "Gutmann 35 遍", "Gutmann",
    kGutmann, ARRAY_SIZE_ALGO (kGutmann), E_FALSE
  },
  {
    "schneier", "Schneier 7 遍", "Schneier 7 pass",
    kSchneier, ARRAY_SIZE_ALGO (kSchneier), E_FALSE
  },
  {
    "hmg_is5_baseline", "英国 HMG IS5（基础）", "British HMG IS5 (Baseline)",
    kHmgBaseline, ARRAY_SIZE_ALGO (kHmgBaseline), E_FALSE
  },
  {
    "hmg_is5_enhanced", "英国 HMG IS5（增强）", "British HMG IS5 (Enhanced)",
    kHmgEnhanced, ARRAY_SIZE_ALGO (kHmgEnhanced), E_TRUE
  },
  {
    "rcmp_tssit_ops2", "加拿大 RCMP TSSIT OPS-II", "RCMP TSSIT OPS-II",
    kRcmp, ARRAY_SIZE_ALGO (kRcmp), E_FALSE
  },
  {
    "vsitr", "德国 VSITR", "German VSITR",
    kVsitr, ARRAY_SIZE_ALGO (kVsitr), E_FALSE
  },
  {
    "gost_p50739", "俄罗斯 GOST P50739-95", "Russian GOST P50739-95",
    kGost, ARRAY_SIZE_ALGO (kGost), E_FALSE
  },
  {
    "ar380_19", "美国陆军 AR 380-19", "US Army AR380-19",
    kAr38019, ARRAY_SIZE_ALGO (kAr38019), E_FALSE
  },
  {
    "usaf_5020", "美国空军 5020", "US Air Force 5020",
    kUsaf5020, ARRAY_SIZE_ALGO (kUsaf5020), E_FALSE
  },
  /* PassCount 0 marks "the pass list comes from the job, not the table":
     the UI offers a pass-count spinner and the engine runs that many random
     passes. Same idea as Eraser's user-defined methods, minus the pass editor. */
  {
    "custom", "自定义（随机 N 遍）", "Custom (N random passes)",
    kRandom, 0, E_FALSE
  },
};

#define ALGO_COUNT  (sizeof (kAlgorithms) / sizeof (kAlgorithms[0]))

E_U32
EraseAlgoCount (
  VOID
  )
{
  return (E_U32)ALGO_COUNT;
}

const ERASE_ALGORITHM *
EraseAlgoAt (
  E_U32  Index
  )
{
  if (Index >= ALGO_COUNT) {
    return E_NULL;
  }
  return &kAlgorithms[Index];
}

const ERASE_ALGORITHM *
EraseAlgoById (
  const char  *Id
  )
{
  E_U32 I;

  if (Id == E_NULL) {
    return E_NULL;
  }
  for (I = 0; I < ALGO_COUNT; I++) {
    const char *A = kAlgorithms[I].Id;
    const char *B = Id;
    while (*A != 0 && *A == *B) {
      A++;
      B++;
    }
    if (*A == 0 && *B == 0) {
      return &kAlgorithms[I];
    }
  }
  return E_NULL;
}

VOID
EraseRandSlotsInit (
  ERASE_RAND_SLOTS  *Slots,
  ERASE_PRNG        *Prng
  )
{
  E_U64 V = ErasePrngNext (Prng);

  Slots->B[0] = (E_U8)(V & 0xFF);
  Slots->B[1] = (E_U8)((V >> 8) & 0xFF);
  Slots->B[2] = (E_U8)((V >> 16) & 0xFF);
}

VOID
ErasePassFill (
  const ERASE_PASS       *Pass,
  E_U8                   *Buf,
  E_SIZE                  Len,
  ERASE_PRNG             *Prng,
  const ERASE_RAND_SLOTS *Slots
  )
{
  E_SIZE I;

  switch (Pass->Type) {
    case PASS_RANDOM:
      ErasePrngBytes (Prng, Buf, Len);
      return;

    case PASS_CONSTANT:
      for (I = 0; I < Len; I++) {
        Buf[I] = Pass->Pattern[I % Pass->PatternLen];
      }
      return;

    case PASS_CONST_RANDOM:
    {
      E_U8 V = Slots->B[Pass->RandSlot % 3];
      if (Pass->Invert) {
        V = (E_U8)(~V);
      }
      E_MEMSET (Buf, V, Len);
      return;
    }

    default:
      E_MEMSET (Buf, 0, Len);
      return;
  }
}

E_SIZE
EraseAlgoDescribePasses (
  const ERASE_ALGORITHM  *Algo,
  char                   *Out,
  E_SIZE                  OutLen
  )
{
  E_SIZE N = 0;
  E_U32  P;

  if (Out == E_NULL || OutLen == 0) {
    return 0;
  }
  Out[0] = 0;
  if (Algo == E_NULL) {
    return 0;
  }
  if (Algo->PassCount == 0) {
    /* runtime-defined (custom) */
    const char *S = "N x random";
    while (*S != 0 && N + 1 < OutLen) {
      Out[N++] = *S++;
    }
    Out[N] = 0;
    return N;
  }

  for (P = 0; P < Algo->PassCount && N + 12 < OutLen; P++) {
    const ERASE_PASS *Ps = &Algo->Passes[P];

    if (P > 0 && N + 1 < OutLen) {
      Out[N++] = ' ';
    }
    /* pass ordinal */
    if (P >= 9 && N + 1 < OutLen) {
      Out[N++] = (char)('0' + (int)((P + 1) / 10));
    }
    if (N + 1 < OutLen) {
      Out[N++] = (char)('0' + (int)((P + 1) % 10));
    }
    if (N + 1 < OutLen) {
      Out[N++] = ':';
    }

    if (Ps->Type == PASS_RANDOM) {
      const char *S = "rnd";
      while (*S != 0 && N + 1 < OutLen) {
        Out[N++] = *S++;
      }
    } else if (Ps->Type == PASS_CONST_RANDOM) {
      if (Ps->Invert) {
        Out[N++] = '~';
      }
      if (N + 1 < OutLen) {
        Out[N++] = (char)('a' + (int)Ps->RandSlot);
      }
    } else {
      E_U8 K;
      for (K = 0; K < Ps->PatternLen && N + 2 < OutLen; K++) {
        static const char Hex[] = "0123456789ABCDEF";
        Out[N++] = Hex[(Ps->Pattern[K] >> 4) & 0xF];
        Out[N++] = Hex[Ps->Pattern[K] & 0xF];
      }
    }
  }
  Out[N] = 0;
  return N;
}

E_U64
EraseAlgoTotalBytes (
  const ERASE_ALGORITHM  *Algo,
  E_U64                   TargetBytes
  )
{
  E_U32 Passes;

  if (Algo == E_NULL) {
    return 0;
  }
  Passes = (Algo->PassCount != 0) ? Algo->PassCount : 1;

  /* Saturate instead of wrapping: the UI shows this as "bytes to be written"
     and a wrapped value would be actively misleading on a multi-TB target. */
  if (TargetBytes > (0xFFFFFFFFFFFFFFFFULL / Passes)) {
    return 0xFFFFFFFFFFFFFFFFULL;
  }
  return TargetBytes * Passes;
}
