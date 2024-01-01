/**
 * @file    selection.c
 * @brief   Metadata scan, A/B selection and trial state (SRS sections 6, 7).
 *
 * @details
 * Implementation notes that apply to the whole file:
 *
 * - **Persistence layout.** Trial state, the stay-request, the hand-over
 *   record and the last outcome all live in @e RTC backup registers 0-3
 *   (see the layout tables in selection.h).  VBAT is tied to VDD on this
 *   board, so the registers survive a system reset but /do not/ a power
 *   loss (TRY-005, OPEN-B03) - the persistent source of truth for the
 *   trial count and the reject state is therefore the append-only
 *   metadata record in flash; the RTC copy is a fast mirror plus the
 *   RAM fallback used when the flash write fails.
 *
 * - **Scan pipeline.** Each scan runs the same five stages in the same
 *   order: metadata read -> RAM rejections -> security floor -> vector
 *   check -> (expensive) CRC/SHA check.  Only the last stage differs
 *   between the fast (boot path) and full (diagnostic) variants.
 *
 * - **Cost model (P2-7).** The hardware CRC and the SHA-256 are
 *   deliberately ordered cheap-first: the CRC is always computed, the
 *   SHA-256 only for an image whose CRC already matches.
 *
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  STM32F767ZIT6
 * @srs     SEL-001..SEL-005, TRY-002/003/005/006/008, HND-003, BSAFE-006
 */

#include <string.h>
#include "stm32f7xx_hal.h"
#include "boot_layout.h"
#include "flash.h"
#include "image.h"
#include "metadata.h"
#include "selection.h"

/**
 * @brief   Compile-time guard: the per-slot flash trial-record area is
 *          large enough to hold the trial limit.
 */
_Static_assert(BOOT_TRIAL_LIMIT <= BOOT_META_TRIAL_RECORDS,
               "trial limit exceeds the TRIAL record area");

/**
 * @brief   Indexed accessor for the RTC backup registers BKP0R .. BKP9R.
 *
 * @param[in] index  0-based index from BKP0R (e.g. @c 0 -> BKP0R, @c 3 -> BKP3R).
 * @return  Pointer to the 32-bit backup register at the given index.
 *
 * @note    Declared `volatile`: the register set is accessed by the
 *          whole module and must not be reordered or dropped by the
 *          compiler.
 */
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

/**
 * @brief   Metadata-level gate: can this slot even be considered?
 *
 * @details Cheap (register-only) pre-filter used before the vector and
 *          CRC checks.  A slot passes when its header is valid, it is
 *          armed by a successful `Activate`, its sequence is coherent
 *          (non-erased), it is not rejected, and it is not below the
 *          anti-rollback floor.
 *
 * @param[in] info  Slot information to test.
 * @return  `true`  when the slot passes the metadata gate.
 * @return  `false` when any of the five conditions fails.
 */
static bool metadata_ok(const boot_slot_info_t *info)
{
    return info->header_valid && info->activated && info->sequence_valid &&
           !info->rejected && !info->below_floor;
}

uint32_t boot_selection_security_floor(const boot_selection_t *selection)
{
    uint32_t floor = 0U;

    for (uint32_t i = 0U; i < 2U; i++) {
        const boot_slot_info_t *info = &selection->slot[i];
        if (info->header_valid && info->confirmed && !info->rejected &&
            info->header.security_counter > floor) {
            floor = info->header.security_counter;
        }
    }
    return floor;
}

/**
 * @brief   Apply the anti-rollback floor from an already-computed value.
 *
 * @details Helper used by the scan pipeline: walks the two slot
 *          entries and sets `below_floor` where the security counter is
 *          strictly below @ref boot_selection_security_floor.
 *
 * @param[in,out] selection  Selection context to annotate.
 *
 * @srs     BSAFE-006
 */
static void apply_security_floor(boot_selection_t *selection)
{
    uint32_t floor = boot_selection_security_floor(selection);

    for (uint32_t i = 0U; i < 2U; i++) {
        boot_slot_info_t *info = &selection->slot[i];
        info->below_floor = info->header_valid &&
                            info->header.security_counter < floor;
    }
}

