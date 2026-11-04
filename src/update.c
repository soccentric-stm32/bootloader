/*
 * update.c - image update state machine (section 9)
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * Target:  STM32F767ZIT6
 * SRS:     UPD-001..UPD-010, IMG-001..IMG-006
 *
 * Only the inactive slot is written (UPD-001).  Slot sectors are erased when
 * the first chunk that falls into them arrives (UPD-003).  Chunks may have
 * any length from 1 to 1024 bytes; a small staging buffer keeps flash
 * programming word-aligned.
 */

#include <string.h>
#include "boot_layout.h"
#include "crc32.h"
#include "flash.h"
#include "image.h"
#include "metadata.h"
#include "selection.h"
#include "update.h"

static boot_progress_fn progress_cb;

static void detail_set(char *detail, size_t cap, const char *text)
{
    if (detail == NULL || cap == 0U) {
        return;
    }
    size_t i = 0U;
    while (i + 1U < cap && text[i] != '\0') {
        detail[i] = text[i];
        i++;
    }
    detail[i] = '\0';
}

static void detail_flash_error(char *detail, size_t cap, uint32_t address)
{
    static const char hex[] = "0123456789ABCDEF";
    static const char prefix[] = "flash error at 0x";
    char buffer[sizeof(prefix) + 8U + 1U];
    size_t pos = 0U;

    for (size_t i = 0U; i < sizeof(prefix) - 1U; i++) {
        buffer[pos++] = prefix[i];
    }
    for (uint32_t shift = 28U;; shift -= 4U) {
        buffer[pos++] = hex[(address >> shift) & 0xFUL];
        if (shift == 0U) {
            break;
        }
    }
    buffer[pos] = '\0';
    detail_set(detail, cap, buffer);
}

void boot_update_reset(boot_update_t *update)
{
    memset(update, 0, sizeof(*update));
}

void boot_update_set_progress_cb(boot_progress_fn callback)
{
    progress_cb = callback;
}

static void progress(uint32_t done, uint32_t total)
{
    if (progress_cb != NULL) {
        progress_cb(done, total);
    }
}

static bool validate_desc(const boot_image_desc_t *desc, boot_error_t *error,
                          char *detail, size_t cap)
{
    if (desc->magic != BOOT_IMAGE_MAGIC ||
        desc->header_version != BOOT_IMAGE_HEADER_VERSION ||
        desc->header_size != BOOT_IMAGE_HEADER_SIZE ||
        desc->image_type != BOOT_IMAGE_TYPE_APP) {
        *error = BOOT_ERR_BAD_HEADER;
        detail_set(detail, cap, "magic, version, size or type");
        return false;
    }
    if (desc->load_address != BOOT_APP_LOAD_ADDR ||
        desc->image_size < 8U || desc->image_size > BOOT_SLOT_SIZE) {
        *error = BOOT_ERR_BAD_LAYOUT;
        detail_set(detail, cap, "load address or image size");
        return false;
    }
    return true;
}

boot_error_t boot_update_begin(boot_update_t *update,
                               const boot_image_desc_t *desc,
                               char *detail, size_t detail_cap)
{
    if (update == NULL || desc == NULL) {
        return BOOT_ERR_MALFORMED;
    }
    if (update->active) {
        detail_set(detail, detail_cap, "update already in progress");
        return BOOT_ERR_BAD_STATE;
    }

    boot_error_t error = BOOT_ERR_NONE;
    if (!validate_desc(desc, &error, detail, detail_cap)) {
        return error;
    }

    boot_image_header_t header;
    memset(&header, 0, sizeof(header));
    header.version         = desc->version;
    header.image_size      = desc->image_size;
    header.image_crc32     = desc->image_crc32;
    header.entry_address   = desc->entry_address;
    header.build_timestamp = desc->build_timestamp;
    memcpy(header.git_commit, desc->git_commit, sizeof(header.git_commit));
    memcpy(header.name, desc->name, sizeof(header.name));
    boot_image_header_finalize(&header);

    boot_slot_t slot = boot_flash_target_slot();

    if (!boot_flash_erase_meta(slot)) {
        detail_set(detail, detail_cap, "metadata sector erase failed");
        return BOOT_ERR_FLASH_ERROR;
    }
    if (!boot_meta_write_header(slot, &header)) {
        detail_set(detail, detail_cap, "header program failed");
        return BOOT_ERR_FLASH_ERROR;
    }

    boot_update_reset(update);
    update->active        = true;
    update->slot          = slot;
    update->image_size    = header.image_size;
    update->image_crc32   = header.image_crc32;
    update->header        = header;
    update->expected_offset = 0U;
    update->last_offset     = 0U;
    update->last_offset_valid = false;
    update->erased_mask     = 0U;
    update->word_stage_len  = 0U;
    return BOOT_ERR_NONE;
}

