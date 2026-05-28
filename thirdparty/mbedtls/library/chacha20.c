/**
 * \file chacha20.c
 *
 * \brief ChaCha20 cipher.
 *
 * \author Daniel King <damaki.gh@gmail.com>
 *
 *  Copyright The Mbed TLS Contributors
 *  SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
 */

#include "common.h"

#if defined(MBEDTLS_CHACHA20_C)

#include "mbedtls/chacha20.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/error.h"

#include <stddef.h>
#include <string.h>

#include "mbedtls/platform.h"

#if !defined(MBEDTLS_CHACHA20_ALT)

#if defined(__AVX2__) && \
    (defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64))
#include <immintrin.h>
#define MBEDTLS_CHACHA20_AVX2 1
#endif

#define ROTL32(value, amount) \
    ((uint32_t) ((value) << (amount)) | ((value) >> (32 - (amount))))

#define CHACHA20_CTR_INDEX (12U)

#define CHACHA20_BLOCK_SIZE_BYTES (4U * 16U)

#if defined(MBEDTLS_CHACHA20_AVX2)
#define CHACHA20_AVX2_BLOCKS (8U)
#define CHACHA20_AVX2_BATCH_SIZE_BYTES \
    (CHACHA20_AVX2_BLOCKS * CHACHA20_BLOCK_SIZE_BYTES)
#endif

/**
 * \brief           ChaCha20 quarter round operation.
 *
 *                  The quarter round is defined as follows (from RFC 7539):
 *                      1.  a += b; d ^= a; d <<<= 16;
 *                      2.  c += d; b ^= c; b <<<= 12;
 *                      3.  a += b; d ^= a; d <<<= 8;
 *                      4.  c += d; b ^= c; b <<<= 7;
 *
 * \param state     ChaCha20 state to modify.
 * \param a         The index of 'a' in the state.
 * \param b         The index of 'b' in the state.
 * \param c         The index of 'c' in the state.
 * \param d         The index of 'd' in the state.
 */
static inline void chacha20_quarter_round(uint32_t state[16],
                                          size_t a,
                                          size_t b,
                                          size_t c,
                                          size_t d)
{
    /* a += b; d ^= a; d <<<= 16; */
    state[a] += state[b];
    state[d] ^= state[a];
    state[d] = ROTL32(state[d], 16);

    /* c += d; b ^= c; b <<<= 12 */
    state[c] += state[d];
    state[b] ^= state[c];
    state[b] = ROTL32(state[b], 12);

    /* a += b; d ^= a; d <<<= 8; */
    state[a] += state[b];
    state[d] ^= state[a];
    state[d] = ROTL32(state[d], 8);

    /* c += d; b ^= c; b <<<= 7; */
    state[c] += state[d];
    state[b] ^= state[c];
    state[b] = ROTL32(state[b], 7);
}

/**
 * \brief           Perform the ChaCha20 inner block operation.
 *
 *                  This function performs two rounds: the column round and the
 *                  diagonal round.
 *
 * \param state     The ChaCha20 state to update.
 */
static void chacha20_inner_block(uint32_t state[16])
{
    chacha20_quarter_round(state, 0, 4, 8,  12);
    chacha20_quarter_round(state, 1, 5, 9,  13);
    chacha20_quarter_round(state, 2, 6, 10, 14);
    chacha20_quarter_round(state, 3, 7, 11, 15);

    chacha20_quarter_round(state, 0, 5, 10, 15);
    chacha20_quarter_round(state, 1, 6, 11, 12);
    chacha20_quarter_round(state, 2, 7, 8,  13);
    chacha20_quarter_round(state, 3, 4, 9,  14);
}

/**
 * \brief               Generates a keystream block.
 *
 * \param initial_state The initial ChaCha20 state (key, nonce, counter).
 * \param keystream     Generated keystream bytes are written to this buffer.
 */
