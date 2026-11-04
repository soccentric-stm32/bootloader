/*
 * selection.h - metadata scan, A/B selection and trial state (sections 6, 7)
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * Target:  STM32F767ZIT6
 * SRS:     SEL-001..SEL-005, SWP-001, TRY-001..TRY-008, HND-003
 */

#ifndef SELECTION_H
#define SELECTION_H

#include <stdbool.h>
#include <stdint.h>
#include "boot_layout.h"
#include "image.h"

#define BOOT_TRIAL_LIMIT          3U

/* RTC_BKP0R: magic + slot + trial count. */
#define BOOT_TRIAL_MAGIC          0x00545249UL   /* "TRI" */
#define BOOT_TRIAL_MAGIC_MASK     0x00FFFFFFUL
#define BOOT_TRIAL_SLOT_SHIFT     24U
#define BOOT_TRIAL_SLOT_MASK      0x3UL
#define BOOT_TRIAL_COUNT_SHIFT    26U
#define BOOT_TRIAL_COUNT_MASK     0x3FUL

/* RTC_BKP1R: application's "stay in BOOT" request. */
#define BOOT_STAY_MAGIC           0x59415453UL   /* "STAY" */

/* RTC_BKP2R: hand-over record, shared with the application. */
#define BOOT_HANDOVER_MAGIC       0x00444E48UL   /* "HND" */
#define BOOT_HANDOVER_MAGIC_MASK  0x00FFFFFFUL
#define BOOT_HANDOVER_SLOT_SHIFT  24U
#define BOOT_HANDOVER_SLOT_MASK   0x3UL
#define BOOT_HANDOVER_PENDING_BIT 26U
#define BOOT_HANDOVER_TRIAL_SHIFT 27U
#define BOOT_HANDOVER_TRIAL_MASK  0x3UL

/* RTC_BKP3R: last boot outcome for GetBootInfo (TRY-008). */
#define BOOT_OUTCOME_UNKNOWN      0U
#define BOOT_OUTCOME_BOOTED       1U
#define BOOT_OUTCOME_ROLLED_BACK  2U
#define BOOT_OUTCOME_UPDATE_MODE  3U

#define BOOT_OUTCOME_MAGIC        0x004F5554UL   /* "OUT" */
#define BOOT_OUTCOME_MAGIC_MASK   0x00FFFFFFUL
#define BOOT_OUTCOME_SHIFT        24U
#define BOOT_OUTCOME_MASK         0xFUL
#define BOOT_OUTCOME_REJECT_SHIFT 28U
#define BOOT_OUTCOME_REJECT_MASK  0x3UL

typedef struct {
    bool header_present;
    bool header_valid;
    bool verified;
    bool activated;
    bool confirmed;
    bool rejected;
    bool sequence_valid;
    bool vectors_ok;
    bool crc_ok;
    bool valid;
    uint32_t sequence;
    uint32_t rejected_reason;
    uint32_t verified_crc;
    boot_image_header_t header;
} boot_slot_info_t;

typedef struct {
    boot_slot_info_t slot[2];
    boot_slot_t active;
    bool active_valid;
    bool active_pending;
    uint8_t trial_count;
} boot_selection_t;

/* Enable backup-domain access (PWR clock + DBP).  Call once at start-up. */
void boot_selection_init(void);

/* Read both metadata sectors and CRC-check the slots (SEL-001). */
void boot_selection_scan(boot_selection_t *selection);

/* Highest sequence number among valid slots (SEL-002). */
void boot_selection_choose(boot_selection_t *selection);

/* TRY-003: reject the selected pending slot when its trial limit is
 * reached and remember the rollback outcome.  Returns true when the caller
 * has to scan and choose again. */
bool boot_selection_apply_trial_limit(boot_selection_t *selection);

/* TRY-002: increment the trial counter before a pending hand-over. */
void boot_selection_start_trial(boot_selection_t *selection);

/* TRY-006: a confirmed slot clears the trial counter. */
void boot_selection_clear_trial(void);

/* ENT-002: read and clear the "stay in BOOT" request. */
bool boot_selection_take_stay_request(void);

/* HND-003: publish the hand-over record in RTC_BKP2R. */
void boot_selection_write_handover(const boot_selection_t *selection);

/* TRY-008: last boot outcome and rejected slot. */
void boot_outcome_set(uint8_t outcome, boot_slot_t rejected);
void boot_outcome_clear(void);
uint8_t boot_outcome_get(boot_slot_t *rejected);

#endif /* SELECTION_H */
