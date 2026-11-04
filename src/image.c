/*
 * image.c - application image header (section 5)
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * Target:  STM32F767ZIT6
 * SRS:     IMG-001..IMG-006, section 5.1
 */

#include <stddef.h>
#include <string.h>
#include "boot_layout.h"
#include "crc32.h"
#include "image.h"

bool boot_image_header_valid(const boot_image_header_t *header)
{
    if (header == NULL) {
        return false;
    }
    if (header->magic != BOOT_IMAGE_MAGIC ||
        header->header_version != BOOT_IMAGE_HEADER_VERSION ||
        header->header_size != BOOT_IMAGE_HEADER_SIZE ||
        header->image_type != BOOT_IMAGE_TYPE_APP) {
        return false;
    }
    if (boot_crc32(header, offsetof(boot_image_header_t, header_crc32)) !=
        header->header_crc32) {
        return false;
    }
    if (header->load_address != BOOT_APP_LOAD_ADDR ||
        header->image_size < 8U || header->image_size > BOOT_SLOT_SIZE) {
        return false;
    }
    return true;
}

bool boot_image_vectors_valid(boot_slot_t slot)
{
    const volatile uint32_t *vec =
        (const volatile uint32_t *)boot_slot_base(slot);
    uint32_t sp    = vec[0];
    uint32_t entry = vec[1];
    uint32_t entry_addr = entry & ~1UL;

    bool sp_ok = (sp > BOOT_RAM_START) && (sp <= BOOT_RAM_END) &&
                 ((sp & 0x7UL) == 0U);
    bool entry_ok = ((entry & 1UL) != 0U) &&
                    (entry_addr >= BOOT_APP_LOAD_ADDR) &&
                    (entry_addr < (BOOT_APP_LOAD_ADDR + BOOT_SLOT_SIZE));
    return sp_ok && entry_ok;
}

bool boot_image_crc_matches(boot_slot_t slot,
                            const boot_image_header_t *header)
{
    if (header == NULL || header->image_size > BOOT_SLOT_SIZE) {
        return false;
    }
    uint32_t crc = boot_crc32((const void *)boot_slot_base(slot),
                              header->image_size);
    return crc == header->image_crc32;
}

bool boot_image_valid(boot_slot_t slot, const boot_image_header_t *header)
{
    return boot_image_header_valid(header) &&
           boot_image_vectors_valid(slot) &&
           boot_image_crc_matches(slot, header);
}

void boot_image_header_finalize(boot_image_header_t *header)
{
    if (header == NULL) {
        return;
    }
    header->magic          = BOOT_IMAGE_MAGIC;
    header->header_version = (uint16_t)BOOT_IMAGE_HEADER_VERSION;
    header->header_size    = (uint16_t)BOOT_IMAGE_HEADER_SIZE;
    header->image_type     = BOOT_IMAGE_TYPE_APP;
    header->load_address   = BOOT_APP_LOAD_ADDR;
    memset(header->reserved, 0, sizeof(header->reserved));
    header->header_crc32 =
        boot_crc32(header, offsetof(boot_image_header_t, header_crc32));
}