static void chacha20_block(const uint32_t initial_state[16],
                           unsigned char keystream[64])
{
    uint32_t working_state[16];
    size_t i;

    memcpy(working_state,
           initial_state,
           CHACHA20_BLOCK_SIZE_BYTES);

    for (i = 0U; i < 10U; i++) {
        chacha20_inner_block(working_state);
    }

    working_state[0] += initial_state[0];
    working_state[1] += initial_state[1];
    working_state[2] += initial_state[2];
    working_state[3] += initial_state[3];
    working_state[4] += initial_state[4];
    working_state[5] += initial_state[5];
    working_state[6] += initial_state[6];
    working_state[7] += initial_state[7];
    working_state[8] += initial_state[8];
    working_state[9] += initial_state[9];
    working_state[10] += initial_state[10];
    working_state[11] += initial_state[11];
    working_state[12] += initial_state[12];
    working_state[13] += initial_state[13];
    working_state[14] += initial_state[14];
    working_state[15] += initial_state[15];

    for (i = 0U; i < 16; i++) {
        size_t offset = i * 4U;

        MBEDTLS_PUT_UINT32_LE(working_state[i], keystream, offset);
    }

    mbedtls_platform_zeroize(working_state, sizeof(working_state));
}

#if defined(MBEDTLS_CHACHA20_AVX2)

static inline __m256i chacha20_avx2_set1_u32(uint32_t value)
{
    return _mm256_set1_epi32((int) value);
}

#define CHACHA20_AVX2_ROTL32(value, amount)                                  \
    _mm256_or_si256(_mm256_slli_epi32((value), (amount)),                    \
                    _mm256_srli_epi32((value), 32 - (amount)))

#define CHACHA20_AVX2_QUARTER_ROUND(a, b, c, d)  \
    do                                            \
    {                                             \
        (a) = _mm256_add_epi32((a), (b));         \
        (d) = _mm256_xor_si256((d), (a));         \
        (d) = CHACHA20_AVX2_ROTL32((d), 16);      \
        (c) = _mm256_add_epi32((c), (d));         \
        (b) = _mm256_xor_si256((b), (c));         \
        (b) = CHACHA20_AVX2_ROTL32((b), 12);      \
        (a) = _mm256_add_epi32((a), (b));         \
        (d) = _mm256_xor_si256((d), (a));         \
        (d) = CHACHA20_AVX2_ROTL32((d), 8);       \
        (c) = _mm256_add_epi32((c), (d));         \
        (b) = _mm256_xor_si256((b), (c));         \
        (b) = CHACHA20_AVX2_ROTL32((b), 7);       \
    } while (0)

