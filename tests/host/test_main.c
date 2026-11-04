/*
 * test_main.c - host unit tests for the boot selection/update state
 *
 * Builds src/app/selection.c and src/app/update.c against an in-memory
 * 2 MB dual-bank flash and fake RTC backup registers.  Covers torn records,
 * trial persistence, the P0-1 update-during-trial guard, sequence
 * saturation and the P0-2 RAM rejection rescan.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "boot_layout.h"
#include "crc32.h"
#include "fake_flash.h"
#include "flash.h"
#include "image.h"
#include "metadata.h"
#include "selection.h"
#include "update.h"

static int failures;

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,         \
                    #condition);                                            \
            failures++;                                                     \
        }                                                                   \
    } while (0)

void fake_hal_reset(void);

enum slot_state {
    SLOT_VERIFIED = 0,
    SLOT_PENDING = 1,
    SLOT_CONFIRMED = 2
};

static void make_slot(boot_slot_t slot, uint32_t sequence,
                      enum slot_state state)
{
    uint8_t body[16];
    memset(body, 0, sizeof(body));

    uint32_t sp = BOOT_RAM_END;
    uint32_t entry = BOOT_APP_LOAD_ADDR + 0x101U;
    memcpy(&body[0], &sp, sizeof(sp));
    memcpy(&body[4], &entry, sizeof(entry));

    boot_flash_set_active(boot_other_slot(slot), true);
    CHECK(boot_flash_erase_meta(slot));
    CHECK(boot_flash_erase_slot_sector(slot, 0U));
    CHECK(boot_flash_program_slot(slot, 0U, body, sizeof(body), NULL));

    boot_image_header_t header;
    memset(&header, 0, sizeof(header));
    header.image_size = sizeof(body);
    header.image_crc32 = boot_crc32(body, sizeof(body));
    header.entry_address = entry;
    header.build_timestamp = 1U;
    memcpy(header.name, "test", 4U);
    boot_image_header_finalize(&header);

    CHECK(boot_meta_write_header(slot, &header));
    CHECK(boot_meta_write_record(slot, BOOT_META_VERIFIED_OFFSET,
                                 BOOT_META_VERIFIED_MAGIC,
                                 header.image_crc32));
    if (state >= SLOT_PENDING) {
        CHECK(boot_meta_write_record(slot, BOOT_META_ACTIVATED_OFFSET,
                                     BOOT_META_ACTIVATED_MAGIC, sequence));
    }
    if (state >= SLOT_CONFIRMED) {
        CHECK(boot_meta_write_record(slot, BOOT_META_CONFIRMED_OFFSET,
                                     BOOT_META_CONFIRMED_MAGIC, 0U));
    }
}

static boot_image_desc_t make_desc(void)
{
    boot_image_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.magic = BOOT_IMAGE_MAGIC;
    desc.header_version = BOOT_IMAGE_HEADER_VERSION;
    desc.header_size = BOOT_IMAGE_HEADER_SIZE;
    desc.image_type = BOOT_IMAGE_TYPE_APP;
    desc.version = 0x00010000U;
    desc.image_size = 16U;
    desc.image_crc32 = 0U;
    desc.load_address = BOOT_APP_LOAD_ADDR;
    desc.entry_address = BOOT_APP_LOAD_ADDR + 0x101U;
    desc.build_timestamp = 1U;
    memcpy(desc.name, "update", 6U);
    desc.name_len = 6U;
    return desc;
}

static void test_metadata_records(void)
{
    fake_flash_reset();
    fake_hal_reset();
    boot_flash_set_active(BOOT_SLOT_B, true);   /* target = A */

    CHECK(boot_meta_write_record(BOOT_SLOT_A, BOOT_META_VERIFIED_OFFSET,
                                 BOOT_META_VERIFIED_MAGIC, 0x12345678U));
    uint32_t value = 0U;
    bool present = false;
    CHECK(boot_meta_read_record(BOOT_SLOT_A, BOOT_META_VERIFIED_OFFSET,
                                BOOT_META_VERIFIED_MAGIC, &value, &present));
    CHECK(present && value == 0x12345678U);

    /* Torn record: correct magic, bad CRC. */
    uint32_t torn[8] = {0};
    torn[0] = BOOT_META_ACTIVATED_MAGIC;
    torn[1] = 7U;
    fake_flash_write_raw(boot_meta_base(BOOT_SLOT_A) +
                             BOOT_META_ACTIVATED_OFFSET,
                         torn, sizeof(torn));
    CHECK(!boot_meta_read_record(BOOT_SLOT_A, BOOT_META_ACTIVATED_OFFSET,
                                 BOOT_META_ACTIVATED_MAGIC, NULL, &present));
    CHECK(!present);

    CHECK(boot_meta_write_trial(BOOT_SLOT_A, 1U));
    CHECK(boot_meta_write_trial(BOOT_SLOT_A, 2U));
    CHECK(boot_meta_trial_count(BOOT_SLOT_A) == 2U);

    /* A torn next trial record ends the contiguous run at 2. */
    memset(torn, 0, sizeof(torn));
    torn[0] = BOOT_META_TRIAL_MAGIC;
    torn[1] = 3U;
    fake_flash_write_raw(boot_meta_base(BOOT_SLOT_A) +
                             BOOT_META_TRIAL_OFFSET +
                             (2U * BOOT_META_RECORD_SIZE),
                         torn, sizeof(torn));
    CHECK(boot_meta_trial_count(BOOT_SLOT_A) == 2U);
}

