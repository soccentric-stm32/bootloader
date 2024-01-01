/**
 * @file    metadata.h
 * @brief   Append-only slot metadata records (Appendix B).
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  STM32F767ZIT6, dual-bank flash
 * @srs     IMG-006, SEL-001, TRY-003/007, UPD-002/006/007, Appendix B
 */

#ifndef METADATA_H
#define METADATA_H

#include <stdbool.h>
#include <stdint.h>
#include "boot_layout.h"
#include "image.h"

/**
 * @brief Copy the 256-byte header record of @p slot.
 * @details Use boot_image_header_valid() to decide whether it is usable.
 * @param[in]  slot   Slot to read.
 * @param[out] header Destination; may be NULL (returns false).
 * @return true when the header magic is present.
 */
bool boot_meta_header_read(boot_slot_t slot, boot_image_header_t *header);

/**
 * @brief Test for the header magic, even if the rest is torn.
 * @param[in] slot Slot to check.
 * @return true when the first word equals BOOT_IMAGE_MAGIC.
 */
bool boot_meta_header_present(boot_slot_t slot);

/**
 * @brief Read one Appendix B record.
 * @param[in]  slot    Slot whose metadata sector to read.
 * @param[in]  offset  Record offset in the sector (BOOT_META_*_OFFSET).
 * @param[in]  magic   Expected record magic.
 * @param[out] value   Second record word, or 0 when absent; may be NULL.
 * @param[out] present true when magic and CRC match; may be NULL.
 * @return Same as @p present.
 */
bool boot_meta_read_record(boot_slot_t slot, uint32_t offset, uint32_t magic,
                           uint32_t *value, bool *present);

/**
 * @brief Program the 256-byte header record.
 * @param[in] slot   Slot whose metadata sector to write.
 * @param[in] header Header to program; NULL returns false.
 * @return true when programmed and verified.
 * @pre The header area is erased (append-only, BSAFE-004).
 */
bool boot_meta_write_header(boot_slot_t slot,
                            const boot_image_header_t *header);

/**
 * @brief Program one 32-byte record: magic, value, zeros, CRC-32.
 * @param[in] slot   Slot whose metadata sector to write.
 * @param[in] offset Record offset in the sector (BOOT_META_*_OFFSET).
 * @param[in] magic  Record magic.
 * @param[in] value  Record value word.
 * @return true when programmed and verified.
 * @pre The record area is erased (append-only, BSAFE-004).
 */
bool boot_meta_write_record(boot_slot_t slot, uint32_t offset,
                            uint32_t magic, uint32_t value);

/**
 * @brief Count contiguous valid TRIAL records (P2-6).
 * @details This is the trial counter that survives a power loss.
 * @param[in] slot Slot to inspect.
 * @return Number of records, 0..BOOT_META_TRIAL_RECORDS.
 */
uint32_t boot_meta_trial_count(boot_slot_t slot);

/**
 * @brief Append TRIAL record number @p attempt.
 * @param[in] slot    Slot whose metadata sector to write.
 * @param[in] attempt 1-based attempt, 1..BOOT_META_TRIAL_RECORDS.
 * @return false when @p attempt is out of range or programming fails.
 */
bool boot_meta_write_trial(boot_slot_t slot, uint32_t attempt);

#endif /* METADATA_H */
