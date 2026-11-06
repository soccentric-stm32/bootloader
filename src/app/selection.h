/**
 * @file    selection.h
 * @brief   Metadata scan, A/B slot selection, and trial/rollback state
 *          (SRS sections 6 and 7).
 *
 * @details
 * The selection module owns every decision about *which* image the
 * bootloader launches and *whether* an update / rollback is allowed.  It
 *
 * - scans the signed metadata of both slots (@ref boot_selection_scan
 *   and @ref boot_selection_scan_full),
 * - ranks slots per the selection rules (SEL-*),
 * - tracks the per-slot trial counters that implement the rollback
 *   window (TRY-*),
 * - enforces the anti-rollback floor (BSAFE-006) derived from the signed
 *   security counters in the slot headers,
 * - exposes the small RTC backup-register interface used to coordinate
 *   with the application (stay-request, hand-over record, last outcome)
 *   and with the host (diagnostics, @ref boot_outcome_get).
 *
 * The module is stateless except for the caller-supplied
 * @ref boot_selection_t context; the only persistent state is the set of
 * `RTC_BKPnR` registers, whose layouts are documented below.
 *
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  STM32F767ZIT6
 * @srs     SEL-001..SEL-005, SWP-001, TRY-001..TRY-008, HND-003,
 *          BSAFE-006
 */

#ifndef SELECTION_H
#define SELECTION_H

#include <stdbool.h>
#include <stdint.h>
#include "boot_layout.h"
#include "image.h"

/**
 * @name Trial / backup-register constants.
 * @brief Layout and limits for the RTC backup registers and the trial
 *        window.
 * @{
 */

/**
 * @brief Maximum number of trial boots a pending image may attempt
 *        before it is rejected (rollback) after this many tries.
 *
 * TRY-003: on the (N+1)-th attempt, the slot is rejected and the
 * selection falls back to the other slot.
 */
#define BOOT_TRIAL_LIMIT          3U

/**
 * @name RTC_BKP0R - trial state (magic | slot | count)
 * @brief Packed layout of the trial-state backup register.
 *
 * Bit layout (32-bit register, LSB = bit 0):
 *
 * | bits      | field         | notes                                |
 * |-----------|---------------|--------------------------------------|
 * | 0..23     | magic         | @ref BOOT_TRIAL_MAGIC ("TRI")        |
 * | 24..25    | slot          | 0 = slot A, 1 = slot B               |
 * | 26..31    | trial count   | 0..63, saturates; only meaningful
 * |           |               | while the slot is still pending     |
 *
 * The pair of macros below is the canonical way to *read* the magic
 * (masked comparison); the writes always use the full register value
 * the module assembles in @ref boot_selection_start_trial, so the
 * individual field writers are not exposed in the public API.
 * @{
 */

/** Magic "TRI" written in the low 24 bits of RTC_BKP0R. */
#define BOOT_TRIAL_MAGIC          0x00545249UL   /* "TRI" */

/** Mask for the trial-state magic; compare against @ref BOOT_TRIAL_MAGIC. */
#define BOOT_TRIAL_MAGIC_MASK     0x00FFFFFFUL

/** Shift of the 2-bit slot field in RTC_BKP0R (bits 24-25). */
#define BOOT_TRIAL_SLOT_SHIFT     24U

/** Mask of the slot field (2 bits, value 0 or 1). */
#define BOOT_TRIAL_SLOT_MASK      0x3UL

/** Shift of the 6-bit trial counter field in RTC_BKP0R (bits 26-31). */
#define BOOT_TRIAL_COUNT_SHIFT    26U

/** Mask of the 6-bit trial counter field (values 0..63). */
#define BOOT_TRIAL_COUNT_MASK     0x3FUL

/** @} */

/**
 * @name RTC_BKP1R - application "stay in BOOT" request
 * @brief The application writes this magic to RTC_BKP1R before
 *        resetting, asking the bootloader to (re-)enter update mode on
 *        the next boot (ENT-002).
 * @{
 */

/** Magic "STAY" written by the application in RTC_BKP1R. */
#define BOOT_STAY_MAGIC           0x59415453UL   /* "STAY" */

/** @} */

