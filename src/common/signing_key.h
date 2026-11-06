/*
 * signing_key.h - public key that BOOT accepts image signatures from
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-05
 * Target:  STM32F767ZIT6
 * SRS:     BSAFE-006
 *
 * The definition is generated from a PEM key by
 * "tools/mkimage.py --export-pubkey" and selected with the
 * BOOT_SIGNING_KEY_SOURCE CMake option.  The default, keys/dev_pubkey.c,
 * is the published development key and must not ship in a product.
 */

#ifndef SIGNING_KEY_H
#define SIGNING_KEY_H

#include <stdint.h>

/* Uncompressed secp256r1 point without the 0x04 prefix: X || Y,
 * big-endian. */
extern const uint8_t boot_signing_pubkey[64];

/* First four bytes of SHA-256(boot_signing_pubkey), little-endian, the
 * value mkimage.py writes into the header's key_id field. */
extern const uint32_t boot_signing_key_id;

#endif /* SIGNING_KEY_H */