static void chacha20_block_x8_avx2(const uint32_t initial_state[16],
                                   unsigned char keystream[CHACHA20_AVX2_BATCH_SIZE_BYTES])
{
    __m256i state0, state1, state2, state3;
    __m256i state4, state5, state6, state7;
    __m256i state8, state9, state10, state11;
    __m256i state12, state13, state14, state15;
    __m256i x0, x1, x2, x3;
    __m256i x4, x5, x6, x7;
    __m256i x8, x9, x10, x11;
    __m256i x12, x13, x14, x15;
    uint32_t words[16][CHACHA20_AVX2_BLOCKS];
    size_t block;
    size_t word;
    size_t round;

    state0 = chacha20_avx2_set1_u32(initial_state[0]);
    state1 = chacha20_avx2_set1_u32(initial_state[1]);
    state2 = chacha20_avx2_set1_u32(initial_state[2]);
    state3 = chacha20_avx2_set1_u32(initial_state[3]);
    state4 = chacha20_avx2_set1_u32(initial_state[4]);
    state5 = chacha20_avx2_set1_u32(initial_state[5]);
    state6 = chacha20_avx2_set1_u32(initial_state[6]);
    state7 = chacha20_avx2_set1_u32(initial_state[7]);
    state8 = chacha20_avx2_set1_u32(initial_state[8]);
    state9 = chacha20_avx2_set1_u32(initial_state[9]);
    state10 = chacha20_avx2_set1_u32(initial_state[10]);
    state11 = chacha20_avx2_set1_u32(initial_state[11]);
    state12 = _mm256_add_epi32(
        chacha20_avx2_set1_u32(initial_state[12]),
        _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7));
    state13 = chacha20_avx2_set1_u32(initial_state[13]);
    state14 = chacha20_avx2_set1_u32(initial_state[14]);
    state15 = chacha20_avx2_set1_u32(initial_state[15]);

    x0 = state0;
    x1 = state1;
    x2 = state2;
    x3 = state3;
    x4 = state4;
    x5 = state5;
    x6 = state6;
    x7 = state7;
    x8 = state8;
    x9 = state9;
    x10 = state10;
    x11 = state11;
    x12 = state12;
    x13 = state13;
    x14 = state14;
    x15 = state15;

    for (round = 0U; round < 10U; round++) {
        CHACHA20_AVX2_QUARTER_ROUND(x0, x4, x8,  x12);
        CHACHA20_AVX2_QUARTER_ROUND(x1, x5, x9,  x13);
        CHACHA20_AVX2_QUARTER_ROUND(x2, x6, x10, x14);
        CHACHA20_AVX2_QUARTER_ROUND(x3, x7, x11, x15);

        CHACHA20_AVX2_QUARTER_ROUND(x0, x5, x10, x15);
        CHACHA20_AVX2_QUARTER_ROUND(x1, x6, x11, x12);
        CHACHA20_AVX2_QUARTER_ROUND(x2, x7, x8,  x13);
        CHACHA20_AVX2_QUARTER_ROUND(x3, x4, x9,  x14);
    }

    x0 = _mm256_add_epi32(x0, state0);
    x1 = _mm256_add_epi32(x1, state1);
    x2 = _mm256_add_epi32(x2, state2);
    x3 = _mm256_add_epi32(x3, state3);
    x4 = _mm256_add_epi32(x4, state4);
    x5 = _mm256_add_epi32(x5, state5);
    x6 = _mm256_add_epi32(x6, state6);
    x7 = _mm256_add_epi32(x7, state7);
    x8 = _mm256_add_epi32(x8, state8);
    x9 = _mm256_add_epi32(x9, state9);
    x10 = _mm256_add_epi32(x10, state10);
    x11 = _mm256_add_epi32(x11, state11);
    x12 = _mm256_add_epi32(x12, state12);
    x13 = _mm256_add_epi32(x13, state13);
    x14 = _mm256_add_epi32(x14, state14);
    x15 = _mm256_add_epi32(x15, state15);

    _mm256_storeu_si256((__m256i *) (void *) words[0], x0);
    _mm256_storeu_si256((__m256i *) (void *) words[1], x1);
    _mm256_storeu_si256((__m256i *) (void *) words[2], x2);
    _mm256_storeu_si256((__m256i *) (void *) words[3], x3);
    _mm256_storeu_si256((__m256i *) (void *) words[4], x4);
    _mm256_storeu_si256((__m256i *) (void *) words[5], x5);
    _mm256_storeu_si256((__m256i *) (void *) words[6], x6);
    _mm256_storeu_si256((__m256i *) (void *) words[7], x7);
    _mm256_storeu_si256((__m256i *) (void *) words[8], x8);
    _mm256_storeu_si256((__m256i *) (void *) words[9], x9);
    _mm256_storeu_si256((__m256i *) (void *) words[10], x10);
    _mm256_storeu_si256((__m256i *) (void *) words[11], x11);
    _mm256_storeu_si256((__m256i *) (void *) words[12], x12);
    _mm256_storeu_si256((__m256i *) (void *) words[13], x13);
    _mm256_storeu_si256((__m256i *) (void *) words[14], x14);
    _mm256_storeu_si256((__m256i *) (void *) words[15], x15);

    for (block = 0U; block < CHACHA20_AVX2_BLOCKS; block++) {
        for (word = 0U; word < 16U; word++) {
            MBEDTLS_PUT_UINT32_LE(words[word][block], keystream,
                                  block * CHACHA20_BLOCK_SIZE_BYTES + word * 4U);
        }
    }

    mbedtls_platform_zeroize(words, sizeof(words));
    _mm256_zeroupper();
}

#endif /* MBEDTLS_CHACHA20_AVX2 */

void mbedtls_chacha20_init(mbedtls_chacha20_context *ctx)
{
    mbedtls_platform_zeroize(ctx->state, sizeof(ctx->state));
    mbedtls_platform_zeroize(ctx->keystream8, sizeof(ctx->keystream8));

    /* Initially, there's no keystream bytes available */
    ctx->keystream_bytes_used = CHACHA20_BLOCK_SIZE_BYTES;
}

void mbedtls_chacha20_free(mbedtls_chacha20_context *ctx)
{
    if (ctx != NULL) {
        mbedtls_platform_zeroize(ctx, sizeof(mbedtls_chacha20_context));
    }
}