/**
 * @name RTC_BKP2R - hand-over record, shared with the application
 * @brief Published by @ref boot_selection_write_handover before each
 *        jump; read by the application (and by the host via
 *        GetBootInfo) to know the slot it started from and whether it
 *        is still in its trial window.
 *
 * Bit layout of the 32-bit register:
 *
 * | bits      | field             | notes                                |
 * |-----------|-------------------|--------------------------------------|
 * | 0..23     | magic             | @ref BOOT_HANDOVER_MAGIC ("HND")     |
 * | 24..25    | slot              | 1 = A, 2 = B (per boot_contract.h)   |
 * | 26        | pending flag      | 1 = slot still in trial window       |
 * | 27..28    | trial count       | 0..3 (saturated by the limiter)      |
 * | 29..31    | reserved / 0      | must be written as zero              |
 * @{
 */

/** Magic "HND" written in the low 24 bits of RTC_BKP2R. */
#define BOOT_HANDOVER_MAGIC       0x00444E48UL   /* "HND" */

/** Mask for the hand-over magic; compare against @ref BOOT_HANDOVER_MAGIC. */
#define BOOT_HANDOVER_MAGIC_MASK  0x00FFFFFFUL

/** Shift of the 2-bit slot field in RTC_BKP2R (bits 24-25). */
#define BOOT_HANDOVER_SLOT_SHIFT  24U

/** Mask of the slot field (2 bits, value 0 or 1). */
#define BOOT_HANDOVER_SLOT_MASK   0x3UL

/** Bit 26: 1 when the launched slot is still "pending" (in trial window). */
#define BOOT_HANDOVER_PENDING_BIT 26U

/** Shift of the 2-bit saturated trial count in RTC_BKP2R (bits 27-28). */
#define BOOT_HANDOVER_TRIAL_SHIFT 27U

/** Mask of the 2-bit saturated trial count (values 0..3). */
#define BOOT_HANDOVER_TRIAL_MASK  0x3UL

/** @} */

/**
 * @name Last boot outcome codes (RTC_BKP3R, low byte)
 * @brief Discrete outcomes recorded by @ref boot_outcome_set after each
 *        selection decision; surfaced by @ref boot_outcome_get and used
 *        by the HWLINK `GetBootInfo` diagnostic (TRY-008).
 * @{
 */

/** Outcome not yet recorded (first boot, or cleared by a host). */
#define BOOT_OUTCOME_UNKNOWN      0U
/** A slot was selected and launched. */
#define BOOT_OUTCOME_BOOTED       1U
/** The selected slot was rejected after trial limit (rollback). */
#define BOOT_OUTCOME_ROLLED_BACK  2U
/** The bootloader is / was left in update mode (no bootable image). */
#define BOOT_OUTCOME_UPDATE_MODE  3U

/** @} */

/**
 * @name RTC_BKP3R - last boot outcome (magic | outcome | rejected slot)
 * @brief Packed layout of the outcome backup register.
 *
 * Bit layout (32-bit register, LSB = bit 0):
 *
 * | bits      | field         | notes                                |
 * |-----------|---------------|--------------------------------------|
 * | 0..23     | magic         | @ref BOOT_OUTCOME_MAGIC ("OUT")      |
 * | 24..27    | outcome code  | one of the @ref BOOT_OUTCOME_* codes |
 * | 28..29    | rejected slot | slot index that was rejected         |
 * | 30..31    | reserved / 0  | must be written as zero              |
 * @{
 */

/** Magic "OUT" written in the low 24 bits of RTC_BKP3R. */
#define BOOT_OUTCOME_MAGIC        0x004F5554UL   /* "OUT" */

/** Mask for the outcome magic; compare against @ref BOOT_OUTCOME_MAGIC. */
#define BOOT_OUTCOME_MAGIC_MASK   0x00FFFFFFUL

/** Shift of the 4-bit outcome code in RTC_BKP3R (bits 24-27). */
#define BOOT_OUTCOME_SHIFT        24U

/** Mask of the 4-bit outcome code (values 0..15). */
#define BOOT_OUTCOME_MASK         0xFUL

/** Shift of the 2-bit rejected-slot index in RTC_BKP3R (bits 28-29). */
#define BOOT_OUTCOME_REJECT_SHIFT 28U

/** Mask of the 2-bit rejected-slot index (values 0..3). */
#define BOOT_OUTCOME_REJECT_MASK  0x3UL

