/**
 * @file    signing_key.h
 * @brief   Public key BOOT accepts image signatures from.
 * @details Defined by `tools/mkimage.py --export-pubkey` output, selected
 *          with the BOOT_SIGNING_KEY_SOURCE CMake option.  The default,
 *          keys/dev_pubkey.c, is the published development key and must not
 *          ship in a product.
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-05
 * @target  STM32F767ZIT6
 * @srs     BSAFE-006
 */

#ifndef SIGNING_KEY_H
#define SIGNING_KEY_H

#include <stdint.h>

/** Uncompressed secp256r1 point without the 0x04 prefix: X || Y, big-endian. */
extern const uint8_t boot_signing_pubkey[64];

/**
 * First four bytes of SHA-256(boot_signing_pubkey), little-endian; the value
 * mkimage.py writes into the header's key_id field.
 */
extern const uint32_t boot_signing_key_id;

#endif /* SIGNING_KEY_H */
