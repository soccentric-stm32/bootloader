/*
 * image.h - application image header (section 5)
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * Target:  STM32F767ZIT6
 * SRS:     IMG-001..IMG-006, section 5.1
 */

#ifndef IMAGE_H
#define IMAGE_H

#include <stdbool.h>
#include <stdint.h>
#include "boot_layout.h"

#define BOOT_IMAGE_MAGIC          0x48574249UL   /* "IBWH" little-endian */
#define BOOT_IMAGE_HEADER_VERSION 1U
#define BOOT_IMAGE_HEADER_SIZE    256U
#define BOOT_IMAGE_TYPE_APP       1U

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
    uint8_t  reserved[164];
    uint32_t header_crc32;
} __attribute__((packed, aligned(4))) boot_image_header_t;

_Static_assert(sizeof(boot_image_header_t) == BOOT_IMAGE_HEADER_SIZE,
               "image header must be 256 bytes");

/* IMG-001, IMG-002: magic, versions, type, header CRC and layout.  The
 * vector and image CRC checks need the slot address (IMG-003/IMG-004). */
bool boot_image_header_valid(const boot_image_header_t *header);

/* IMG-003: initial SP in RAM, reset vector inside the slot. */
bool boot_image_vectors_valid(boot_slot_t slot);

/* IMG-004/IMG-005: CRC32 over the slot contents read back from flash. */
bool boot_image_crc_matches(boot_slot_t slot,
                            const boot_image_header_t *header);

/* Header, vector and image CRC checks together (SEL-001). */
bool boot_image_valid(boot_slot_t slot, const boot_image_header_t *header);

/* Fill in the constant fields and the header CRC after the caller has set
 * version, image_size, image_crc32, entry_address, build_timestamp,
 * git_commit and name. */
void boot_image_header_finalize(boot_image_header_t *header);

#endif /* IMAGE_H */