/** @} */

/** @} */

/**
 * @brief Per-slot scan result, filled by @ref boot_selection_scan.
 *
 * @details One instance per physical slot (A and B).  Every boolean is
 *        a *positive* signal for that aspect; `valid` is the final
 *        composite gate.  The struct is passed by reference to the
 *        selection, protocol and update module, so the fields double as
 *        the diagnostic surface for the HWLINK `GetSlot` and
 *        `GetBootInfo` endpoints.
 */
typedef struct {
    /** @brief True when the slot's image-header region was readable. */
    bool header_present;
    /** @brief True when the image header's fields (magic, version, size)
     *         are internally consistent. */
    bool header_valid;
    /** @brief Discriminator the authenticator returned for the header
     *         (see @ref boot_header_status_t) - diagnostic only, the
     *         gate is `header_valid`. */
    boot_header_status_t header_status;   /* BSAFE-006 diagnostics      */
    /** @brief Signature (ECDSA P-256 over the SHA-256 digest) verified
     *         against the trusted public key. */
    bool verified;
    /** @brief True after a successful `Activate` in this boot (or a
     *         previous boot), i.e. the slot is "armed" to boot. */
    bool activated;
    /** @brief True after the application confirmed the slot (exit-trial
     *         handshake), i.e. the slot is "confirmed". */
    bool confirmed;
    /** @brief True when the slot has been rejected (trial limit or
     *         manual reject); rejected slots are never booted or
     *         targeted. */
    bool rejected;
    /** @brief True when the boot/activate/confirm sequence for this
     *         slot is internally consistent. */
    bool sequence_valid;
    /** @brief True when the vector table (initial SP, reset vector) at
     *         the slot base lies inside the slot and the reset vector
     *         lies inside the reset vector's expected range. */
    bool vectors_ok;
    /** @brief True when the image CRC32 check was attempted (i.e. the
     *         header declared a CRC and the region was readable). */
    bool crc_checked;
    /** @brief True when the image CRC32 matches. */
    bool crc_ok;
    /** @brief True when the SHA-256 digest of the slot content matches
     *         the digest committed by the signer (BSAFE-006). */
    bool digest_ok;      /* BSAFE-006: SHA-256 of the slot matches       */
    /** @brief True when the slot's security counter would fall *below*
     *         the anti-rollback floor - such a slot is refused even if
     *         everything above is true (BSAFE-006 anti-rollback). */
    bool below_floor;    /* BSAFE-006: security counter was superseded   */
    /** @brief Composite gate: `valid` is true only when every aspect
     *         required by the SRS is satisfied. */
    bool valid;
    /** @brief Image sequence number advertised by the (valid) header;
     *         0 if none. */
    uint32_t sequence;
    /** @brief Slot-rejection reason code (module-internal, see selection
     *         for the code table) when `rejected` is true; 0 otherwise. */
    uint32_t rejected_reason;
    /** @brief The CRC32 value the module computed over the slot (for
     *         diagnostics, regardless of whether it matched). */
    uint32_t verified_crc;
    /** @brief Trial count observed in the trial-state record for this
     *         slot (0 if none or if the slot is confirmed / rejected). */
    uint8_t trial_count;
    /** @brief Copy of the (validated or not) image header for the host
     *         to read via the protocol endpoints. */
    boot_image_header_t header;
} boot_slot_info_t;

/**
 * @brief Whole-boot selection context, filled by @ref boot_selection_scan
 *        and mutated by the choose / trial / anti-rollback helpers.
 *
 * @details A single instance is created per boot in @ref main and
 *        threaded through to every module that needs the decision.
 */
typedef struct {
    /** @brief Per-slot scan results; index 0 = slot A, index 1 = slot B
     *         (mapping defined by @ref boot_slot_t). */
    boot_slot_info_t slot[2];
    /** @brief The slot @ref boot_selection_choose selected to launch. */
    boot_slot_t active;
    /** @brief True when @ref active actually has a valid image. */
    bool active_valid;
    /** @brief True when @ref active is still in its trial window (the
     *         application has not confirmed it yet). */
    bool active_pending;
    /** @brief Trial counter for @ref active (0 when not pending). */
    uint8_t trial_count;
    /**
     * @brief Bitmask of slots that must be treated as rejected *in RAM
     *        for this boot*, because the persistent REJECTED record
     *        could not be programmed (P0-2).
     *
     * @details Survives rescans: every later @ref boot_selection_scan
     *        re-applies this mask so that `GetBootInfo` (or a fresh
     *        `UpdateBegin`) cannot make a trial-exhausted slot bootable
     *        or updatable again in the middle of the same boot.
     */
    uint8_t ram_rejected_mask;
} boot_selection_t;