static bool ensure_sectors_erased(boot_update_t *update, uint32_t offset,
                                  uint32_t len, char *detail,
                                  size_t detail_cap)
{
    if (len == 0U) {
        return true;
    }
    uint32_t first = offset;
    uint32_t last  = offset + len - 1U;

    for (uint32_t i = 0U; i < BOOT_SLOT_SECTOR_COUNT; i++) {
        uint32_t base = boot_slot_sector_offset(i);
        uint32_t size = boot_slot_sector_size(i);
        if (last < base || first >= (base + size)) {
            continue;
        }
        uint32_t bit = 1UL << i;
        if ((update->erased_mask & bit) != 0U) {
            continue;
        }
        progress(offset, update->image_size);
        if (!boot_flash_erase_slot_sector(update->slot, i)) {
            detail_set(detail, detail_cap, "slot sector erase failed");
            return false;
        }
        update->erased_mask |= bit;
        progress(offset, update->image_size);
    }
    return true;
}

static bool program_word(boot_update_t *update, uint32_t offset,
                         const uint8_t bytes[4], char *detail,
                         size_t detail_cap)
{
    uint32_t word = (uint32_t)bytes[0] |
                    ((uint32_t)bytes[1] << 8U) |
                    ((uint32_t)bytes[2] << 16U) |
                    ((uint32_t)bytes[3] << 24U);
    uint32_t fail_address = 0U;

    if (!boot_flash_program_slot(update->slot, offset, &word, sizeof(word),
                                 &fail_address)) {
        detail_flash_error(detail, detail_cap, fail_address);
        return false;
    }
    return true;
}

boot_error_t boot_update_data(boot_update_t *update, uint32_t offset,
                              const uint8_t *data, uint32_t len,
                              uint32_t *next_offset,
                              char *detail, size_t detail_cap)
{
    if (next_offset != NULL) {
        *next_offset = update != NULL ? update->expected_offset : 0U;
    }
    if (update == NULL || data == NULL) {
        return BOOT_ERR_MALFORMED;
    }
    if (!update->active) {
        detail_set(detail, detail_cap, "no update in progress");
        return BOOT_ERR_BAD_STATE;
    }
    if (len == 0U || len > 1024U) {
        detail_set(detail, detail_cap, "chunk must be 1..1024 bytes");
        return BOOT_ERR_BAD_ARGUMENT;
    }

    /* UPD-004: a repeated chunk is acknowledged again without reprogramming. */
    if (offset != update->expected_offset) {
        if (update->last_offset_valid && offset == update->last_offset) {
            if (next_offset != NULL) {
                *next_offset = update->expected_offset;
            }
            return BOOT_ERR_NONE;
        }
        detail_set(detail, detail_cap, "offset does not match");
        return BOOT_ERR_OUT_OF_ORDER;
    }

    if (offset > update->image_size ||
        len > (update->image_size - offset)) {
        detail_set(detail, detail_cap, "chunk beyond image size");
        return BOOT_ERR_BAD_ARGUMENT;
    }

    if (!ensure_sectors_erased(update, offset, len, detail, detail_cap)) {
        return BOOT_ERR_FLASH_ERROR;
    }

    uint32_t position = offset;
    uint32_t remaining = len;
    const uint8_t *source = data;

    while (remaining > 0U) {
        if (update->word_stage_len == 0U) {
            update->word_stage_offset = position;
        }
        uint32_t need = 4U - update->word_stage_len;
        uint32_t take = remaining < need ? remaining : need;

        memcpy(&update->word_stage[update->word_stage_len], source, take);
        update->word_stage_len += take;
        source    += take;
        position  += take;
        remaining -= take;

        if (update->word_stage_len == 4U) {
            if (!program_word(update, update->word_stage_offset,
                              update->word_stage, detail, detail_cap)) {
                return BOOT_ERR_FLASH_ERROR;
            }
            update->word_stage_len = 0U;
        }
    }

    update->last_offset       = offset;
    update->last_offset_valid = true;
    update->expected_offset   = offset + len;
    if (next_offset != NULL) {
        *next_offset = update->expected_offset;
    }
    return BOOT_ERR_NONE;
}

