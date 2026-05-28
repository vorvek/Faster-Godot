/* adler32.c -- compute the Adler-32 checksum of a data stream
 * Copyright (C) 1995-2011, 2016 Mark Adler
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

/* @(#) $Id$ */

#include "zutil.h"

#if defined(ZLIB_ADLER32_AVX2) && (defined(__AVX2__) || defined(_MSC_VER)) && \
    (defined(__x86_64__) || defined(__x86_64) || defined(_M_X64))
#  include <immintrin.h>
#  define ADLER32_AVX2_FAST
#endif

#define BASE 65521U     /* largest prime smaller than 65536 */
#define NMAX 5552
/* NMAX is the largest n such that 255n(n+1)/2 + (n+1)(BASE-1) <= 2^32-1 */
#define AVX2_NMAX (NMAX - (NMAX % 32))

#define DO1(buf,i)  {adler += (buf)[i]; sum2 += adler;}
#define DO2(buf,i)  DO1(buf,i); DO1(buf,i+1);
#define DO4(buf,i)  DO2(buf,i); DO2(buf,i+2);
#define DO8(buf,i)  DO4(buf,i); DO4(buf,i+4);
#define DO16(buf)   DO8(buf,0); DO8(buf,8);

/* use NO_DIVIDE if your processor does not do division in hardware --
   try it both ways to see which is faster */
#ifdef NO_DIVIDE
/* note that this assumes BASE is 65521, where 65536 % 65521 == 15
   (thank you to John Reiser for pointing this out) */
#  define CHOP(a) \
    do { \
        unsigned long tmp = a >> 16; \
        a &= 0xffffUL; \
        a += (tmp << 4) - tmp; \
    } while (0)
#  define MOD28(a) \
    do { \
        CHOP(a); \
        if (a >= BASE) a -= BASE; \
    } while (0)
#  define MOD(a) \
    do { \
        CHOP(a); \
        MOD28(a); \
    } while (0)
#  define MOD63(a) \
    do { /* this assumes a is not negative */ \
        z_off64_t tmp = a >> 32; \
        a &= 0xffffffffL; \
        a += (tmp << 8) - (tmp << 5) + tmp; \
        tmp = a >> 16; \
        a &= 0xffffL; \
        a += (tmp << 4) - tmp; \
        tmp = a >> 16; \
        a &= 0xffffL; \
        a += (tmp << 4) - tmp; \
        if (a >= BASE) a -= BASE; \
    } while (0)
#else
#  define MOD(a) a %= BASE
#  define MOD28(a) a %= BASE
#  define MOD63(a) a %= BASE
#endif

/* ========================================================================= */
local uLong adler32_z_scalar(uLong adler, const Bytef *buf, z_size_t len) {
    unsigned long sum2;
    unsigned n;

    /* split Adler-32 into component sums */
    sum2 = (adler >> 16) & 0xffff;
    adler &= 0xffff;

    /* in case user likes doing a byte at a time, keep it fast */
    if (len == 1) {
        adler += buf[0];
        if (adler >= BASE)
            adler -= BASE;
        sum2 += adler;
        if (sum2 >= BASE)
            sum2 -= BASE;
        return adler | (sum2 << 16);
    }

    /* initial Adler-32 value (deferred check for len == 1 speed) */
    if (buf == Z_NULL)
        return 1L;

    /* in case short lengths are provided, keep it somewhat fast */
    if (len < 16) {
        while (len--) {
            adler += *buf++;
            sum2 += adler;
        }
        if (adler >= BASE)
            adler -= BASE;
        MOD28(sum2);            /* only added so many BASE's */
        return adler | (sum2 << 16);
    }

    /* do length NMAX blocks -- requires just one modulo operation */
    while (len >= NMAX) {
        len -= NMAX;
        n = NMAX / 16;          /* NMAX is divisible by 16 */
        do {
            DO16(buf);          /* 16 sums unrolled */
            buf += 16;
        } while (--n);
        MOD(adler);
        MOD(sum2);
    }

    /* do remaining bytes (less than NMAX, still just one modulo) */
    if (len) {                  /* avoid modulos if none remaining */
        while (len >= 16) {
            len -= 16;
            DO16(buf);
            buf += 16;
        }
        while (len--) {
            adler += *buf++;
            sum2 += adler;
        }
        MOD(adler);
        MOD(sum2);
    }

    /* return recombined sums */
    return adler | (sum2 << 16);
}

#ifdef ADLER32_AVX2_FAST
local unsigned adler32_avx2_hsum_epi64(__m256i v) {
    __m128i lo;
    __m128i hi;
    __m128i sum;

    lo = _mm256_castsi256_si128(v);
    hi = _mm256_extracti128_si256(v, 1);
    sum = _mm_add_epi64(lo, hi);
    sum = _mm_add_epi64(sum, _mm_srli_si128(sum, 8));
    return (unsigned)_mm_cvtsi128_si64(sum);
}

