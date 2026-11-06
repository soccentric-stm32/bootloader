/*
 * test_main.c - host unit tests for the boot selection/update state
 *
 * Builds src/app/selection.c and src/app/update.c against an in-memory
 * 2 MB dual-bank flash and fake RTC backup registers.  Covers torn records,
 * trial persistence, the P0-1 update-during-trial guard, sequence
 * saturation and the P0-2 RAM rejection rescan.  BSAFE-006: SHA-256
 * vectors, signatures made by mkimage.py and by uECC_sign(), signature and
 * digest checks in the update path and at boot, and the anti-rollback
 * floor.
 */

#include <stdbool.h>
#include <stddef.h>
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
#include "sha256.h"
#include "signing_key.h"
#include "test_vectors.h"
#include "uECC.h"
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

static void make_body(uint8_t body[16])
{
    memset(body, 0, 16U);
    uint32_t sp = BOOT_RAM_END;
    uint32_t entry = BOOT_APP_LOAD_ADDR + 0x101U;
    memcpy(&body[0], &sp, sizeof(sp));
    memcpy(&body[4], &entry, sizeof(entry));
}

/* BSAFE-006: turn a finalized header into a signed version 2 header for
 * body, the way tools/mkimage.py does, with the host-test key. */
static void sign_header(boot_image_header_t *header, const uint8_t *body,
                        uint32_t body_len, uint32_t security_counter)
{
    header->header_version = (uint16_t)BOOT_IMAGE_HEADER_VERSION;
    header->security_counter = security_counter;
    header->key_id = boot_signing_key_id;
    boot_sha256(body, body_len, header->image_sha256);

    uint8_t digest[BOOT_SHA256_SIZE];
    boot_sha256(header, BOOT_IMAGE_SIGNED_SIZE, digest);
    CHECK(uECC_sign(test_signing_privkey, digest, sizeof(digest),
                    header->signature, uECC_secp256r1()) == 1);
    boot_image_header_finalize(header);
}

