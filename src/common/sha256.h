/*
 * sha256.h - SHA-256 (FIPS 180-4) for image authentication
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-05
 * Target:  STM32F767ZIT6 (no HASH peripheral), host tests
 * SRS:     BSAFE-006
 *
 * Small and portable rather than fast: one context, no tables beyond the
 * 64 round constants.  BOOT hashes at most one slot per boot.
 */

#ifndef SHA256_H
#define SHA256_H

#include <stddef.h>
#include <stdint.h>

#define BOOT_SHA256_SIZE 32U

typedef struct {
    uint32_t state[8];
    uint64_t length;
    uint8_t  block[64];
    uint32_t used;
} boot_sha256_t;

void boot_sha256_init(boot_sha256_t *ctx);
void boot_sha256_update(boot_sha256_t *ctx, const void *data, size_t len);
void boot_sha256_final(boot_sha256_t *ctx, uint8_t digest[BOOT_SHA256_SIZE]);

/* One-shot helper. */
void boot_sha256(const void *data, size_t len,
                 uint8_t digest[BOOT_SHA256_SIZE]);

#endif /* SHA256_H */
