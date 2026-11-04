/*
 * selection.c - metadata scan, A/B selection and trial state (sections 6, 7)
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * Target:  STM32F767ZIT6
 * SRS:     SEL-001..SEL-005, TRY-002/003/005/006/008, HND-003
 *
 * Trial state, the stay request, the hand-over record and the last outcome
 * live in RTC backup registers 0-3.  VBAT is tied to VDD, so they survive a
 * system reset but not a power loss (TRY-005, OPEN-B03).
 */

#include <string.h>
#include "stm32f7xx_hal.h"
#include "boot_layout.h"
#include "flash.h"
#include "image.h"
#include "metadata.h"
#include "selection.h"

static volatile uint32_t *bkp(uint32_t index)
{
    return &((volatile uint32_t *)&RTC->BKP0R)[index];
}

void boot_selection_init(void)
{
    __HAL_RCC_PWR_CLK_ENABLE();
    HAL_PWR_EnableBkUpAccess();
    __HAL_RCC_RTC_CLK_ENABLE();
}

void boot_selection_scan(boot_selection_t *selection)
{
    memset(selection, 0, sizeof(*selection));

    for (uint32_t i = 0U; i < 2U; i++) {
        boot_slot_t slot = (boot_slot_t)i;
        boot_slot_info_t *info = &selection->slot[i];
        uint32_t value = 0U;

        info->header_present = boot_meta_header_present(slot);
        if (info->header_present &&
            boot_meta_header_read(slot, &info->header)) {
            info->header_valid = boot_image_header_valid(&info->header);
        }

        (void)boot_meta_read_record(slot, BOOT_META_VERIFIED_OFFSET,
                                    BOOT_META_VERIFIED_MAGIC, &value,
                                    &info->verified);
        info->verified_crc = value;

        (void)boot_meta_read_record(slot, BOOT_META_ACTIVATED_OFFSET,
                                    BOOT_META_ACTIVATED_MAGIC, &value,
                                    &info->activated);
        info->sequence = value;
        info->sequence_valid = info->activated && value != 0xFFFFFFFFUL;

        (void)boot_meta_read_record(slot, BOOT_META_CONFIRMED_OFFSET,
                                    BOOT_META_CONFIRMED_MAGIC, &value,
                                    &info->confirmed);

        (void)boot_meta_read_record(slot, BOOT_META_REJECTED_OFFSET,
                                    BOOT_META_REJECTED_MAGIC, &value,
                                    &info->rejected);
        info->rejected_reason = value;

        /* A torn REJECTED record cannot be programmed again (append-only).
         * Treat any non-erased rejection area as a conservative rejection so
         * that a damaged image is never booted forever. */
        if (!info->rejected &&
            !boot_flash_range_erased(
                boot_meta_base(slot) + BOOT_META_REJECTED_OFFSET,
                BOOT_META_RECORD_SIZE)) {
            info->rejected = true;
        }

        if (info->header_valid && info->activated && info->sequence_valid &&
            !info->rejected) {
            info->vectors_ok = boot_image_vectors_valid(slot);
            if (info->vectors_ok) {
                info->crc_ok = boot_image_crc_matches(slot, &info->header);
            }
            info->valid = info->vectors_ok && info->crc_ok;
        }
    }
}

void boot_selection_choose(boot_selection_t *selection)
{
    selection->active_valid   = false;
    selection->active         = BOOT_SLOT_A;
    selection->active_pending = false;
    selection->trial_count    = 0U;

    uint32_t best_sequence = 0U;

    for (uint32_t i = 0U; i < 2U; i++) {
        const boot_slot_info_t *info = &selection->slot[i];
        if (!info->valid) {
            continue;
        }
        /* Highest activation sequence wins; a tie keeps the lower slot. */
        if (!selection->active_valid || info->sequence > best_sequence) {
            selection->active_valid = true;
            selection->active       = (boot_slot_t)i;
            best_sequence           = info->sequence;
        }
    }

    if (!selection->active_valid) {
        return;
    }

    const boot_slot_info_t *info = &selection->slot[selection->active];
    selection->active_pending =
        info->activated && !info->confirmed && !info->rejected;

    if (!selection->active_pending) {
        return;
    }

    uint32_t raw = *bkp(0);
    if ((raw & BOOT_TRIAL_MAGIC_MASK) == BOOT_TRIAL_MAGIC &&
        ((raw >> BOOT_TRIAL_SLOT_SHIFT) & BOOT_TRIAL_SLOT_MASK) ==
            (uint32_t)selection->active) {
        selection->trial_count =
            (uint8_t)((raw >> BOOT_TRIAL_COUNT_SHIFT) &
                      BOOT_TRIAL_COUNT_MASK);
    }
}