int mbedtls_chacha20_setkey(mbedtls_chacha20_context *ctx,
                            const unsigned char key[32])
{
    /* ChaCha20 constants - the string "expand 32-byte k" */
    ctx->state[0] = 0x61707865;
    ctx->state[1] = 0x3320646e;
    ctx->state[2] = 0x79622d32;
    ctx->state[3] = 0x6b206574;

    /* Set key */
    ctx->state[4]  = MBEDTLS_GET_UINT32_LE(key, 0);
    ctx->state[5]  = MBEDTLS_GET_UINT32_LE(key, 4);
    ctx->state[6]  = MBEDTLS_GET_UINT32_LE(key, 8);
    ctx->state[7]  = MBEDTLS_GET_UINT32_LE(key, 12);
    ctx->state[8]  = MBEDTLS_GET_UINT32_LE(key, 16);
    ctx->state[9]  = MBEDTLS_GET_UINT32_LE(key, 20);
    ctx->state[10] = MBEDTLS_GET_UINT32_LE(key, 24);
    ctx->state[11] = MBEDTLS_GET_UINT32_LE(key, 28);

    return 0;
}

int mbedtls_chacha20_starts(mbedtls_chacha20_context *ctx,
                            const unsigned char nonce[12],
                            uint32_t counter)
{
    /* Counter */
    ctx->state[12] = counter;

    /* Nonce */
    ctx->state[13] = MBEDTLS_GET_UINT32_LE(nonce, 0);
    ctx->state[14] = MBEDTLS_GET_UINT32_LE(nonce, 4);
    ctx->state[15] = MBEDTLS_GET_UINT32_LE(nonce, 8);

    mbedtls_platform_zeroize(ctx->keystream8, sizeof(ctx->keystream8));

    /* Initially, there's no keystream bytes available */
    ctx->keystream_bytes_used = CHACHA20_BLOCK_SIZE_BYTES;

    return 0;
}

int mbedtls_chacha20_update(mbedtls_chacha20_context *ctx,
                            size_t size,
                            const unsigned char *input,
                            unsigned char *output)
{
    size_t offset = 0U;

    /* Use leftover keystream bytes, if available */
    while (size > 0U && ctx->keystream_bytes_used < CHACHA20_BLOCK_SIZE_BYTES) {
        output[offset] = input[offset]
                         ^ ctx->keystream8[ctx->keystream_bytes_used];

        ctx->keystream_bytes_used++;
        offset++;
        size--;
    }

#if defined(MBEDTLS_CHACHA20_AVX2)
    while (size >= CHACHA20_AVX2_BATCH_SIZE_BYTES) {
        unsigned char keystream[CHACHA20_AVX2_BATCH_SIZE_BYTES];

        chacha20_block_x8_avx2(ctx->state, keystream);
        ctx->state[CHACHA20_CTR_INDEX] += CHACHA20_AVX2_BLOCKS;

        mbedtls_xor(output + offset, input + offset, keystream,
                    CHACHA20_AVX2_BATCH_SIZE_BYTES);
        mbedtls_platform_zeroize(keystream, sizeof(keystream));

        offset += CHACHA20_AVX2_BATCH_SIZE_BYTES;
        size   -= CHACHA20_AVX2_BATCH_SIZE_BYTES;
    }
#endif /* MBEDTLS_CHACHA20_AVX2 */

    /* Process full blocks */
    while (size >= CHACHA20_BLOCK_SIZE_BYTES) {
        /* Generate new keystream block and increment counter */
        chacha20_block(ctx->state, ctx->keystream8);
        ctx->state[CHACHA20_CTR_INDEX]++;

        mbedtls_xor(output + offset, input + offset, ctx->keystream8, 64U);

        offset += CHACHA20_BLOCK_SIZE_BYTES;
        size   -= CHACHA20_BLOCK_SIZE_BYTES;
    }

    /* Last (partial) block */
    if (size > 0U) {
        /* Generate new keystream block and increment counter */
        chacha20_block(ctx->state, ctx->keystream8);
        ctx->state[CHACHA20_CTR_INDEX]++;

        mbedtls_xor(output + offset, input + offset, ctx->keystream8, size);

        ctx->keystream_bytes_used = size;

    }

    return 0;
}

