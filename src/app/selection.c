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

_Static_assert(BOOT_TRIAL_LIMIT <= BOOT_META_TRIAL_RECORDS,
               "trial limit exceeds the TRIAL record area");

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

static bool metadata_ok(const boot_slot_info_t *info)
{
    return info->header_valid && info->activated && info->sequence_valid &&
           !info->rejected;
}

static void apply_ram_rejections(boot_selection_t *selection)
{
    for (uint32_t i = 0U; i < 2U; i++) {
        if ((selection->ram_rejected_mask & (uint8_t)(1U << i)) != 0U) {
            boot_slot_info_t *info = &selection->slot[i];
            if (!info->rejected) {
                info->rejected_reason = BOOT_REJECT_TRIAL_LIMIT;
            }
            info->rejected = true;
            info->valid = false;
        }
    }
}

static void scan_metadata(boot_selection_t *selection)
{
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

        info->trial_count = (uint8_t)boot_meta_trial_count(slot);
    }
}

static void check_vectors(boot_selection_t *selection)
{
    for (uint32_t i = 0U; i < 2U; i++) {
        boot_slot_info_t *info = &selection->slot[i];
        if (metadata_ok(info)) {
            info->vectors_ok = boot_image_vectors_valid((boot_slot_t)i);
        }
    }
}

static void check_crc(boot_selection_t *selection, boot_slot_t slot)
{
    boot_slot_info_t *info = &selection->slot[slot];
    info->crc_checked = true;
    info->crc_ok = boot_image_crc_matches(slot, &info->header);
    info->valid = info->vectors_ok && info->crc_ok;
}

static void scan_slots(boot_selection_t *selection, bool verify_all)
{
    uint8_t ram_mask = selection->ram_rejected_mask;
    memset(selection, 0, sizeof(*selection));
    selection->ram_rejected_mask = ram_mask;

    scan_metadata(selection);
    apply_ram_rejections(selection);
    check_vectors(selection);

    if (verify_all) {
        for (uint32_t i = 0U; i < 2U; i++) {
            if (metadata_ok(&selection->slot[i]) &&
                selection->slot[i].vectors_ok) {
                check_crc(selection, (boot_slot_t)i);
            }
        }
        return;
    }

    /* P2-7: verify the best candidate first and only fall back when it
     * fails.  A lower-sequence slot is never checked while the candidate
     * passes, which halves the boot-time CRC work. */
    bool tried[2] = {false, false};
    for (uint32_t pass = 0U; pass < 2U; pass++) {
        int32_t best = -1;
        uint32_t best_sequence = 0U;

        for (uint32_t i = 0U; i < 2U; i++) {
            const boot_slot_info_t *info = &selection->slot[i];
            if (tried[i] || !metadata_ok(info) || !info->vectors_ok) {
                continue;
            }
            if (best < 0 || info->sequence > best_sequence) {
                best = (int32_t)i;
                best_sequence = info->sequence;
            }
        }
        if (best < 0) {
            break;
        }
        tried[best] = true;
        check_crc(selection, (boot_slot_t)best);
        if (selection->slot[best].valid) {
            break;
        }
    }
}

void boot_selection_scan(boot_selection_t *selection)
{
    scan_slots(selection, false);
}

void boot_selection_scan_full(boot_selection_t *selection)
{
    scan_slots(selection, true);
}

void boot_selection_refresh_slots(boot_selection_t *selection)
{
    boot_selection_t refreshed = *selection;

    boot_selection_scan_full(&refreshed);
    for (uint32_t i = 0U; i < 2U; i++) {
        selection->slot[i] = refreshed.slot[i];
    }
}

void boot_selection_mark_rejected(boot_selection_t *selection,
                                  boot_slot_t slot, uint32_t reason)
{
    if (selection == NULL || (uint32_t)slot >= 2U) {
        return;
    }
    selection->ram_rejected_mask |= (uint8_t)(1U << (uint32_t)slot);
    selection->slot[slot].rejected = true;
    selection->slot[slot].rejected_reason = reason;
    selection->slot[slot].valid = false;
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

    uint8_t count = info->trial_count;
    uint32_t raw = *bkp(0);
    if ((raw & BOOT_TRIAL_MAGIC_MASK) == BOOT_TRIAL_MAGIC &&
        ((raw >> BOOT_TRIAL_SLOT_SHIFT) & BOOT_TRIAL_SLOT_MASK) ==
            (uint32_t)selection->active) {
        uint8_t rtc_count =
            (uint8_t)((raw >> BOOT_TRIAL_COUNT_SHIFT) &
                      BOOT_TRIAL_COUNT_MASK);
        if (rtc_count > count) {
            count = rtc_count;
        }
    }
    selection->trial_count = count;
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

    boot_slot_t slot = selection->active;
    boot_slot_info_t *info = &selection->slot[slot];
    uint32_t count = info->trial_count;

    /* P2-6: append a TRIAL record to the active slot's metadata sector.
     * The count is then independent of VBAT and survives a power cycle. */
    if (count < BOOT_TRIAL_LIMIT) {
        uint32_t attempt = count + 1U;
        if (boot_meta_write_trial(slot, attempt)) {
            info->trial_count = (uint8_t)attempt;
            count = attempt;
        } else {
            /* Keep the RTC counter moving even when the metadata write
             * fails so that a single bad flash word cannot cause an endless
             * trial loop within one power session. */
            count = attempt;
        }
    }

    *bkp(0) = BOOT_TRIAL_MAGIC |
              ((uint32_t)slot << BOOT_TRIAL_SLOT_SHIFT) |
              ((count & BOOT_TRIAL_COUNT_MASK) << BOOT_TRIAL_COUNT_SHIFT);
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