boot_error_t boot_update_end(boot_update_t *update, uint32_t *image_crc,
                             char *detail, size_t detail_cap)
{
    if (image_crc != NULL) {
        *image_crc = 0U;
    }
    if (update == NULL || !update->active) {
        detail_set(detail, detail_cap, "no update in progress");
        return BOOT_ERR_BAD_STATE;
    }
    if (update->verified) {
        /* A repeated UpdateEnd (lost reply) is a no-op. */
        if (image_crc != NULL) {
            *image_crc = update->image_crc32;
        }
        return BOOT_ERR_NONE;
    }
    if (update->expected_offset != update->image_size) {
        detail_set(detail, detail_cap, "image incomplete");
        return BOOT_ERR_BAD_STATE;
    }

    /* Flush the final partial word with 0xFF padding; the image CRC only
     * covers image_size bytes. */
    if (update->word_stage_len > 0U) {
        if (!ensure_sectors_erased(update, update->word_stage_offset, 4U,
                                   detail, detail_cap)) {
            return BOOT_ERR_FLASH_ERROR;
        }
        for (uint32_t i = update->word_stage_len; i < 4U; i++) {
            update->word_stage[i] = 0xFFU;
        }
        if (!program_word(update, update->word_stage_offset,
                          update->word_stage, detail, detail_cap)) {
            return BOOT_ERR_FLASH_ERROR;
        }
        update->word_stage_len = 0U;
    }

    if (!boot_image_vectors_valid(update->slot)) {
        detail_set(detail, detail_cap, "stack pointer or reset vector");
        return BOOT_ERR_BAD_VECTORS;
    }

    uint32_t crc = boot_crc32((const void *)boot_slot_base(update->slot),
                              update->image_size);
    if (image_crc != NULL) {
        *image_crc = crc;
    }
    if (crc != update->image_crc32) {
        detail_set(detail, detail_cap, "image CRC32 mismatch");
        return BOOT_ERR_CRC_MISMATCH;
    }

    if (!boot_meta_write_record(update->slot, BOOT_META_VERIFIED_OFFSET,
                                BOOT_META_VERIFIED_MAGIC, crc)) {
        detail_set(detail, detail_cap, "VERIFIED record program failed");
        return BOOT_ERR_FLASH_ERROR;
    }

    update->verified = true;
    return BOOT_ERR_NONE;
}

boot_error_t boot_update_activate(boot_update_t *update,
                                  const boot_selection_t *selection,
                                  char *detail, size_t detail_cap)
{
    boot_slot_t slot = BOOT_SLOT_A;

    if (update != NULL && update->active) {
        if (!update->verified) {
            detail_set(detail, detail_cap, "slot is not verified");
            return BOOT_ERR_BAD_STATE;
        }
        slot = update->slot;
    } else {
        /* Activate a slot that was verified in an earlier session. */
        slot = boot_flash_target_slot();
        bool present = false;
        (void)boot_meta_read_record(slot, BOOT_META_VERIFIED_OFFSET,
                                    BOOT_META_VERIFIED_MAGIC, NULL,
                                    &present);
        if (!present) {
            detail_set(detail, detail_cap, "slot is not verified");
            return BOOT_ERR_BAD_STATE;
        }
    }

    bool already = false;
    (void)boot_meta_read_record(slot, BOOT_META_ACTIVATED_OFFSET,
                                BOOT_META_ACTIVATED_MAGIC, NULL, &already);
    if (already) {
        detail_set(detail, detail_cap, "slot is already activated");
        return BOOT_ERR_BAD_STATE;
    }

    bool rejected = false;
    (void)boot_meta_read_record(slot, BOOT_META_REJECTED_OFFSET,
                                BOOT_META_REJECTED_MAGIC, NULL, &rejected);
    if (rejected ||
        !boot_flash_range_erased(
            boot_meta_base(slot) + BOOT_META_REJECTED_OFFSET,
            BOOT_META_RECORD_SIZE)) {
        detail_set(detail, detail_cap, "slot is rejected");
        return BOOT_ERR_BAD_STATE;
    }

    /* The slot must still pass the full header, vector and CRC checks. */
    boot_image_header_t header;
    if (!boot_meta_header_read(slot, &header) ||
        !boot_image_valid(slot, &header)) {
        detail_set(detail, detail_cap, "slot is no longer valid");
        return BOOT_ERR_BAD_STATE;
    }

    boot_slot_t other = boot_other_slot(slot);
    uint32_t other_sequence = 0U;
    bool other_present = false;
    (void)boot_meta_read_record(other, BOOT_META_ACTIVATED_OFFSET,
                                BOOT_META_ACTIVATED_MAGIC, &other_sequence,
                                &other_present);
    if (!other_present || other_sequence == 0xFFFFFFFFUL) {
        other_sequence = 0U;
    }
    uint32_t sequence = other_sequence + 1U;
    if (sequence == 0xFFFFFFFFUL) {
        sequence = 1U;
    }

    (void)selection;

    if (!boot_meta_write_record(slot, BOOT_META_ACTIVATED_OFFSET,
                                BOOT_META_ACTIVATED_MAGIC, sequence)) {
        detail_set(detail, detail_cap, "ACTIVATED record program failed");
        return BOOT_ERR_FLASH_ERROR;
    }

    boot_selection_clear_trial();
    boot_outcome_clear();
    if (update != NULL) {
        update->active = false;
    }
    return BOOT_ERR_NONE;
}

void boot_update_abort(boot_update_t *update)
{
    if (update == NULL) {
        return;
    }
    if (update->active) {
        (void)boot_flash_erase_meta(update->slot);
    }
    boot_update_reset(update);
}