int mbedtls_chacha20_crypt(const unsigned char key[32],
                           const unsigned char nonce[12],
                           uint32_t counter,
                           size_t data_len,
                           const unsigned char *input,
                           unsigned char *output)
{
    mbedtls_chacha20_context ctx;
    int ret = MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;

    mbedtls_chacha20_init(&ctx);

    ret = mbedtls_chacha20_setkey(&ctx, key);
    if (ret != 0) {
        goto cleanup;
    }

    ret = mbedtls_chacha20_starts(&ctx, nonce, counter);
    if (ret != 0) {
        goto cleanup;
    }

    ret = mbedtls_chacha20_update(&ctx, data_len, input, output);

cleanup:
    mbedtls_chacha20_free(&ctx);
    return ret;
}

#endif /* !MBEDTLS_CHACHA20_ALT */

#if defined(MBEDTLS_SELF_TEST)

static const unsigned char test_keys[2][32] =
{
    {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    },
    {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01
    }
};

static const unsigned char test_nonces[2][12] =
{
    {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00
    },
    {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x02
    }
};

static const uint32_t test_counters[2] =
{
    0U,
    1U
};

static const unsigned char test_input[2][375] =
{
    {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    },
    {
        0x41, 0x6e, 0x79, 0x20, 0x73, 0x75, 0x62, 0x6d,
        0x69, 0x73, 0x73, 0x69, 0x6f, 0x6e, 0x20, 0x74,
        0x6f, 0x20, 0x74, 0x68, 0x65, 0x20, 0x49, 0x45,
        0x54, 0x46, 0x20, 0x69, 0x6e, 0x74, 0x65, 0x6e,
        0x64, 0x65, 0x64, 0x20, 0x62, 0x79, 0x20, 0x74,
        0x68, 0x65, 0x20, 0x43, 0x6f, 0x6e, 0x74, 0x72,
        0x69, 0x62, 0x75, 0x74, 0x6f, 0x72, 0x20, 0x66,
        0x6f, 0x72, 0x20, 0x70, 0x75, 0x62, 0x6c, 0x69,
        0x63, 0x61, 0x74, 0x69, 0x6f, 0x6e, 0x20, 0x61,
        0x73, 0x20, 0x61, 0x6c, 0x6c, 0x20, 0x6f, 0x72,
        0x20, 0x70, 0x61, 0x72, 0x74, 0x20, 0x6f, 0x66,
        0x20, 0x61, 0x6e, 0x20, 0x49, 0x45, 0x54, 0x46,
        0x20, 0x49, 0x6e, 0x74, 0x65, 0x72, 0x6e, 0x65,
        0x74, 0x2d, 0x44, 0x72, 0x61, 0x66, 0x74, 0x20,
        0x6f, 0x72, 0x20, 0x52, 0x46, 0x43, 0x20, 0x61,
        0x6e, 0x64, 0x20, 0x61, 0x6e, 0x79, 0x20, 0x73,
        0x74, 0x61, 0x74, 0x65, 0x6d, 0x65, 0x6e, 0x74,
        0x20, 0x6d, 0x61, 0x64, 0x65, 0x20, 0x77, 0x69,
        0x74, 0x68, 0x69, 0x6e, 0x20, 0x74, 0x68, 0x65,
        0x20, 0x63, 0x6f, 0x6e, 0x74, 0x65, 0x78, 0x74,
        0x20, 0x6f, 0x66, 0x20, 0x61, 0x6e, 0x20, 0x49,
        0x45, 0x54, 0x46, 0x20, 0x61, 0x63, 0x74, 0x69,
        0x76, 0x69, 0x74, 0x79, 0x20, 0x69, 0x73, 0x20,
        0x63, 0x6f, 0x6e, 0x73, 0x69, 0x64, 0x65, 0x72,
        0x65, 0x64, 0x20, 0x61, 0x6e, 0x20, 0x22, 0x49,
        0x45, 0x54, 0x46, 0x20, 0x43, 0x6f, 0x6e, 0x74,
        0x72, 0x69, 0x62, 0x75, 0x74, 0x69, 0x6f, 0x6e,
        0x22, 0x2e, 0x20, 0x53, 0x75, 0x63, 0x68, 0x20,
        0x73, 0x74, 0x61, 0x74, 0x65, 0x6d, 0x65, 0x6e,
        0x74, 0x73, 0x20, 0x69, 0x6e, 0x63, 0x6c, 0x75,
        0x64, 0x65, 0x20, 0x6f, 0x72, 0x61, 0x6c, 0x20,
        0x73, 0x74, 0x61, 0x74, 0x65, 0x6d, 0x65, 0x6e,
        0x74, 0x73, 0x20, 0x69, 0x6e, 0x20, 0x49, 0x45,
        0x54, 0x46, 0x20, 0x73, 0x65, 0x73, 0x73, 0x69,
        0x6f, 0x6e, 0x73, 0x2c, 0x20, 0x61, 0x73, 0x20,
        0x77, 0x65, 0x6c, 0x6c, 0x20, 0x61, 0x73, 0x20,
        0x77, 0x72, 0x69, 0x74, 0x74, 0x65, 0x6e, 0x20,
        0x61, 0x6e, 0x64, 0x20, 0x65, 0x6c, 0x65, 0x63,
        0x74, 0x72, 0x6f, 0x6e, 0x69, 0x63, 0x20, 0x63,
        0x6f, 0x6d, 0x6d, 0x75, 0x6e, 0x69, 0x63, 0x61,
        0x74, 0x69, 0x6f, 0x6e, 0x73, 0x20, 0x6d, 0x61,
        0x64, 0x65, 0x20, 0x61, 0x74, 0x20, 0x61, 0x6e,
        0x79, 0x20, 0x74, 0x69, 0x6d, 0x65, 0x20, 0x6f,
        0x72, 0x20, 0x70, 0x6c, 0x61, 0x63, 0x65, 0x2c,
        0x20, 0x77, 0x68, 0x69, 0x63, 0x68, 0x20, 0x61,
        0x72, 0x65, 0x20, 0x61, 0x64, 0x64, 0x72, 0x65,
        0x73, 0x73, 0x65, 0x64, 0x20, 0x74, 0x6f
    }
};

