/*
 * image.h - application image header (section 5)
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * Target:  STM32F767ZIT6
 * SRS:     IMG-001..IMG-006, section 5.1, BSAFE-006
 *
 * Header version 2 adds image authentication in what was the reserved
 * area of version 1: an anti-rollback security counter, the signing key
 * id, the SHA-256 of the image and an ECDSA P-256 signature over header
 * bytes 0..127.  The 256-byte size and every version 1 offset are kept.
 */

#ifndef IMAGE_H
#define IMAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "boot_layout.h"

#define BOOT_IMAGE_MAGIC          0x48574249UL   /* "IBWH" little-endian */
#define BOOT_IMAGE_HEADER_VERSION 2U   /* signed (BSAFE-006)        */
#define BOOT_IMAGE_HEADER_VERSION_UNSIGNED 1U
#define BOOT_IMAGE_HEADER_SIZE    256U
#define BOOT_IMAGE_TYPE_APP       1U

/* BSAFE-006: 1 = only signed (version 2) images are accepted.  Set by the
 * build (BOOT_SIGNED_IMAGES CMake option); 0 keeps the version 1 CRC-only
 * behaviour for bench work. */
#ifndef BOOT_SIGNED_IMAGES
#define BOOT_SIGNED_IMAGES        1
#endif

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t header_version;
    uint16_t header_size;
    uint32_t image_type;
    uint32_t version;
    uint32_t image_size;
    uint32_t image_crc32;
    uint32_t load_address;
    uint32_t entry_address;
    uint32_t build_timestamp;
    uint8_t  git_commit[20];
    char     name[32];
    /* Version 2 (BSAFE-006).  Zero in a version 1 header. */
    uint32_t security_counter;    /* anti-rollback, offset 88          */
    uint32_t key_id;              /* first 4 bytes of SHA-256(pubkey)  */
    uint8_t  image_sha256[32];    /* SHA-256 of the image_size bytes   */
    uint8_t  signature[64];       /* ECDSA P-256 r || s, big-endian    */
    uint8_t  reserved[60];
    uint32_t header_crc32;
} __attribute__((packed, aligned(4))) boot_image_header_t;

_Static_assert(sizeof(boot_image_header_t) == BOOT_IMAGE_HEADER_SIZE,
               "image header must be 256 bytes");

/* Header bytes 88..251 (the version 1 reserved area) travel verbatim in
 * hwlink.ImageHeader.extension so that BOOT rebuilds the signed header
 * exactly (protocol minor 3). */
#define BOOT_IMAGE_EXT_OFFSET     88U
#define BOOT_IMAGE_EXT_SIZE       164U
/* The signature covers header bytes 0..127, everything before it. */
#define BOOT_IMAGE_SIGNED_SIZE    128U

_Static_assert(offsetof(boot_image_header_t, security_counter) ==
                   BOOT_IMAGE_EXT_OFFSET, "extension offset");
_Static_assert(offsetof(boot_image_header_t, signature) ==
                   BOOT_IMAGE_SIGNED_SIZE, "signed prefix");
_Static_assert(offsetof(boot_image_header_t, header_crc32) ==
                   BOOT_IMAGE_EXT_OFFSET + BOOT_IMAGE_EXT_SIZE,
               "extension size");

/* IMG-001, IMG-002: magic, versions, type, header CRC and layout.  The
 * vector and image CRC checks need the slot address (IMG-003/IMG-004).
 * With BOOT_SIGNED_IMAGES this also requires header version 2, BOOT's key
 * id and a valid signature (BSAFE-006). */
bool boot_image_header_valid(const boot_image_header_t *header);

/* BSAFE-006: why boot_image_header_valid() failed, for diagnostics. */
typedef enum {
    BOOT_HEADER_OK = 0,
    BOOT_HEADER_BAD_FORMAT,      /* magic, version, size, type or CRC    */
    BOOT_HEADER_BAD_LAYOUT,      /* load address or image size           */
    BOOT_HEADER_UNSIGNED,        /* version 1 while signing is required  */
    BOOT_HEADER_WRONG_KEY,       /* key_id is not BOOT's key             */
    BOOT_HEADER_BAD_SIGNATURE
} boot_header_status_t;

boot_header_status_t boot_image_header_check(
    const boot_image_header_t *header);

/* BSAFE-006: SHA-256 of the slot contents equals header->image_sha256.
 * Always true for an unsigned build. */
bool boot_image_digest_matches(boot_slot_t slot,
                               const boot_image_header_t *header);

/* IMG-003: initial SP in RAM, reset vector inside the slot. */
bool boot_image_vectors_valid(boot_slot_t slot);

/* IMG-004/IMG-005: CRC32 over the slot contents read back from flash. */
bool boot_image_crc_matches(boot_slot_t slot,
                            const boot_image_header_t *header);

/* Header, vector, image CRC and digest checks together (SEL-001). */
bool boot_image_valid(boot_slot_t slot, const boot_image_header_t *header);

/* Fill in the constant fields and the header CRC after the caller has set
 * version, image_size, image_crc32, entry_address, build_timestamp,
 * git_commit, name, header_version and, for a signed image, the version 2
 * fields.  A header_version of 0 means an unsigned version 1 header. */
void boot_image_header_finalize(boot_image_header_t *header);

#endif /* IMAGE_H */
