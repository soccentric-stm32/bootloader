/*
 * metadata.c - append-only slot metadata (Appendix B)
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * Target:  STM32F767ZIT6, dual-bank flash
 * SRS:     IMG-006, SEL-001, TRY-003/007, UPD-002/006/007, Appendix B
 *
 * A record is 32 bytes: magic, one value word, five zero words and the CRC32
 * of the first 28 bytes.  A missing (erased) or torn record fails the magic
 * or CRC check and is treated as absent.  Records are only programmed into
 * erased areas, never rewritten.
 */

#include <string.h>
#include "boot_layout.h"
#include "crc32.h"
#include "flash.h"
#include "image.h"
#include "metadata.h"

#define RECORD_WORDS (BOOT_META_RECORD_SIZE / 4U)

bool boot_meta_header_present(boot_slot_t slot)
{
    const volatile uint32_t *raw =
        (const volatile uint32_t *)boot_meta_base(slot);
    return raw[0] == BOOT_IMAGE_MAGIC;
}

bool boot_meta_header_read(boot_slot_t slot, boot_image_header_t *header)
{
    if (header == NULL) {
        return false;
    }
    memcpy(header, (const void *)boot_meta_base(slot),
           sizeof(boot_image_header_t));
    return header->magic == BOOT_IMAGE_MAGIC;
}

bool boot_meta_read_record(boot_slot_t slot, uint32_t offset, uint32_t magic,
                           uint32_t *value, bool *present)
{
    const volatile uint32_t *record =
        (const volatile uint32_t *)(boot_meta_base(slot) + offset);

    if (value != NULL) {
        *value = record[1];
    }
    if (present == NULL) {
        return false;
    }

    *present = record[0] == magic &&
               boot_crc32((const void *)record,
                          BOOT_META_RECORD_SIZE - 4U) ==
                   record[RECORD_WORDS - 1U];
    return *present;
}

bool boot_meta_write_header(boot_slot_t slot,
                            const boot_image_header_t *header)
{
    if (header == NULL) {
        return false;
    }
    return boot_flash_program_meta(slot, BOOT_META_HEADER_OFFSET, header,
                                   sizeof(*header));
}

bool boot_meta_write_record(boot_slot_t slot, uint32_t offset,
                            uint32_t magic, uint32_t value)
{
    uint32_t record[RECORD_WORDS] = {0};

    record[0] = magic;
    record[1] = value;
    record[RECORD_WORDS - 1U] =
        boot_crc32(record, BOOT_META_RECORD_SIZE - 4U);

    return boot_flash_program_meta(slot, offset, record,
                                   BOOT_META_RECORD_SIZE);
}