/**
 * @brief   Enable backup-domain access (PWR clock on + DBP deasserted).
 *
 * @details Idempotent; must be called once at start-up before any
 *          other selection-module function that reads/writes the
 *          RTC backup registers.
 *
 * @srs     prerequisite for every RTC_BKPnR consumer
 */
void boot_selection_init(void);

/**
 * @brief   Fast scan of both slots (boot path).
 *
 * @details P2-7: the fast path only reads the metadata and vectors of
 *          *both* slots (cheap), and computes the image CRC only for the
 *          best candidate - and for the fallback only if the best
 *          fails.  This keeps the boot time tight while still giving a
 *          correct decision.
 *
 * @param[in,out] selection  Selection context to fill; previous
 *                           contents are fully overwritten (except
 *                           `ram_rejected_mask`, which is preserved).
 *
 * @srs     SEL-001, P2-7
 */
void boot_selection_scan(boot_selection_t *selection);

/**
 * @brief   Full scan of both slots (diagnostic path).
 *
 * @details Same contract as @ref boot_selection_scan but with the
 *          image CRC of *both* slots always computed - used by the
 *          HWLINK `GetBootInfo` endpoint where the extra cost is
 *          acceptable and the host wants a complete picture.
 *
 * @param[in,out] selection  Selection context to fill.
 *
 * @srs     SEL-001 (diagnostics variant)
 */
void boot_selection_scan_full(boot_selection_t *selection);

/**
 * @brief   Re-read the slot details without re-running the choose step.
 *
 * @details Used by the `GetBootInfo` diagnostic while an update session
 *          may be open: refreshing the slot-level booleans keeps the
 *          host's view up to date, but the active-slot decision is
 *          left alone so that an in-flight update target is not
 *          accidentally invalidated mid-transfer (P0-2/P2-7).
 *
 * @param[in,out] selection  Context to refresh.
 */
void boot_selection_refresh_slots(boot_selection_t *selection);

/**
 * @brief   Reject a slot in RAM for the rest of this boot (P0-2).
 *
 * @details Sets the bit in `selection->ram_rejected_mask` and records
 *          the reason in the slot's `rejected_reason`.  The mask is
 *          re-applied by every later @ref boot_selection_scan, so the
 *          decision survives the "rescan-and-choose" sequence used
 *          after trial-limit handling and after update-mode completion.
 *          It is, by design, *not* persisted here - the persistent
 *          REJECTED record is the responsibility of the caller (which is
 *          where the flash write is permitted).
 *
 * @param[in,out] selection  Context to update.
 * @param[in]     slot       Slot to reject.
 * @param[in]     reason     Reason code stored in the slot's
 *                           `rejected_reason` for diagnostics.
 *
 * @srs     P0-2, TRY-003 (fallback path)
 */
void boot_selection_mark_rejected(boot_selection_t *selection,
                                  boot_slot_t slot, uint32_t reason);

/**
 * @brief   Anti-rollback security floor (BSAFE-006).
 *
 * @details Returns the highest security counter among slots whose
 *          header is authentic, which the application has *confirmed*,
 *          and which are not rejected.  No image with a security counter
 *          below this floor is booted, activated, or accepted by
 *          `UpdateBegin`.
 *
 * The floor lives in the signed headers themselves - it needs no OTP
 * or extra flash, and it rises automatically as newer images are
 * confirmed by the application.
 *
 * @param[in] selection  Context to derive the floor from.
 * @return  Highest confirmed-and-not-rejected security counter; 0 when
 *          no such slot exists (no floor).
 *
 * @srs     BSAFE-006
 */
uint32_t boot_selection_security_floor(const boot_selection_t *selection);