/**
 * @brief   Re-apply the per-boot RAM rejection mask (P0-2).
 *
 * @details Invoked from every scan so that a slot rejected in RAM (because
 *          the persistent record could not be programmed) cannot become
 *          bootable or updatable again in the same boot sequence.
 *
 * @param[in,out] selection  Selection context; bits in
 *                           `ram_rejected_mask` force the corresponding
 *                           slot's `rejected`/`valid` flags.
 *
 * @srs     P0-2, TRY-003 (fallback path)
 */
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

/**
 * @brief   Stage 1 of the scan: read header + lifecycle records for both
 *          slots.
 *
 * @details Reads, per slot: the image header, the VERIFIED, ACTIVATED,
 *          CONFIRMED and REJECTED metadata records, and the per-slot
 *          trial count.  A special defensive rule: a /torn/ REJECTED
 *          record (the flash range is non-erased but the record itself
 *          is not a clean write) is treated as a rejection, because the
 *          record is append-only and cannot be re-programmed - a
 *          conservative rejection is preferred over booting a damaged
 *          image forever.
 *
 * @param[in,out] selection  Selection context; the `header_*` and
 *                           lifecycle fields of both slots are filled.
 *                           `ram_rejected_mask` is /not/ touched.
 */
static void scan_metadata(boot_selection_t *selection)
{
    for (uint32_t i = 0U; i < 2U; i++) {
        boot_slot_t slot = (boot_slot_t)i;
        boot_slot_info_t *info = &selection->slot[i];
        uint32_t value = 0U;

        info->header_present = boot_meta_header_present(slot);
        if (info->header_present &&
            boot_meta_header_read(slot, &info->header)) {
            info->header_status = boot_image_header_check(&info->header);
            info->header_valid = info->header_status == BOOT_HEADER_OK;
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

/**
 * @brief   Stage 4 of the scan: validate the vector tables of every
 *          slot that already passed the metadata gate.
 *
 * @details Cheap (few words of flash).  A slot that fails the metadata
 *          gate is skipped - no point checking the vectors of an image
 *          we would reject anyway.
 *
 * @param[in,out] selection  Selection context; `vectors_ok` set for
 *                           metadata-good slots.
 */
static void check_vectors(boot_selection_t *selection)
{
    for (uint32_t i = 0U; i < 2U; i++) {
        boot_slot_info_t *info = &selection->slot[i];
        if (metadata_ok(info)) {
            info->vectors_ok = boot_image_vectors_valid((boot_slot_t)i);
        }
    }
}

/**
 * @brief   Stage 5 (expensive) of the scan: CRC + SHA-256 of one slot.
 *
 * @details Order is deliberately cheap-first: the hardware CRC is
 *          computed for every candidate; the SHA-256 (and hence the
 *          ECDSA verification it feeds into) is only computed for an
 *          image whose CRC already matches, so a single CRC failure
 *          saves a full digest over the slot bytes.
 *
 * Sets the composite `valid` gate for the slot on exit.
 *
 * @param[in,out] selection  Selection context; `crc_checked`, `crc_ok`,
 *                           `digest_ok` and `valid` set for the slot.
 * @param[in]     slot       Slot to verify (A or B).
 */
static void check_crc(boot_selection_t *selection, boot_slot_t slot)
{
    boot_slot_info_t *info = &selection->slot[slot];
    info->crc_checked = true;
    info->crc_ok = boot_image_crc_matches(slot, &info->header);
    /* The cheap hardware CRC catches corruption first; the SHA-256 is only
     * computed for an image that already passed it. */
    info->digest_ok = info->crc_ok &&
                      boot_image_digest_matches(slot, &info->header);
    info->valid = info->vectors_ok && info->crc_ok && info->digest_ok;
}

/**
 * @brief   Scan both slots and fill @ref boot_slot_info_t entries.
 *
 * @details Common pipeline for both the boot-path and the diagnostic
 *          scan: metadata -> RAM rejections -> security floor -> vector
 *          check -> (expensive) verify pass.  The only difference is
 *          the verify pass:
 *
 * - **`verify_all == true`** (diagnostic path, @ref boot_selection_scan_full):
 *   every slot that passed the cheap checks is verified, so the host
 *   gets the full picture.
 * - **`verify_all == false`** (boot path, @ref boot_selection_scan):
 *   verify the best candidate first, then the next-best only when the
 *   first fails.  This is the P2-7 optimisation: a passing lower-
 *   sequence slot is never checked while the higher-sequence
 *   candidate passes, which halves the CRC work in the common
 *   case (exactly one good image present).
 *
 * The per-boot RAM rejection mask is preserved across the scan (it is
 * the only field that /survives/ the `memset` and is re-established
 * on the fresh structure).
 *
 * @param[in,out] selection  Selection context; fully overwritten except
 *                           for `ram_rejected_mask`.
 * @param[in]     verify_all See above.
 */
static void scan_slots(boot_selection_t *selection, bool verify_all)
{
    uint8_t ram_mask = selection->ram_rejected_mask;
    memset(selection, 0, sizeof(*selection));
    selection->ram_rejected_mask = ram_mask;

    scan_metadata(selection);
    apply_ram_rejections(selection);
    apply_security_floor(selection);
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

/**
 * @brief   Fast scan (boot path) - see @ref scan_slots for the pipeline.
 *
 * @param[in,out] selection  Selection context.
 */
void boot_selection_scan(boot_selection_t *selection)
{
    scan_slots(selection, false);
}

/**
 * @brief   Full scan (diagnostic path) - see @ref scan_slots for the pipeline.
 *
 * @param[in,out] selection  Selection context.
 */
void boot_selection_scan_full(boot_selection_t *selection)
{
    scan_slots(selection, true);
}

/**
 * @brief   Refresh slot-level info without changing the active/target
 *          decision (used by GetBootInfo).
 *
 * @details Runs a full scan into a scratch copy of the caller's context
 *          (preserving any in-flight update target) and then replaces
 *          only the two @ref boot_slot_info_t entries.  The
 *          `active*` fields and `ram_rejected_mask` are untouched.
 *
 * @param[in,out] selection  Selection context whose slot views are
 *                           refreshed.
 */
void boot_selection_refresh_slots(boot_selection_t *selection)
{
    boot_selection_t refreshed = *selection;

    boot_selection_scan_full(&refreshed);
    for (uint32_t i = 0U; i < 2U; i++) {
        selection->slot[i] = refreshed.slot[i];
    }
}

/**
 * @brief   Reject a slot in RAM for this boot (P0-2) - see selection.h.
 *
 * @param[in,out] selection  Context to update; no-op on NULL or invalid
 *                           slot index.
 * @param[in]     slot       Slot to reject.
 * @param[in]     reason     Reason code for diagnostics.
 *
 * @srs     P0-2
 */
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

/**
 * @brief   Choose the active slot from an already-scanned selection.
 *
 * @details Ranking rule (SEL-002): highest activation sequence wins;
 *          a tie keeps the lower slot (slot A preferred over B on a
 *          tie).  When the selected slot is pending (activated but not
 *          confirmed), `trial_count` is reconciled with the RTC mirror:
 *          the RTC value is used when it is higher than the flash-derived
 *          count (power-cycle resilience).
 *
 * @param[in,out] selection  Context; `active`, `active_valid`,
 *                           `active_pending`, `trial_count` are set on
 *                           exit.
 *
 * @srs     SEL-002
 */
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

/**
 * @brief   Apply the per-boot trial limit (TRY-003).
 *
 * @details Called by @ref main after the first choose.  When the
 *          selected slot is pending and `trial_count` has reached
 *          `BOOT_TRIAL_LIMIT`:
 *
 * 1. Program a REJECTED record into the slot's metadata (persistent;
 *    the flash-write lock must allow it - which the caller knows, in
 *    the normal order of operations, it does).
 * 2. Record the rollback outcome (for the host) and clear the trial
 *          mirror.
 * 3. Return `true` so the caller re-scans and re-choose.
 *
 * If the REJECTED record /could not/ be programmed, the function
 * returns `false` - the caller then falls back to @ref
 * boot_selection_mark_rejected (RAM-only, P0-2) and re-scans.
 *
 * @param[in,out] selection  Context to update.
 * @return  `true`  when the caller must re-scan and re-choose.
 * @return  `false` when no rejection was applied (either the limit was
 *          not reached, or the persistent write failed and the caller
 *          must use the RAM fallback).
 *
 * @srs     TRY-003
 */
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

/**
 * @brief   Increment the trial counter for the slot about to be launched
 *          (TRY-002).
 *
 * @details Appends a TRIAL record to the active slot's metadata sector
 *          (P2-6: persistent, power-cycle-resilient) and updates the
 *          RTC mirror so the count is readable between power cycles
 *          even if the flash write failed.  When the flash write
 *          failed but the RAM/RTC mirror was updated, the next boot
 *          will not be able to re-program the (now-erased?) record -
 *          the RAM fallback in @ref boot_selection_mark_rejected covers
 *          that case.
 *
 * @param[in,out] selection  Context; slot trial count advanced as
 *                           applicable.
 *
 * @srs     TRY-002, P2-6
 */
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

/**
 * @brief   Clear the trial mirror (TRY-006).
 *
 * @details Called when the slot being launched has just been confirmed
 *          (exit-trial handshake from the application) - the trial
 *          window is over and the RTC mirror is zeroed so the next
 *          pending slot starts from a clean count.
 *
 * @srs     TRY-006
 */
void boot_selection_clear_trial(void)
{
    *bkp(0) = 0U;
    __DSB();
}

/**
 * @brief   Read and clear the application's stay-in-BOOT request
 *          (ENT-002).
 *
 * @details Atomic read-and-clear of RTC_BKP1R: the magic is consumed
 *          on read so the intent does not survive into the next, unrelated
 *          boot (e.g. after `Activate` resets the board, an old request
 *          must not re-enter update mode).
 *
 * @return  `true` if a valid request was present (and has now been
 *          cleared), `false` otherwise.
 *
 * @srs     ENT-002
 */
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

/**
 * @brief   Write the hand-over record to RTC_BKP2R (HND-003).
 *
 * @details Called just before @ref boot_handover.  The record carries
 *          the slot index (1 = A, 2 = B per boot_contract.h), the
 *          pending flag, and the saturated trial count - everything the
 *          application and the host need to know about the slot that
 *          was launched.
 *
 * @param[in] selection  Context to publish.
 *
 * @srs     HND-003
 */
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

/**
 * @brief   Record the last selection outcome and rejected slot
 *          (TRY-008).  See selection.h for the register layout.
 *
 * @param[in] outcome   One of the @ref BOOT_OUTCOME_* codes.
 * @param[in] rejected  Slot index of the rejected slot, or 0 for none.
 *
 * @srs     TRY-008
 */
void boot_outcome_set(uint8_t outcome, boot_slot_t rejected)
{
    uint32_t raw = BOOT_OUTCOME_MAGIC;
    raw |= ((uint32_t)outcome & BOOT_OUTCOME_MASK) << BOOT_OUTCOME_SHIFT;
    raw |= ((uint32_t)rejected & BOOT_OUTCOME_REJECT_MASK)
           << BOOT_OUTCOME_REJECT_SHIFT;
    *bkp(3) = raw;
    __DSB();
}

/**
 * @brief   Clear the last-outcome record (RTC_BKP3R).
 *
 * @srs     TRY-008
 */
void boot_outcome_clear(void)
{
    *bkp(3) = 0U;
    __DSB();
}

/**
 * @brief   Read the last selection outcome and rejected slot
 *          (TRY-008).
 *
 * @param[out] rejected  On exit, holds the rejected slot index, or is
 *                       left untouched when `NULL`.  When no record is
 *                       present, *rejected is set to `BOOT_SLOT_A` as
 *                       a neutral default (slot A) for the caller.
 * @return  One of the @ref BOOT_OUTCOME_* codes; `BOOT_OUTCOME_UNKNOWN`
 *          when the record is absent or its magic does not match.
 *
 * @srs     TRY-008
 */
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