static const unsigned char test_output[2][375] =
{
    {
        0x76, 0xb8, 0xe0, 0xad, 0xa0, 0xf1, 0x3d, 0x90,
        0x40, 0x5d, 0x6a, 0xe5, 0x53, 0x86, 0xbd, 0x28,
        0xbd, 0xd2, 0x19, 0xb8, 0xa0, 0x8d, 0xed, 0x1a,
        0xa8, 0x36, 0xef, 0xcc, 0x8b, 0x77, 0x0d, 0xc7,
        0xda, 0x41, 0x59, 0x7c, 0x51, 0x57, 0x48, 0x8d,
        0x77, 0x24, 0xe0, 0x3f, 0xb8, 0xd8, 0x4a, 0x37,
        0x6a, 0x43, 0xb8, 0xf4, 0x15, 0x18, 0xa1, 0x1c,
        0xc3, 0x87, 0xb6, 0x69, 0xb2, 0xee, 0x65, 0x86
    },
    {
        0xa3, 0xfb, 0xf0, 0x7d, 0xf3, 0xfa, 0x2f, 0xde,
        0x4f, 0x37, 0x6c, 0xa2, 0x3e, 0x82, 0x73, 0x70,
        0x41, 0x60, 0x5d, 0x9f, 0x4f, 0x4f, 0x57, 0xbd,
        0x8c, 0xff, 0x2c, 0x1d, 0x4b, 0x79, 0x55, 0xec,
        0x2a, 0x97, 0x94, 0x8b, 0xd3, 0x72, 0x29, 0x15,
        0xc8, 0xf3, 0xd3, 0x37, 0xf7, 0xd3, 0x70, 0x05,
        0x0e, 0x9e, 0x96, 0xd6, 0x47, 0xb7, 0xc3, 0x9f,
        0x56, 0xe0, 0x31, 0xca, 0x5e, 0xb6, 0x25, 0x0d,
        0x40, 0x42, 0xe0, 0x27, 0x85, 0xec, 0xec, 0xfa,
        0x4b, 0x4b, 0xb5, 0xe8, 0xea, 0xd0, 0x44, 0x0e,
        0x20, 0xb6, 0xe8, 0xdb, 0x09, 0xd8, 0x81, 0xa7,
        0xc6, 0x13, 0x2f, 0x42, 0x0e, 0x52, 0x79, 0x50,
        0x42, 0xbd, 0xfa, 0x77, 0x73, 0xd8, 0xa9, 0x05,
        0x14, 0x47, 0xb3, 0x29, 0x1c, 0xe1, 0x41, 0x1c,
        0x68, 0x04, 0x65, 0x55, 0x2a, 0xa6, 0xc4, 0x05,
        0xb7, 0x76, 0x4d, 0x5e, 0x87, 0xbe, 0xa8, 0x5a,
        0xd0, 0x0f, 0x84, 0x49, 0xed, 0x8f, 0x72, 0xd0,
        0xd6, 0x62, 0xab, 0x05, 0x26, 0x91, 0xca, 0x66,
        0x42, 0x4b, 0xc8, 0x6d, 0x2d, 0xf8, 0x0e, 0xa4,
        0x1f, 0x43, 0xab, 0xf9, 0x37, 0xd3, 0x25, 0x9d,
        0xc4, 0xb2, 0xd0, 0xdf, 0xb4, 0x8a, 0x6c, 0x91,
        0x39, 0xdd, 0xd7, 0xf7, 0x69, 0x66, 0xe9, 0x28,
        0xe6, 0x35, 0x55, 0x3b, 0xa7, 0x6c, 0x5c, 0x87,
        0x9d, 0x7b, 0x35, 0xd4, 0x9e, 0xb2, 0xe6, 0x2b,
        0x08, 0x71, 0xcd, 0xac, 0x63, 0x89, 0x39, 0xe2,
        0x5e, 0x8a, 0x1e, 0x0e, 0xf9, 0xd5, 0x28, 0x0f,
        0xa8, 0xca, 0x32, 0x8b, 0x35, 0x1c, 0x3c, 0x76,
        0x59, 0x89, 0xcb, 0xcf, 0x3d, 0xaa, 0x8b, 0x6c,
        0xcc, 0x3a, 0xaf, 0x9f, 0x39, 0x79, 0xc9, 0x2b,
        0x37, 0x20, 0xfc, 0x88, 0xdc, 0x95, 0xed, 0x84,
        0xa1, 0xbe, 0x05, 0x9c, 0x64, 0x99, 0xb9, 0xfd,
        0xa2, 0x36, 0xe7, 0xe8, 0x18, 0xb0, 0x4b, 0x0b,
        0xc3, 0x9c, 0x1e, 0x87, 0x6b, 0x19, 0x3b, 0xfe,
        0x55, 0x69, 0x75, 0x3f, 0x88, 0x12, 0x8c, 0xc0,
        0x8a, 0xaa, 0x9b, 0x63, 0xd1, 0xa1, 0x6f, 0x80,
        0xef, 0x25, 0x54, 0xd7, 0x18, 0x9c, 0x41, 0x1f,
        0x58, 0x69, 0xca, 0x52, 0xc5, 0xb8, 0x3f, 0xa3,
        0x6f, 0xf2, 0x16, 0xb9, 0xc1, 0xd3, 0x00, 0x62,
        0xbe, 0xbc, 0xfd, 0x2d, 0xc5, 0xbc, 0xe0, 0x91,
        0x19, 0x34, 0xfd, 0xa7, 0x9a, 0x86, 0xf6, 0xe6,
        0x98, 0xce, 0xd7, 0x59, 0xc3, 0xff, 0x9b, 0x64,
        0x77, 0x33, 0x8f, 0x3d, 0xa4, 0xf9, 0xcd, 0x85,
        0x14, 0xea, 0x99, 0x82, 0xcc, 0xaf, 0xb3, 0x41,
        0xb2, 0x38, 0x4d, 0xd9, 0x02, 0xf3, 0xd1, 0xab,
        0x7a, 0xc6, 0x1d, 0xd2, 0x9c, 0x6f, 0x21, 0xba,
        0x5b, 0x86, 0x2f, 0x37, 0x30, 0xe3, 0x7c, 0xfd,
        0xc4, 0xfd, 0x80, 0x6c, 0x22, 0xf2, 0x21
    }
};