/**
 * @brief   Choose the active slot per the selection rules (SEL-002).
 *
 * @details Ranking preference (simplified):
 *
 * 1. activated-and-confirmed slot with the highest sequence number,
 * 2. activated-but-not-yet-confirmed slot (still in trial window)
 *    with the highest sequence number,
 * 3. rejected slots are never chosen.
 *
 * @param[in,out] selection  Filled on entry by a scan; `active`,
 *                           `active_valid`, `active_pending`,
 *                           `trial_count` are set on exit.
 *
 * @srs     SEL-002
 */
void boot_selection_choose(boot_selection_t *selection);

/**
 * @brief   Enforce the trial limit on the currently selected pending
 *          slot (TRY-003).
 *
 * @details If the selected slot is pending and its trial count has
 *          reached `BOOT_TRIAL_LIMIT`, the slot is rejected (record
 *          programmed when flash writes are allowed) and the outcome
 *          remembered.
 *
 * @param[in,out] selection  Context to update.
 * @return  `true`  when the caller must re-scan and re-choose (a slot
 *          was just rejected).
 * @return  `false` when no rejection occurred and the current
 *          selection stands.
 *
 * @srs     TRY-003
 */
bool boot_selection_apply_trial_limit(boot_selection_t *selection);

/**
 * @brief   Increment the trial counter for the pending slot about to be
 *          launched (TRY-002).
 *
 * @details Called just before the hand-over for a still-pending slot.
 *          No effect for a confirmed slot (the counter is cleared by
 *          @ref boot_selection_clear_trial at that transition).
 *
 * @param[in,out] selection  Context whose trial state is advanced.
 *
 * @srs     TRY-002
 */
void boot_selection_start_trial(boot_selection_t *selection);

/**
 * @brief   Clear the trial counter when a slot is confirmed (TRY-006).
 *
 * @details "Confirmed" is the application handshake that says "this
 *          image is working, keep it" - the trial window is over and
 *          the counter is reset ready for the next pending slot.
 *
 * @srs     TRY-006
 */
void boot_selection_clear_trial(void);

/**
 * @brief   Read and clear the application's "stay in BOOT" request
 *          (ENT-002).
 *
 * @details The application sets the magic in RTC_BKP1R before a reset
 *          to ask the bootloader to (re)enter update mode.  This call
 *          atomically consumes the request - the register is cleared -
 *          so the intent does not survive into a subsequent, unrelated
 *          boot.
 *
 * @return  `true`  when a valid request was present (and has now been
 *          cleared), `false` otherwise.
 *
 * @srs     ENT-002
 */
bool boot_selection_take_stay_request(void);

/**
 * @brief   Publish the hand-over record into RTC_BKP2R (HND-003).
 *
 * @details Called just before the jump; gives the application (and the
 *          host, after the fact) the slot it started from and whether
 *          the slot was still pending.
 *
 * @param[in] selection  Context whose `active*` fields are published.
 *
 * @srs     HND-003
 */
void boot_selection_write_handover(const boot_selection_t *selection);

/**
 * @name Last boot outcome accessors (RTC_BKP3R)
 * @brief Record / read / clear the last selection outcome and the slot
 *        that was rejected (if any), for the HWLINK `GetBootInfo`
 *        diagnostic (TRY-008).
 * @{
 */

/**
 * @brief   Record the last boot outcome and, if any, the slot that was
 *          rejected in this boot.
 *
 * @param[in] outcome   One of the @ref BOOT_OUTCOME_* codes.
 * @param[in] rejected  Slot index of the rejected slot, or 0 when none.
 *
 * @srs     TRY-008
 */
void boot_outcome_set(uint8_t outcome, boot_slot_t rejected);

/**
 * @brief   Clear the last-boot outcome record (host-initiated reset of
 *          the diagnostic state).
 *
 * @srs     TRY-008
 */
void boot_outcome_clear(void);

/**
 * @brief   Read the last boot outcome.
 *
 * @param[out] rejected  On exit, holds the rejected slot index (0 when
 *                       none was recorded), or is left untouched when
 *                       `NULL`.
 * @return  One of the @ref BOOT_OUTCOME_* codes (UNKNOWN when no record
 *          is present or the magic does not match).
 *
 * @srs     TRY-008
 */
uint8_t boot_outcome_get(boot_slot_t *rejected);

/** @} */

#endif /* SELECTION_H */