local unsigned adler32_avx2_hsum_epi32(__m256i v) {
    __m128i lo;
    __m128i hi;
    __m128i sum;

    lo = _mm256_castsi256_si128(v);
    hi = _mm256_extracti128_si256(v, 1);
    sum = _mm_add_epi32(lo, hi);
    sum = _mm_hadd_epi32(sum, sum);
    sum = _mm_hadd_epi32(sum, sum);
    return (unsigned)_mm_cvtsi128_si32(sum);
}

local void adler32_avx2_32(const Bytef *buf, unsigned long *adler, unsigned long *sum2) {
    const __m256i weights = _mm256_setr_epi8(
        32, 31, 30, 29, 28, 27, 26, 25,
        24, 23, 22, 21, 20, 19, 18, 17,
        16, 15, 14, 13, 12, 11, 10, 9,
        8, 7, 6, 5, 4, 3, 2, 1);
    const __m256i ones = _mm256_set1_epi16(1);
    const __m256i zero = _mm256_setzero_si256();
    __m256i bytes;
    __m256i byte_sums;
    __m256i weighted_pairs;
    __m256i weighted_sums;
    unsigned sum1_add;
    unsigned sum2_add;

    bytes = _mm256_loadu_si256((const __m256i *)(const void *)buf);
    byte_sums = _mm256_sad_epu8(bytes, zero);
    weighted_pairs = _mm256_maddubs_epi16(bytes, weights);
    weighted_sums = _mm256_madd_epi16(weighted_pairs, ones);

    sum1_add = adler32_avx2_hsum_epi64(byte_sums);
    sum2_add = adler32_avx2_hsum_epi32(weighted_sums);

    *sum2 += 32 * *adler + sum2_add;
    *adler += sum1_add;
}

local uLong adler32_z_avx2(uLong adler, const Bytef *buf, z_size_t len) {
    unsigned long sum2;
    z_size_t vector_len;
    z_size_t blocks;

    if (buf == Z_NULL || len < 128)
        return adler32_z_scalar(adler, buf, len);

    sum2 = (adler >> 16) & 0xffff;
    adler &= 0xffff;

    while (len >= 32) {
        vector_len = len < AVX2_NMAX ? (len & ~((z_size_t)31)) : AVX2_NMAX;
        blocks = vector_len >> 5;
        len -= vector_len;
        do {
            adler32_avx2_32(buf, &adler, &sum2);
            buf += 32;
        } while (--blocks);
        MOD(adler);
        MOD(sum2);
    }

    if (len)
        return adler32_z_scalar(adler | (sum2 << 16), buf, len);

    return adler | (sum2 << 16);
}
#endif

/* ========================================================================= */
uLong ZEXPORT adler32_z(uLong adler, const Bytef *buf, z_size_t len) {
#ifdef ADLER32_AVX2_FAST
    return adler32_z_avx2(adler, buf, len);
#else
    return adler32_z_scalar(adler, buf, len);
#endif
}

/* ========================================================================= */
uLong ZEXPORT adler32(uLong adler, const Bytef *buf, uInt len) {
    return adler32_z(adler, buf, len);
}

/* ========================================================================= */
local uLong adler32_combine_(uLong adler1, uLong adler2, z_off64_t len2) {
    unsigned long sum1;
    unsigned long sum2;
    unsigned rem;

    /* for negative len, return invalid adler32 as a clue for debugging */
    if (len2 < 0)
        return 0xffffffffUL;

    /* the derivation of this formula is left as an exercise for the reader */
    MOD63(len2);                /* assumes len2 >= 0 */
    rem = (unsigned)len2;
    sum1 = adler1 & 0xffff;
    sum2 = rem * sum1;
    MOD(sum2);
    sum1 += (adler2 & 0xffff) + BASE - 1;
    sum2 += ((adler1 >> 16) & 0xffff) + ((adler2 >> 16) & 0xffff) + BASE - rem;
    if (sum1 >= BASE) sum1 -= BASE;
    if (sum1 >= BASE) sum1 -= BASE;
    if (sum2 >= ((unsigned long)BASE << 1)) sum2 -= ((unsigned long)BASE << 1);
    if (sum2 >= BASE) sum2 -= BASE;
    return sum1 | (sum2 << 16);
}

/* ========================================================================= */
uLong ZEXPORT adler32_combine(uLong adler1, uLong adler2, z_off_t len2) {
    return adler32_combine_(adler1, adler2, len2);
}

uLong ZEXPORT adler32_combine64(uLong adler1, uLong adler2, z_off64_t len2) {
    return adler32_combine_(adler1, adler2, len2);
}