static const size_t test_lengths[2] =
{
    64U,
    375U
};

#if defined(MBEDTLS_CHACHA20_AVX2)
static int chacha20_avx2_self_test(void)
{
    uint32_t state[16];
    unsigned char avx2_output[CHACHA20_AVX2_BATCH_SIZE_BYTES];
    unsigned char scalar_output[CHACHA20_AVX2_BATCH_SIZE_BYTES];
    size_t i;
    int ret;

    state[0] = 0x61707865;
    state[1] = 0x3320646e;
    state[2] = 0x79622d32;
    state[3] = 0x6b206574;

    state[4]  = MBEDTLS_GET_UINT32_LE(test_keys[1], 0);
    state[5]  = MBEDTLS_GET_UINT32_LE(test_keys[1], 4);
    state[6]  = MBEDTLS_GET_UINT32_LE(test_keys[1], 8);
    state[7]  = MBEDTLS_GET_UINT32_LE(test_keys[1], 12);
    state[8]  = MBEDTLS_GET_UINT32_LE(test_keys[1], 16);
    state[9]  = MBEDTLS_GET_UINT32_LE(test_keys[1], 20);
    state[10] = MBEDTLS_GET_UINT32_LE(test_keys[1], 24);
    state[11] = MBEDTLS_GET_UINT32_LE(test_keys[1], 28);
    state[12] = 0xFFFFFFFCU;
    state[13] = MBEDTLS_GET_UINT32_LE(test_nonces[1], 0);
    state[14] = MBEDTLS_GET_UINT32_LE(test_nonces[1], 4);
    state[15] = MBEDTLS_GET_UINT32_LE(test_nonces[1], 8);

    chacha20_block_x8_avx2(state, avx2_output);

    for (i = 0U; i < CHACHA20_AVX2_BLOCKS; i++) {
        uint32_t scalar_state[16];

        memcpy(scalar_state, state, sizeof(scalar_state));
        scalar_state[CHACHA20_CTR_INDEX] += (uint32_t) i;
        chacha20_block(scalar_state,
                       scalar_output + i * CHACHA20_BLOCK_SIZE_BYTES);
    }

    ret = (memcmp(avx2_output, scalar_output, sizeof(avx2_output)) == 0) ? 0 : -1;

    mbedtls_platform_zeroize(state, sizeof(state));
    mbedtls_platform_zeroize(avx2_output, sizeof(avx2_output));
    mbedtls_platform_zeroize(scalar_output, sizeof(scalar_output));

    return ret;
}
#endif /* MBEDTLS_CHACHA20_AVX2 */