static void test_trial_limit_and_ram_rejection(void)
{
    fake_flash_reset();
    fake_hal_reset();
    make_slot(BOOT_SLOT_A, 1U, SLOT_CONFIRMED);
    make_slot(BOOT_SLOT_B, 2U, SLOT_PENDING);

    boot_flash_set_active(BOOT_SLOT_A, true);
    boot_selection_t selection;
    boot_selection_scan(&selection);
    boot_selection_choose(&selection);
    CHECK(selection.active_valid);
    CHECK(selection.active == BOOT_SLOT_B);
    CHECK(selection.active_pending);

    /* P2-6: the trial counter is written to flash on each attempt. */
    boot_selection_start_trial(&selection);
    boot_selection_start_trial(&selection);
    boot_selection_start_trial(&selection);
    CHECK(selection.trial_count == 3U);
    CHECK(boot_meta_trial_count(BOOT_SLOT_B) == 3U);

    /* Simulate a metadata sector whose REJECTED record cannot be written. */
    fake_flash_fail_meta_offset(BOOT_META_REJECTED_OFFSET, true);
    CHECK(!boot_selection_apply_trial_limit(&selection));
    boot_selection_mark_rejected(&selection, BOOT_SLOT_B,
                                 BOOT_REJECT_TRIAL_LIMIT);
    boot_selection_scan(&selection);
    boot_selection_choose(&selection);
    CHECK(selection.active == BOOT_SLOT_A);
    CHECK(selection.slot[BOOT_SLOT_B].rejected);

    /* P0-2: a rescan (GetBootInfo) must not make B bootable again. */
    boot_selection_scan(&selection);
    boot_selection_choose(&selection);
    CHECK(selection.active == BOOT_SLOT_A);
    CHECK(selection.slot[BOOT_SLOT_B].rejected);
    CHECK(!selection.slot[BOOT_SLOT_B].valid);

    /* With a working REJECTED write, the decision is persistent in flash. */
    fake_flash_reset();
    fake_hal_reset();
    memset(&selection, 0, sizeof(selection));
    make_slot(BOOT_SLOT_A, 1U, SLOT_CONFIRMED);
    make_slot(BOOT_SLOT_B, 2U, SLOT_PENDING);
    boot_flash_set_active(BOOT_SLOT_A, true);
    boot_selection_scan(&selection);
    boot_selection_choose(&selection);
    boot_selection_start_trial(&selection);
    boot_selection_start_trial(&selection);
    boot_selection_start_trial(&selection);
    boot_selection_scan(&selection);
    boot_selection_choose(&selection);
    CHECK(selection.trial_count == 3U);
    CHECK(boot_selection_apply_trial_limit(&selection));
    boot_selection_scan(&selection);
    boot_selection_choose(&selection);
    CHECK(selection.active == BOOT_SLOT_A);
    CHECK(selection.slot[BOOT_SLOT_B].rejected);
    CHECK(selection.slot[BOOT_SLOT_B].rejected_reason ==
          BOOT_REJECT_TRIAL_LIMIT);
}

static void test_update_during_trial(void)
{
    fake_flash_reset();
    fake_hal_reset();
    make_slot(BOOT_SLOT_A, 1U, SLOT_CONFIRMED);
    make_slot(BOOT_SLOT_B, 2U, SLOT_PENDING);

    boot_flash_set_active(BOOT_SLOT_A, true);
    boot_selection_t selection;
    boot_selection_scan(&selection);
    boot_selection_choose(&selection);
    CHECK(selection.active == BOOT_SLOT_B);
    CHECK(selection.active_pending);
    boot_flash_set_active(selection.active, selection.active_valid);

    boot_update_t update;
    boot_update_reset(&update);
    boot_image_desc_t desc = make_desc();
    char detail[64];

    /* P0-1: A is the rollback image; it must not be erased. */
    CHECK(boot_update_begin(&update, &selection, &desc, false, detail,
                            sizeof(detail)) == BOOT_ERR_BAD_STATE);
    CHECK(boot_meta_header_present(BOOT_SLOT_A));

    /* Bench override. */
    CHECK(boot_update_begin(&update, &selection, &desc, true, detail,
                            sizeof(detail)) == BOOT_ERR_NONE);

    /* Activate is refused while the active slot is on trial. */
    boot_update_t activate_update;
    boot_update_reset(&activate_update);
    CHECK(boot_update_activate(&activate_update, &selection, detail,
                               sizeof(detail)) == BOOT_ERR_BAD_STATE);
}

static void test_sequence_saturation(void)
{
    fake_flash_reset();
    fake_hal_reset();
    make_slot(BOOT_SLOT_A, 0xFFFFFFFEU, SLOT_CONFIRMED);
    make_slot(BOOT_SLOT_B, 0U, SLOT_VERIFIED);

    boot_flash_set_active(BOOT_SLOT_A, true);
    boot_selection_t selection;
    boot_selection_scan(&selection);
    boot_selection_choose(&selection);
    CHECK(selection.active == BOOT_SLOT_A);
    CHECK(!selection.active_pending);

    boot_update_t update;
    boot_update_reset(&update);
    char detail[64];
    CHECK(boot_update_activate(&update, &selection, detail, sizeof(detail)) ==
          BOOT_ERR_NONE);

    uint32_t sequence = 0U;
    bool present = false;
    CHECK(boot_meta_read_record(BOOT_SLOT_B, BOOT_META_ACTIVATED_OFFSET,
                                BOOT_META_ACTIVATED_MAGIC, &sequence,
                                &present));
    CHECK(present);
    CHECK(sequence == 0xFFFFFFFEU);
}

int main(void)
{
    test_metadata_records();
    test_trial_limit_and_ram_rejection();
    test_update_during_trial();
    test_sequence_saturation();

    if (failures != 0) {
        fprintf(stderr, "host tests: %d failure(s)\n", failures);
        return 1;
    }
    printf("host tests: all checks passed\n");
    return 0;
}