bool boot_selection_apply_trial_limit(boot_selection_t *selection)
{
    if (!selection->active_valid || !selection->active_pending ||
        selection->trial_count < BOOT_TRIAL_LIMIT) {
        return false;
    }

    boot_slot_t slot = selection->active;
    if (!boot_meta_write_record(slot, BOOT_META_REJECTED_OFFSET,
                                BOOT_META_REJECTED_MAGIC,
                                BOOT_REJECT_TRIAL_LIMIT)) {
        return false;
    }

    boot_outcome_set(BOOT_OUTCOME_ROLLED_BACK, slot);
    boot_selection_clear_trial();
    return true;
}

void boot_selection_start_trial(boot_selection_t *selection)
{
    if (!selection->active_valid || !selection->active_pending) {
        return;
    }

    uint32_t raw = *bkp(0);
    uint32_t count = 0U;

    if ((raw & BOOT_TRIAL_MAGIC_MASK) == BOOT_TRIAL_MAGIC &&
        ((raw >> BOOT_TRIAL_SLOT_SHIFT) & BOOT_TRIAL_SLOT_MASK) ==
            (uint32_t)selection->active) {
        count = (raw >> BOOT_TRIAL_COUNT_SHIFT) & BOOT_TRIAL_COUNT_MASK;
    }
    if (count < BOOT_TRIAL_LIMIT) {
        count++;
    }

    *bkp(0) = BOOT_TRIAL_MAGIC |
              ((uint32_t)selection->active << BOOT_TRIAL_SLOT_SHIFT) |
              (count << BOOT_TRIAL_COUNT_SHIFT);
    __DSB();
    selection->trial_count = (uint8_t)count;
}

void boot_selection_clear_trial(void)
{
    *bkp(0) = 0U;
    __DSB();
}

bool boot_selection_take_stay_request(void)
{
    uint32_t raw = *bkp(1);
    if (raw != BOOT_STAY_MAGIC) {
        return false;
    }
    *bkp(1) = 0U;
    __DSB();
    return true;
}

void boot_selection_write_handover(const boot_selection_t *selection)
{
    uint32_t raw = BOOT_HANDOVER_MAGIC;
    /* The contract uses 1 = slot A and 2 = slot B (boot_contract.h). */
    raw |= (((uint32_t)selection->active + 1U) &
            BOOT_HANDOVER_SLOT_MASK) << BOOT_HANDOVER_SLOT_SHIFT;
    if (selection->active_valid && selection->active_pending) {
        raw |= 1UL << BOOT_HANDOVER_PENDING_BIT;
        raw |= ((uint32_t)selection->trial_count & BOOT_HANDOVER_TRIAL_MASK)
               << BOOT_HANDOVER_TRIAL_SHIFT;
    }
    *bkp(2) = raw;
    __DSB();
}

void boot_outcome_set(uint8_t outcome, boot_slot_t rejected)
{
    uint32_t raw = BOOT_OUTCOME_MAGIC;
    raw |= ((uint32_t)outcome & BOOT_OUTCOME_MASK) << BOOT_OUTCOME_SHIFT;
    raw |= ((uint32_t)rejected & BOOT_OUTCOME_REJECT_MASK)
           << BOOT_OUTCOME_REJECT_SHIFT;
    *bkp(3) = raw;
    __DSB();
}

void boot_outcome_clear(void)
{
    *bkp(3) = 0U;
    __DSB();
}

uint8_t boot_outcome_get(boot_slot_t *rejected)
{
    uint32_t raw = *bkp(3);
    if ((raw & BOOT_OUTCOME_MAGIC_MASK) != BOOT_OUTCOME_MAGIC) {
        if (rejected != NULL) {
            *rejected = BOOT_SLOT_A;
        }
        return BOOT_OUTCOME_UNKNOWN;
    }
    if (rejected != NULL) {
        uint32_t r = (raw >> BOOT_OUTCOME_REJECT_SHIFT) &
                     BOOT_OUTCOME_REJECT_MASK;
        *rejected = (boot_slot_t)r;
    }
    return (uint8_t)((raw >> BOOT_OUTCOME_SHIFT) & BOOT_OUTCOME_MASK);
}