/* Make sure no other definition is already present. */
#undef ASSERT

#define ASSERT(cond, args)            \
    do                                  \
    {                                   \
        if (!(cond))                \
        {                               \
            if (verbose != 0)          \
            mbedtls_printf args;    \
                                        \
            return -1;               \
        }                               \
    }                                   \
    while (0)

int mbedtls_chacha20_self_test(int verbose)
{
    unsigned char output[381];
    unsigned i;
    int ret = MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;

    for (i = 0U; i < 2U; i++) {
        if (verbose != 0) {
            mbedtls_printf("  ChaCha20 test %u ", i);
        }

        ret = mbedtls_chacha20_crypt(test_keys[i],
                                     test_nonces[i],
                                     test_counters[i],
                                     test_lengths[i],
                                     test_input[i],
                                     output);

        ASSERT(0 == ret, ("error code: %i\n", ret));

        ASSERT(0 == memcmp(output, test_output[i], test_lengths[i]),
               ("failed (output)\n"));

        if (verbose != 0) {
            mbedtls_printf("passed\n");
        }
    }

#if defined(MBEDTLS_CHACHA20_AVX2)
    if (verbose != 0) {
        mbedtls_printf("  ChaCha20 AVX2 test ");
    }

    ret = chacha20_avx2_self_test();
    ASSERT(0 == ret, ("failed (AVX2 scalar equivalence)\n"));

    if (verbose != 0) {
        mbedtls_printf("passed\n");
    }
#endif /* MBEDTLS_CHACHA20_AVX2 */

    if (verbose != 0) {
        mbedtls_printf("\n");
    }

    return 0;
}

#endif /* MBEDTLS_SELF_TEST */

#endif /* !MBEDTLS_CHACHA20_C */