static void make_slot_counter(boot_slot_t slot, uint32_t sequence,
                              enum slot_state state,
                              uint32_t security_counter)
{
    uint8_t body[16];
    make_body(body);

    boot_flash_set_active(boot_other_slot(slot), true);
    CHECK(boot_flash_erase_meta(slot));
    CHECK(boot_flash_erase_slot_sector(slot, 0U));
    CHECK(boot_flash_program_slot(slot, 0U, body, sizeof(body), NULL));

    boot_image_header_t header;
    memset(&header, 0, sizeof(header));
    header.image_size = sizeof(body);
    header.image_crc32 = boot_crc32(body, sizeof(body));
    header.entry_address = BOOT_APP_LOAD_ADDR + 0x101U;
    header.build_timestamp = 1U;
    memcpy(header.name, "test", 4U);
    boot_image_header_finalize(&header);
    sign_header(&header, body, sizeof(body), security_counter);

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

static void make_slot(boot_slot_t slot, uint32_t sequence,
                      enum slot_state state)
{
    make_slot_counter(slot, sequence, state, 0U);
}

/* The header boot_update_begin() rebuilds from a descriptor. */
static boot_image_header_t desc_header(const boot_image_desc_t *desc)
{
    boot_image_header_t header;
    memset(&header, 0, sizeof(header));
    header.header_version  = desc->header_version;
    header.version         = desc->version;
    header.image_size      = desc->image_size;
    header.image_crc32     = desc->image_crc32;
    header.entry_address   = desc->entry_address;
    header.build_timestamp = desc->build_timestamp;
    memcpy(header.git_commit, desc->git_commit, sizeof(header.git_commit));
    memcpy(header.name, desc->name, sizeof(header.name));
    boot_image_header_finalize(&header);
    return header;
}

/* Sign a descriptor for body and attach the protocol minor 3 extension. */
static void sign_desc(boot_image_desc_t *desc, const uint8_t *body,
                      uint32_t body_len, uint32_t security_counter)
{
    boot_image_header_t header = desc_header(desc);
    sign_header(&header, body, body_len, security_counter);
    desc->header_version = header.header_version;
    memcpy(desc->extension, (const uint8_t *)&header + BOOT_IMAGE_EXT_OFFSET,
           BOOT_IMAGE_EXT_SIZE);
    desc->extension_present = true;
    desc->header_crc32 = header.header_crc32;
    desc->header_crc32_present = true;
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

    uint8_t body[16];
    make_body(body);
    desc.image_crc32 = boot_crc32(body, sizeof(body));
    sign_desc(&desc, body, sizeof(body), 0U);
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
    memset(&selection, 0, sizeof(selection));
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
    memset(&selection, 0, sizeof(selection));
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
    memset(&selection, 0, sizeof(selection));
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

static void test_sha256_vectors(void)
{
    for (size_t i = 0U; i < test_sha_vector_count; i++) {
        uint8_t digest[BOOT_SHA256_SIZE];
        boot_sha256(test_sha_vectors[i].message, test_sha_vectors[i].length,
                    digest);
        CHECK(memcmp(digest, test_sha_vectors[i].digest, sizeof(digest)) ==
              0);

        /* Byte-at-a-time updates cross every block boundary. */
        boot_sha256_t ctx;
        boot_sha256_init(&ctx);
        for (size_t j = 0U; j < test_sha_vectors[i].length; j++) {
            boot_sha256_update(&ctx, &test_sha_vectors[i].message[j], 1U);
        }
        boot_sha256_final(&ctx, digest);
        CHECK(memcmp(digest, test_sha_vectors[i].digest, sizeof(digest)) ==
              0);
    }
}

static void refresh_crc(boot_image_header_t *header)
{
    header->header_crc32 =
        boot_crc32(header, offsetof(boot_image_header_t, header_crc32));
}

static void test_header_signature(void)
{
    boot_image_header_t header;
    memcpy(&header, test_python_header, sizeof(header));

    /* A header signed by tools/mkimage.py verifies in C. */
    CHECK(boot_image_header_check(&header) == BOOT_HEADER_OK);
    CHECK(header.security_counter == 7U);
    CHECK(header.key_id == boot_signing_key_id);

    boot_image_header_t bad = header;
    bad.version ^= 1U;              /* inside the signed prefix */
    CHECK(boot_image_header_check(&bad) == BOOT_HEADER_BAD_FORMAT);
    refresh_crc(&bad);              /* only the signature can catch it */
    CHECK(boot_image_header_check(&bad) == BOOT_HEADER_BAD_SIGNATURE);

    bad = header;
    bad.security_counter = 8U;      /* raising the counter breaks it too */
    refresh_crc(&bad);
    CHECK(boot_image_header_check(&bad) == BOOT_HEADER_BAD_SIGNATURE);

    bad = header;
    bad.signature[10] ^= 0x01U;
    refresh_crc(&bad);
    CHECK(boot_image_header_check(&bad) == BOOT_HEADER_BAD_SIGNATURE);

    bad = header;
    bad.key_id ^= 1U;
    refresh_crc(&bad);
    CHECK(boot_image_header_check(&bad) == BOOT_HEADER_WRONG_KEY);

    bad = header;
    bad.header_version = (uint16_t)BOOT_IMAGE_HEADER_VERSION_UNSIGNED;
    refresh_crc(&bad);
    CHECK(boot_image_header_check(&bad) == BOOT_HEADER_UNSIGNED);

    /* The reserved tail is outside the signature but inside the CRC. */
    bad = header;
    bad.reserved[0] = 1U;
    CHECK(boot_image_header_check(&bad) == BOOT_HEADER_BAD_FORMAT);
}

static void test_update_authentication(void)
{
    fake_flash_reset();
    fake_hal_reset();
    make_slot_counter(BOOT_SLOT_A, 1U, SLOT_CONFIRMED, 5U);
    make_slot(BOOT_SLOT_B, 0U, SLOT_VERIFIED);

    boot_flash_set_active(BOOT_SLOT_A, true);
    boot_selection_t selection;
    memset(&selection, 0, sizeof(selection));
    boot_selection_scan(&selection);
    boot_selection_choose(&selection);
    CHECK(selection.active == BOOT_SLOT_A);
    CHECK(boot_selection_security_floor(&selection) == 5U);

    boot_update_t update;
    char detail[64];
    uint8_t body[16];
    make_body(body);

    /* An unsigned image is refused before the target slot is erased. */
    boot_image_desc_t desc = make_desc();
    desc.header_version = (uint16_t)BOOT_IMAGE_HEADER_VERSION_UNSIGNED;
    desc.extension_present = false;
    desc.header_crc32_present = false;
    boot_update_reset(&update);
    CHECK(boot_update_begin(&update, &selection, &desc, false, detail,
                            sizeof(detail)) == BOOT_ERR_AUTH_FAILED);
    CHECK(boot_meta_header_present(BOOT_SLOT_B));

    /* A forged signature is refused, force or not. */
    desc = make_desc();
    sign_desc(&desc, body, sizeof(body), 5U);
    desc.extension[(offsetof(boot_image_header_t, signature) -
                    BOOT_IMAGE_EXT_OFFSET) + 3U] ^= 0x80U;
    desc.header_crc32_present = false;
    CHECK(boot_update_begin(&update, &selection, &desc, true, detail,
                            sizeof(detail)) == BOOT_ERR_AUTH_FAILED);
    CHECK(boot_meta_header_present(BOOT_SLOT_B));

    /* Anti-rollback: below the confirmed slot's counter is refused. */
    desc = make_desc();
    sign_desc(&desc, body, sizeof(body), 4U);
    CHECK(boot_update_begin(&update, &selection, &desc, true, detail,
                            sizeof(detail)) == BOOT_ERR_ROLLBACK);
    CHECK(boot_meta_header_present(BOOT_SLOT_B));

    /* Equal counter: accepted, written and verified end to end. */
    desc = make_desc();
    sign_desc(&desc, body, sizeof(body), 5U);
    CHECK(boot_update_begin(&update, &selection, &desc, false, detail,
                            sizeof(detail)) == BOOT_ERR_NONE);
    uint32_t next = 0U;
    CHECK(boot_update_data(&update, 0U, body, sizeof(body), &next, detail,
                           sizeof(detail)) == BOOT_ERR_NONE);
    uint32_t crc = 0U;
    CHECK(boot_update_end(&update, &crc, detail, sizeof(detail)) ==
          BOOT_ERR_NONE);
    CHECK(boot_update_activate(&update, &selection, detail,
                               sizeof(detail)) == BOOT_ERR_NONE);

    /* The signed SHA-256 must match the bytes that reached flash even when
     * the CRC32 does (the CRC is not a security check). */
    fake_flash_reset();
    fake_hal_reset();
    make_slot(BOOT_SLOT_A, 1U, SLOT_CONFIRMED);
    boot_flash_set_active(BOOT_SLOT_A, true);
    boot_selection_scan(&selection);
    boot_selection_choose(&selection);
    uint8_t other[16];
    make_body(other);
    other[12] = 0x5AU;
    desc = make_desc();
    sign_desc(&desc, other, sizeof(other), 0U);   /* digest of other */
    boot_update_reset(&update);
    CHECK(boot_update_begin(&update, &selection, &desc, false, detail,
                            sizeof(detail)) == BOOT_ERR_NONE);
    CHECK(boot_update_data(&update, 0U, body, sizeof(body), &next, detail,
                           sizeof(detail)) == BOOT_ERR_NONE);
    CHECK(boot_update_end(&update, &crc, detail, sizeof(detail)) ==
          BOOT_ERR_AUTH_FAILED);
    CHECK(strcmp(detail, "image SHA-256 mismatch") == 0);
}

/* Regression: the lazy erase compared slot-relative image offsets with
 * bank-relative sector offsets, so a slot that already held data kept its
 * first 48 KB and lost sector 0 again once the update passed 0xC000. */
static void test_update_over_used_slot(void)
{
    static uint8_t image[100U * 1024U];
    for (uint32_t i = 0U; i < sizeof(image); i++) {
        image[i] = (uint8_t)((i * 7U) ^ (i >> 8U));
    }
    uint8_t vectors[16];
    make_body(vectors);
    memcpy(image, vectors, 8U);

    fake_flash_reset();
    fake_hal_reset();
    make_slot(BOOT_SLOT_A, 1U, SLOT_CONFIRMED);
    /* Leave old, non-erased content across B's first two sectors. */
    fake_flash_write_raw(BOOT_SLOT_B_BASE, image + 1U, 0x14000U);
    boot_flash_set_active(BOOT_SLOT_A, true);

    boot_selection_t selection;
    memset(&selection, 0, sizeof(selection));
    boot_selection_scan(&selection);
    boot_selection_choose(&selection);

    boot_image_desc_t desc = make_desc();
    desc.image_size = sizeof(image);
    desc.image_crc32 = boot_crc32(image, sizeof(image));
    sign_desc(&desc, image, sizeof(image), 0U);

    boot_update_t update;
    boot_update_reset(&update);
    char detail[64];
    CHECK(boot_update_begin(&update, &selection, &desc, false, detail,
                            sizeof(detail)) == BOOT_ERR_NONE);
    uint32_t next = 0U;
    for (uint32_t offset = 0U; offset < sizeof(image); offset += 1000U) {
        uint32_t len = sizeof(image) - offset;
        if (len > 1000U) {
            len = 1000U;
        }
        CHECK(boot_update_data(&update, offset, &image[offset], len, &next,
                               detail, sizeof(detail)) == BOOT_ERR_NONE);
    }
    uint32_t crc = 0U;
    CHECK(boot_update_end(&update, &crc, detail, sizeof(detail)) ==
          BOOT_ERR_NONE);
    CHECK(crc == desc.image_crc32);
    CHECK(memcmp(fake_flash_ptr(BOOT_SLOT_B_BASE), image, sizeof(image)) ==
          0);
}

static void test_boot_authentication(void)
{
    fake_flash_reset();
    fake_hal_reset();
    make_slot(BOOT_SLOT_A, 1U, SLOT_CONFIRMED);
    make_slot(BOOT_SLOT_B, 2U, SLOT_CONFIRMED);

    /* Replace B's header with one whose CRC32 matches the slot but whose
     * signed digest does not: B must not boot. */
    uint8_t body[16];
    make_body(body);
    uint8_t other[16];
    make_body(other);
    other[15] = 0xA5U;
    boot_image_header_t header;
    CHECK(boot_meta_header_read(BOOT_SLOT_B, &header));
    sign_header(&header, other, sizeof(other), 0U);
    boot_flash_set_active(BOOT_SLOT_A, true);
    CHECK(boot_flash_erase_meta(BOOT_SLOT_B));
    CHECK(boot_meta_write_header(BOOT_SLOT_B, &header));
    CHECK(boot_meta_write_record(BOOT_SLOT_B, BOOT_META_VERIFIED_OFFSET,
                                 BOOT_META_VERIFIED_MAGIC,
                                 header.image_crc32));
    CHECK(boot_meta_write_record(BOOT_SLOT_B, BOOT_META_ACTIVATED_OFFSET,
                                 BOOT_META_ACTIVATED_MAGIC, 2U));
    CHECK(boot_meta_write_record(BOOT_SLOT_B, BOOT_META_CONFIRMED_OFFSET,
                                 BOOT_META_CONFIRMED_MAGIC, 0U));

    boot_selection_t selection;
    memset(&selection, 0, sizeof(selection));
    boot_selection_scan(&selection);
    boot_selection_choose(&selection);
    CHECK(selection.slot[BOOT_SLOT_B].crc_ok);
    CHECK(!selection.slot[BOOT_SLOT_B].digest_ok);
    CHECK(!selection.slot[BOOT_SLOT_B].valid);
    CHECK(selection.active_valid && selection.active == BOOT_SLOT_A);

    /* A slot written with an unsigned header is not bootable either. */
    fake_flash_reset();
    fake_hal_reset();
    memset(&selection, 0, sizeof(selection));
    make_slot(BOOT_SLOT_A, 1U, SLOT_CONFIRMED);
    CHECK(boot_meta_header_read(BOOT_SLOT_A, &header));
    header.header_version = (uint16_t)BOOT_IMAGE_HEADER_VERSION_UNSIGNED;
    memset(header.signature, 0, sizeof(header.signature));
    boot_image_header_finalize(&header);
    boot_flash_set_active(BOOT_SLOT_B, true);
    CHECK(boot_flash_erase_meta(BOOT_SLOT_A));
    CHECK(boot_meta_write_header(BOOT_SLOT_A, &header));
    CHECK(boot_meta_write_record(BOOT_SLOT_A, BOOT_META_ACTIVATED_OFFSET,
                                 BOOT_META_ACTIVATED_MAGIC, 1U));
    boot_selection_scan(&selection);
    boot_selection_choose(&selection);
    CHECK(selection.slot[BOOT_SLOT_A].header_status ==
          BOOT_HEADER_UNSIGNED);
    CHECK(!selection.active_valid);
}

static void test_security_floor(void)
{
    boot_selection_t selection;
    memset(&selection, 0, sizeof(selection));

    /* B has the newer sequence but an older counter than confirmed A. */
    fake_flash_reset();
    fake_hal_reset();
    memset(&selection, 0, sizeof(selection));
    make_slot_counter(BOOT_SLOT_A, 1U, SLOT_CONFIRMED, 5U);
    make_slot_counter(BOOT_SLOT_B, 2U, SLOT_PENDING, 4U);
    boot_flash_set_active(BOOT_SLOT_A, true);
    boot_selection_scan(&selection);
    boot_selection_choose(&selection);
    CHECK(selection.slot[BOOT_SLOT_B].below_floor);
    CHECK(selection.active == BOOT_SLOT_A && !selection.active_pending);

    /* A pending image with a higher counter boots on trial; the old
     * confirmed image stays as the rollback target until B confirms. */
    fake_flash_reset();
    fake_hal_reset();
    memset(&selection, 0, sizeof(selection));
    make_slot_counter(BOOT_SLOT_A, 1U, SLOT_CONFIRMED, 5U);
    make_slot_counter(BOOT_SLOT_B, 2U, SLOT_PENDING, 6U);
    boot_flash_set_active(BOOT_SLOT_A, true);
    boot_selection_scan(&selection);
    boot_selection_choose(&selection);
    CHECK(selection.active == BOOT_SLOT_B && selection.active_pending);
    CHECK(!selection.slot[BOOT_SLOT_A].below_floor);
    CHECK(boot_selection_security_floor(&selection) == 5U);

    /* Once B is confirmed, A falls below the floor and cannot come back. */
    CHECK(boot_meta_write_record(BOOT_SLOT_B, BOOT_META_CONFIRMED_OFFSET,
                                 BOOT_META_CONFIRMED_MAGIC, 0U));
    boot_selection_scan(&selection);
    boot_selection_choose(&selection);
    CHECK(boot_selection_security_floor(&selection) == 6U);
    CHECK(selection.slot[BOOT_SLOT_A].below_floor);
    CHECK(!selection.slot[BOOT_SLOT_A].valid);
    CHECK(selection.active == BOOT_SLOT_B);

    /* A rejected image does not raise the floor. */
    fake_flash_reset();
    fake_hal_reset();
    memset(&selection, 0, sizeof(selection));
    make_slot_counter(BOOT_SLOT_A, 1U, SLOT_CONFIRMED, 1U);
    make_slot_counter(BOOT_SLOT_B, 2U, SLOT_CONFIRMED, 9U);
    boot_flash_set_active(BOOT_SLOT_A, true);
    CHECK(boot_meta_write_record(BOOT_SLOT_B, BOOT_META_REJECTED_OFFSET,
                                 BOOT_META_REJECTED_MAGIC,
                                 BOOT_REJECT_TRIAL_LIMIT));
    boot_selection_scan(&selection);
    boot_selection_choose(&selection);
    CHECK(boot_selection_security_floor(&selection) == 1U);
    CHECK(selection.active == BOOT_SLOT_A);
}

int main(void)
{
    test_sha256_vectors();
    test_header_signature();
    test_update_authentication();
    test_update_over_used_slot();
    test_boot_authentication();
    test_security_floor();
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
