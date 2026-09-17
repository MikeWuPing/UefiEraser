/** @file
  Core/Prng.h - xoshiro256** pseudo-random generator for the random passes.

  Why a hand-rolled PRNG instead of a platform RNG: UEFI offers no /dev/urandom
  and EFI_RNG_PROTOCOL is optional (not guaranteed on OVMF), so the erasure
  engine cannot depend on a platform entropy source. xoshiro256** is small,
  fast, and - decisively - seedable, which lets the host-side self-test assert
  exact byte sequences for the random passes.

  Entropy: the Platform layer mixes EFI_RNG_PROTOCOL output (when present) with
  the TSC and target-device identity into a 64-bit seed. For a multi-pass
  overwrite the quality bar is "not predictable by the drive's wear-levelling
  controller from the previous pass", not cryptographic strength.

  Reference: Blackman & Vigna, "Scrambled Linear Pseudorandom Number
  Generators" (2018).

  Copyright (c) 2026, Mike Wu. All rights reserved.
**/
#ifndef ERASE_CORE_PRNG_H
#define ERASE_CORE_PRNG_H

#include "Port.h"

typedef struct {
  E_U64 S[4];
} ERASE_PRNG;

/** Rotate left. */
static inline E_U64
ErasePrngRotl (
  E_U64 X,
  int   K
  )
{
  return (X << K) | (X >> (64 - K));
}

/** Seed the generator. Any seed is valid; splitmix64 is used to spread the
    bits so that even a low-entropy seed (e.g. all-zero) gives a good state. */
static inline void
ErasePrngSeed (
  ERASE_PRNG  *P,
  E_U64        Seed
  )
{
  E_U64 Z = Seed + 0x9E3779B97F4A7C15ULL;
  int   I;

  for (I = 0; I < 4; I++) {
    Z = (Z ^ (Z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    Z = (Z ^ (Z >> 27)) * 0x94D049BB133111EBULL;
    P->S[I] = Z ^ (Z >> 31);
    Z += 0x9E3779B97F4A7C15ULL;
  }
}

/** Next 64-bit value (xoshiro256**). */
static inline E_U64
ErasePrngNext (
  ERASE_PRNG  *P
  )
{
  E_U64 Result = ErasePrngRotl (P->S[1] * 5, 7) * 9;
  E_U64 T      = P->S[1] << 17;

  P->S[2] ^= P->S[0];
  P->S[3] ^= P->S[1];
  P->S[1] ^= P->S[2];
  P->S[0] ^= P->S[3];
  P->S[2] ^= T;
  P->S[3] = ErasePrngRotl (P->S[3], 45);

  return Result;
}

/** Fill Len bytes with the generator's stream. */
static inline void
ErasePrngBytes (
  ERASE_PRNG  *P,
  E_U8        *Out,
  E_SIZE       Len
  )
{
  E_SIZE I = 0;

  while (I < Len) {
    E_U64 V = ErasePrngNext (P);
    int   B;
    for (B = 0; B < 8 && I < Len; B++, I++) {
      Out[I] = (E_U8)(V & 0xFF);
      V    >>= 8;
    }
  }
}

#endif /* ERASE_CORE_PRNG_H */
