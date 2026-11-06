/*
 * sha256.c - SHA-256 (FIPS 180-4) for image authentication
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-05
 * Target:  STM32F767ZIT6 (no HASH peripheral), host tests
 * SRS:     BSAFE-006
 */

#include <stdint.h>
#include <string.h>
#include "sha256.h"

static const uint32_t k[64] = {
    0x428a2f98UL, 0x71374491UL, 0xb5c0fbcfUL, 0xe9b5dba5UL,
    0x3956c25bUL, 0x59f111f1UL, 0x923f82a4UL, 0xab1c5ed5UL,
    0xd807aa98UL, 0x12835b01UL, 0x243185beUL, 0x550c7dc3UL,
    0x72be5d74UL, 0x80deb1feUL, 0x9bdc06a7UL, 0xc19bf174UL,
    0xe49b69c1UL, 0xefbe4786UL, 0x0fc19dc6UL, 0x240ca1ccUL,
    0x2de92c6fUL, 0x4a7484aaUL, 0x5cb0a9dcUL, 0x76f988daUL,
    0x983e5152UL, 0xa831c66dUL, 0xb00327c8UL, 0xbf597fc7UL,
    0xc6e00bf3UL, 0xd5a79147UL, 0x06ca6351UL, 0x14292967UL,
    0x27b70a85UL, 0x2e1b2138UL, 0x4d2c6dfcUL, 0x53380d13UL,
    0x650a7354UL, 0x766a0abbUL, 0x81c2c92eUL, 0x92722c85UL,
    0xa2bfe8a1UL, 0xa81a664bUL, 0xc24b8b70UL, 0xc76c51a3UL,
    0xd192e819UL, 0xd6990624UL, 0xf40e3585UL, 0x106aa070UL,
    0x19a4c116UL, 0x1e376c08UL, 0x2748774cUL, 0x34b0bcb5UL,
    0x391c0cb3UL, 0x4ed8aa4aUL, 0x5b9cca4fUL, 0x682e6ff3UL,
    0x748f82eeUL, 0x78a5636fUL, 0x84c87814UL, 0x8cc70208UL,
    0x90befffaUL, 0xa4506cebUL, 0xbef9a3f7UL, 0xc67178f2UL
};

static uint32_t rotr(uint32_t x, uint32_t n)
{
    return (x >> n) | (x << (32U - n));
}

static void compress(uint32_t state[8], const uint8_t block[64])
{
    uint32_t w[64];

    for (uint32_t i = 0U; i < 16U; i++) {
        w[i] = ((uint32_t)block[4U * i] << 24U) |
               ((uint32_t)block[4U * i + 1U] << 16U) |
               ((uint32_t)block[4U * i + 2U] << 8U) |
               (uint32_t)block[4U * i + 3U];
    }
    for (uint32_t i = 16U; i < 64U; i++) {
        uint32_t s0 = rotr(w[i - 15U], 7U) ^ rotr(w[i - 15U], 18U) ^
                      (w[i - 15U] >> 3U);
        uint32_t s1 = rotr(w[i - 2U], 17U) ^ rotr(w[i - 2U], 19U) ^
                      (w[i - 2U] >> 10U);
        w[i] = w[i - 16U] + s0 + w[i - 7U] + s1;
    }

    uint32_t v[8];
    memcpy(v, state, sizeof(v));
    for (uint32_t i = 0U; i < 64U; i++) {
        uint32_t s1 = rotr(v[4], 6U) ^ rotr(v[4], 11U) ^ rotr(v[4], 25U);
        uint32_t ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
        uint32_t t1 = v[7] + s1 + ch + k[i] + w[i];
        uint32_t s0 = rotr(v[0], 2U) ^ rotr(v[0], 13U) ^ rotr(v[0], 22U);
        uint32_t maj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
        uint32_t t2 = s0 + maj;

        v[7] = v[6];
        v[6] = v[5];
        v[5] = v[4];
        v[4] = v[3] + t1;
        v[3] = v[2];
        v[2] = v[1];
        v[1] = v[0];
        v[0] = t1 + t2;
    }
    for (uint32_t i = 0U; i < 8U; i++) {
        state[i] += v[i];
    }
}

void boot_sha256_init(boot_sha256_t *ctx)
{
    static const uint32_t iv[8] = {
        0x6a09e667UL, 0xbb67ae85UL, 0x3c6ef372UL, 0xa54ff53aUL,
        0x510e527fUL, 0x9b05688cUL, 0x1f83d9abUL, 0x5be0cd19UL
    };
    memcpy(ctx->state, iv, sizeof(iv));
    ctx->length = 0U;
    ctx->used   = 0U;
}

void boot_sha256_update(boot_sha256_t *ctx, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;

    ctx->length += (uint64_t)len;
    while (len > 0U) {
        size_t take = 64U - ctx->used;
        if (take > len) {
            take = len;
        }
        memcpy(&ctx->block[ctx->used], p, take);
        ctx->used += (uint32_t)take;
        p   += take;
        len -= take;
        if (ctx->used == 64U) {
            compress(ctx->state, ctx->block);
            ctx->used = 0U;
        }
    }
}

void boot_sha256_final(boot_sha256_t *ctx, uint8_t digest[BOOT_SHA256_SIZE])
{
    uint64_t bits = ctx->length * 8U;

    ctx->block[ctx->used++] = 0x80U;
    if (ctx->used > 56U) {
        memset(&ctx->block[ctx->used], 0, 64U - ctx->used);
        compress(ctx->state, ctx->block);
        ctx->used = 0U;
    }
    memset(&ctx->block[ctx->used], 0, 56U - ctx->used);
    for (uint32_t i = 0U; i < 8U; i++) {
        ctx->block[63U - i] = (uint8_t)(bits >> (8U * i));
    }
    compress(ctx->state, ctx->block);

    for (uint32_t i = 0U; i < 8U; i++) {
        digest[4U * i]      = (uint8_t)(ctx->state[i] >> 24U);
        digest[4U * i + 1U] = (uint8_t)(ctx->state[i] >> 16U);
        digest[4U * i + 2U] = (uint8_t)(ctx->state[i] >> 8U);
        digest[4U * i + 3U] = (uint8_t)ctx->state[i];
    }
}

void boot_sha256(const void *data, size_t len,
                 uint8_t digest[BOOT_SHA256_SIZE])
{
    boot_sha256_t ctx;
    boot_sha256_init(&ctx);
    boot_sha256_update(&ctx, data, len);
    boot_sha256_final(&ctx, digest);
}
