/**
 * @file    sha256.h
 * @brief   SHA-256 (FIPS 180-4) for image authentication.
 * @details Small and portable rather than fast: the F767 has no HASH
 *          peripheral and BOOT hashes at most one slot per boot.
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-05
 * @target  STM32F767ZIT6 (no HASH peripheral), host tests
 * @srs     BSAFE-006
 */

#ifndef SHA256_H
#define SHA256_H

#include <stddef.h>
#include <stdint.h>

#define BOOT_SHA256_SIZE 32U /**< Digest length in bytes. */

/**
 * @brief Streaming SHA-256 context.
 */
typedef struct {
    uint32_t state[8];  /**< Working hash H0..H7. */
    uint64_t length;    /**< Total bytes absorbed. */
    uint8_t  block[64]; /**< Pending partial block. */
    uint32_t used;      /**< Bytes used in @c block, 0..63 between calls. */
} boot_sha256_t;

/**
 * @brief Reset @p ctx to the SHA-256 initial hash value.
 * @param[out] ctx Context to initialise.
 */
void boot_sha256_init(boot_sha256_t *ctx);

/**
 * @brief Absorb @p len bytes into @p ctx.
 * @param[in,out] ctx  Initialised context.
 * @param[in]     data Input bytes.
 * @param[in]     len  Input length in bytes.
 */
void boot_sha256_update(boot_sha256_t *ctx, const void *data, size_t len);

/**
 * @brief Pad, finish and write the digest.
 * @details @p ctx must be re-initialised before reuse.
 * @param[in,out] ctx    Context to finish.
 * @param[out]    digest 32-byte big-endian digest.
 */
void boot_sha256_final(boot_sha256_t *ctx, uint8_t digest[BOOT_SHA256_SIZE]);

/**
 * @brief Hash a buffer in one call.
 * @param[in]  data   Input bytes.
 * @param[in]  len    Input length in bytes.
 * @param[out] digest 32-byte big-endian digest.
 */
void boot_sha256(const void *data, size_t len,
                 uint8_t digest[BOOT_SHA256_SIZE]);

#endif /* SHA256_H */
