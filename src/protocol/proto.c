/*
 * proto.c - HWLINK bootloader message service (section 9)
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * Target:  STM32F767ZIT6
 * SRS:     BGEN-002, ENT-001/005, UPD-001..UPD-010, TRY-008, section 9
 *
 * One static flatcc builder and static frame buffers; no dynamic memory.
 * Repeated request sequence numbers resend the previous response without
 * executing the request again ([LINK] LNK-008).
 */

#include <string.h>
#include "stm32f7xx_hal.h"
#include "boot_board.h"
#include "boot_layout.h"
#include "flatcc/flatcc_builder.h"
#include "hwlink.h"
#include "hwlink_builder.h"
#include "hwlink_verifier.h"
#include "flash.h"
#include "iwdg.h"
#include "image.h"
#include "metadata.h"
#include "proto.h"
#include "selection.h"
#include "uart3.h"
#include "update.h"

#ifndef BOOT_VERSION_STR
#define BOOT_VERSION_STR "0.1.0"
#endif
#ifndef BOOT_GIT_COMMIT
#define BOOT_GIT_COMMIT "unknown"
#endif
#ifndef BOOT_BUILD_DATE
#define BOOT_BUILD_DATE "unknown"
#endif



static flatcc_builder_t builder;

static uint8_t rx_enc[HWLINK_MAX_ENCODED] __attribute__((aligned(8)));
static size_t rx_enc_len;
static bool rx_enc_overflow;

static uint8_t rx_raw[HWLINK_MAX_FRAME] __attribute__((aligned(8)));
static uint8_t rx_payload[HWLINK_MAX_FRAME] __attribute__((aligned(8)));
static uint8_t build_payload[HWLINK_MAX_FRAME] __attribute__((aligned(8)));
static uint8_t raw_tx[HWLINK_MAX_FRAME] __attribute__((aligned(8)));
static uint8_t tx_frame[HWLINK_MAX_ENCODED] __attribute__((aligned(8)));

static uint8_t last_response[HWLINK_MAX_FRAME] __attribute__((aligned(8)));
static size_t last_response_len;
static uint16_t last_request_seq;
static bool last_request_valid;

static boot_selection_t *g_selection;
static boot_update_t *g_update;
static bool hello_seen;
static uint32_t last_activity_ms;

/* P2-7: a GetBootInfo scan is expensive (two 976 KB CRC runs).  Inside an
 * update session the metadata only changes at UpdateBegin/UpdateEnd/
 * UpdateAbort, so cache the scanned slot details and invalidate at those
 * points. */
static boot_selection_t boot_info_cache;
static bool boot_info_cache_valid;

static void boot_info_cache_invalidate(void)
{
    boot_info_cache_valid = false;
}

static uint32_t tx_timeout_ms(uint16_t len);

static bool send_payload(const uint8_t *payload, size_t payload_len,
                         uint8_t flags, uint16_t seq, bool store)
{
    size_t raw_len = 0U;
    size_t encoded_len = 0U;

    if (!hwlink_build_raw(payload, payload_len, flags, seq, raw_tx,
                          sizeof(raw_tx), &raw_len)) {
        return false;
    }
    if (store && raw_len <= sizeof(last_response)) {
        memcpy(last_response, raw_tx, raw_len);
        last_response_len = raw_len;
    }
    if (!hwlink_cobs_encode(raw_tx, raw_len, tx_frame, sizeof(tx_frame),
                            &encoded_len)) {
        return false;
    }
    return boot_uart3_send(tx_frame, (uint16_t)encoded_len,
                           tx_timeout_ms((uint16_t)encoded_len));
}

static bool send_body(hwlink_Body_union_ref_t body, uint16_t seq,
                      uint8_t flags, bool store)
{
    if (hwlink_Message_create_as_root(&builder, body) == 0U) {
        flatcc_builder_reset(&builder);
        return false;
    }

    size_t size = 0U;
    void *direct = flatcc_builder_get_direct_buffer(&builder, &size);
    if (direct == NULL || size == 0U || size > sizeof(build_payload)) {
        flatcc_builder_reset(&builder);
        return false;
    }
    memcpy(build_payload, direct, size);
    flatcc_builder_reset(&builder);

    return send_payload(build_payload, size, flags, seq, store);
}

static bool send_ack_offset(uint16_t seq, uint32_t next_offset)
{
    flatcc_builder_reset(&builder);
    hwlink_Ack_ref_t ack = hwlink_Ack_create(&builder, seq, next_offset);
    return ack != 0 &&
           send_body(hwlink_Body_as_Ack(ack), seq, HWLINK_FLAG_RESPONSE, true);
}

static bool send_ack(uint16_t seq)
{
    return send_ack_offset(seq, 0U);
}

static bool send_nak(uint16_t seq, hwlink_Error_enum_t error,
                     const char *detail, uint32_t expected_offset)
{
    flatcc_builder_reset(&builder);
    flatbuffers_string_ref_t text =
        flatbuffers_string_create_str(&builder, detail != NULL ? detail : "");
    hwlink_Nak_ref_t nak =
        hwlink_Nak_create(&builder, seq, error, text, expected_offset);
    return nak != 0 &&
           send_body(hwlink_Body_as_Nak(nak), seq, HWLINK_FLAG_RESPONSE, true);
}

static bool send_pong(uint16_t seq)
{
    flatcc_builder_reset(&builder);
    hwlink_Pong_ref_t pong =
        hwlink_Pong_create(&builder, (uint64_t)HAL_GetTick());
    return pong != 0 &&
           send_body(hwlink_Body_as_Pong(pong), seq, HWLINK_FLAG_RESPONSE,
                     true);
}

/* A full frame at 115200 takes about 180 ms; derive the timeout from the
 * encoded length and the current baud rate instead of a fixed 200 ms. */
static uint32_t tx_timeout_ms(uint16_t len)
{
    uint32_t baud = boot_uart3_baud();
    if (baud == 0U) {
        baud = 115200U;
    }
    uint32_t ms = (uint32_t)(((uint64_t)len * 10U * 1000U) / baud);
    return ms + 50U;
}

static hwlink_FlashMode_enum_t flash_mode(void)
{
    return (FLASH->OPTCR & FLASH_OPTCR_nDBANK) != 0U
               ? hwlink_FlashMode_SINGLE_BANK
               : hwlink_FlashMode_DUAL_BANK;
}

static bool send_hello_reply(uint16_t seq)
{
    flatcc_builder_reset(&builder);

    const char *detail = "A/B bootloader";
    if (flash_mode() != hwlink_FlashMode_DUAL_BANK) {
        detail = "WRONG_FLASH_MODE";
    }

    flatbuffers_string_ref_t name =
        flatbuffers_string_create_str(&builder, "stm32-bootloader");
    flatbuffers_string_ref_t version =
        flatbuffers_string_create_str(&builder, BOOT_VERSION_STR);
    flatbuffers_string_ref_t commit =
        flatbuffers_string_create_str(&builder, BOOT_GIT_COMMIT);
    flatbuffers_string_ref_t build =
        flatbuffers_string_create_str(&builder, BOOT_BUILD_DATE);
    flatbuffers_string_ref_t text =
        flatbuffers_string_create_str(&builder, detail);

    uint64_t uid = ((uint64_t)HAL_GetUIDw0() << 32U) |
                   (uint64_t)HAL_GetUIDw1();

    hwlink_HelloReply_ref_t reply = hwlink_HelloReply_create(
        &builder, hwlink_Role_BOOTLOADER, name, version, commit, build, uid,
        HAL_GetDEVID(), HAL_GetREVID(), flash_mode(),
        (uint16_t)HWLINK_PROTOCOL_MAJOR, (uint16_t)HWLINK_PROTOCOL_MINOR,
        (uint16_t)HWLINK_MAX_FRAME, text);

    return reply != 0 &&
           send_body(hwlink_Body_as_HelloReply(reply), seq,
                     HWLINK_FLAG_RESPONSE, true);
}

static bool send_update_result(uint16_t seq, bool success, uint32_t crc,
                               hwlink_Error_enum_t error, const char *detail)
{
    flatcc_builder_reset(&builder);
    flatbuffers_string_ref_t text =
        flatbuffers_string_create_str(&builder, detail != NULL ? detail : "");
    hwlink_UpdateResult_ref_t result =
        hwlink_UpdateResult_create(&builder, success ? 1U : 0U, crc, error,
                                   text);
    return result != 0 &&
           send_body(hwlink_Body_as_UpdateResult(result), seq,
                     HWLINK_FLAG_RESPONSE, true);
}

static bool send_progress(const char *operation, uint32_t done,
                          uint32_t total)
{
    flatcc_builder_reset(&builder);
    flatbuffers_string_ref_t op =
        flatbuffers_string_create_str(&builder,
                                      operation != NULL ? operation
                                                        : "update");
    hwlink_Progress_ref_t progress =
        hwlink_Progress_create(&builder, op, done, total);
    return progress != 0 &&
           send_body(hwlink_Body_as_Progress(progress), 0U,
                     HWLINK_FLAG_EVENT, false);
}

static void proto_progress(const char *operation, uint32_t done,
                           uint32_t total)
{
    (void)send_progress(operation, done, total);
}

static void commit_to_hex(const uint8_t commit[20], char out[41])
{
    static const char hex[] = "0123456789abcdef";
    for (uint32_t i = 0U; i < 20U; i++) {
        out[i * 2U]      = hex[(commit[i] >> 4U) & 0xFU];
        out[(i * 2U) + 1U] = hex[commit[i] & 0xFU];
    }
    out[40] = '\0';
}

static const char *slot_detail(const boot_slot_info_t *info)
{
    if (!info->header_present) {
        return "no header";
    }
    if (!info->header_valid) {
        return "bad header";
    }
    if (info->rejected) {
        return info->rejected_reason == BOOT_REJECT_TRIAL_LIMIT
                   ? "rejected: trial limit"
                   : "rejected";
    }
    if (!info->activated) {
        return info->verified ? "verified, not activated" : "not activated";
    }
    if (!info->sequence_valid) {
        return "erased sequence number";
    }
    if (!info->vectors_ok) {
        return "bad stack pointer or reset vector";
    }
    if (!info->crc_checked) {
        return "image CRC not checked";
    }
    if (!info->crc_ok) {
        return "image CRC32 mismatch";
    }
    if (info->confirmed) {
        return "confirmed";
    }
    return "pending";
}

static bool send_boot_info(uint16_t seq)
{
    /* P0-2/P2-7: refresh slot details without changing the active/target
     * selection.  A REJECTED record that could only be applied in RAM is
     * re-applied by the scan, so this request cannot undo a rollback while
     * an update session is open.  During an update session, reuse the
     * previous scan until the metadata changes. */
    if (g_update != NULL && g_update->active && boot_info_cache_valid) {
        for (uint32_t i = 0U; i < 2U; i++) {
            g_selection->slot[i] = boot_info_cache.slot[i];
        }
    } else {
        boot_selection_refresh_slots(g_selection);
        if (g_update != NULL && g_update->active) {
            for (uint32_t i = 0U; i < 2U; i++) {
                boot_info_cache.slot[i] = g_selection->slot[i];
            }
            boot_info_cache_valid = true;
        }
    }

    flatcc_builder_reset(&builder);

    hwlink_BootSlotInfo_ref_t refs[2];

    for (uint32_t i = 0U; i < 2U; i++) {
        const boot_slot_info_t *info = &g_selection->slot[i];
        bool pending = info->valid && info->activated && !info->confirmed &&
                       !info->rejected;

        char name[33];
        memset(name, 0, sizeof(name));
        char commit[41];
        memset(commit, 0, sizeof(commit));
        if (info->header_valid) {
            memcpy(name, info->header.name, 32);
            name[32] = '\0';
            commit_to_hex(info->header.git_commit, commit);
        }

        uint8_t trial = 0U;
        if (pending) {
            trial = info->trial_count;
            uint32_t raw = RTC->BKP0R;
            if ((raw & BOOT_TRIAL_MAGIC_MASK) == BOOT_TRIAL_MAGIC &&
                ((raw >> BOOT_TRIAL_SLOT_SHIFT) & BOOT_TRIAL_SLOT_MASK) ==
                    i) {
                uint8_t rtc_trial =
                    (uint8_t)((raw >> BOOT_TRIAL_COUNT_SHIFT) &
                              BOOT_TRIAL_COUNT_MASK);
                if (rtc_trial > trial) {
                    trial = rtc_trial;
                }
            }
        }

        flatbuffers_string_ref_t commit_ref =
            flatbuffers_string_create_str(&builder, commit);
        flatbuffers_string_ref_t name_ref =
            flatbuffers_string_create_str(&builder, name);
        flatbuffers_string_ref_t detail_ref =
            flatbuffers_string_create_str(&builder, slot_detail(info));

        refs[i] = hwlink_BootSlotInfo_create(
            &builder, (hwlink_Slot_enum_t)(i + 1U),
            info->header_present ? 1U : 0U, info->valid ? 1U : 0U,
            pending ? 1U : 0U, info->confirmed ? 1U : 0U,
            info->rejected ? 1U : 0U,
            info->sequence_valid ? info->sequence : 0U, trial,
            info->header_valid ? info->header.version : 0U,
            info->header_valid ? info->header.build_timestamp : 0U,
            commit_ref, name_ref,
            info->header_valid ? info->header.image_size : 0U, detail_ref);
        if (refs[i] == 0) {
            flatcc_builder_reset(&builder);
            return false;
        }
    }

    boot_slot_t rejected_slot = BOOT_SLOT_A;
    uint8_t outcome = boot_outcome_get(&rejected_slot);
    if (outcome == BOOT_OUTCOME_UNKNOWN) {
        /* The rejected record survives a power loss even when the backup
         * register does not. */
        bool found = false;
        for (uint32_t i = 0U; i < 2U; i++) {
            if (g_selection->slot[i].rejected) {
                outcome = BOOT_OUTCOME_ROLLED_BACK;
                rejected_slot = (boot_slot_t)i;
                found = true;
                break;
            }
        }
        if (!found) {
            outcome = g_selection->active_valid ? BOOT_OUTCOME_BOOTED
                                                : BOOT_OUTCOME_UPDATE_MODE;
        }
    }

    hwlink_Slot_enum_t active_slot =
        g_selection->active_valid
            ? (hwlink_Slot_enum_t)((uint32_t)g_selection->active + 1U)
            : hwlink_Slot_UNKNOWN;
    hwlink_Slot_enum_t rejected =
        outcome == BOOT_OUTCOME_ROLLED_BACK
            ? (hwlink_Slot_enum_t)((uint32_t)rejected_slot + 1U)
            : hwlink_Slot_UNKNOWN;

    /* P0-3: report the slot BOOT will actually write next instead of
     * letting hosts guess "the slot that is not active". */
    hwlink_Slot_enum_t target_slot =
        (hwlink_Slot_enum_t)((uint32_t)boot_flash_target_slot() + 1U);
    flatbuffers_string_ref_t boot_version =
        flatbuffers_string_create_str(&builder, BOOT_VERSION_STR);
    flatbuffers_string_ref_t boot_build =
        flatbuffers_string_create_str(&builder, BOOT_BUILD_DATE);

    if (hwlink_BootInfo_start(&builder) != 0 ||
        hwlink_BootInfo_slots_start(&builder) != 0) {
        flatcc_builder_reset(&builder);
        return false;
    }
    for (uint32_t i = 0U; i < 2U; i++) {
        if (hwlink_BootInfo_slots_push(&builder, refs[i]) == NULL) {
            flatcc_builder_reset(&builder);
            return false;
        }
    }
    if (hwlink_BootInfo_slots_end(&builder) != 0 ||
        hwlink_BootInfo_active_slot_add(&builder, active_slot) != 0 ||
        hwlink_BootInfo_last_outcome_add(
            &builder, (hwlink_BootOutcome_enum_t)outcome) != 0 ||
        hwlink_BootInfo_rejected_slot_add(&builder, rejected) != 0 ||
        hwlink_BootInfo_flash_mode_add(&builder, flash_mode()) != 0 ||
        hwlink_BootInfo_active_valid_add(
            &builder, g_selection->active_valid ? 1U : 0U) != 0 ||
        hwlink_BootInfo_target_slot_add(&builder, target_slot) != 0 ||
        hwlink_BootInfo_boot_version_add(&builder, boot_version) != 0 ||
        hwlink_BootInfo_boot_build_add(&builder, boot_build) != 0) {
        flatcc_builder_reset(&builder);
        return false;
    }

    hwlink_BootInfo_ref_t info = hwlink_BootInfo_end(&builder);
    if (info == 0) {
        flatcc_builder_reset(&builder);
        return false;
    }
    return send_body(hwlink_Body_as_BootInfo(info), seq,
                     HWLINK_FLAG_RESPONSE, true);
}

static void handle_update_begin(uint16_t seq,
                                hwlink_UpdateBegin_table_t begin)
{
    if (begin == 0) {
        (void)send_nak(seq, hwlink_Error_MALFORMED, "missing header", 0U);
        return;
    }
    hwlink_ImageHeader_table_t image = hwlink_UpdateBegin_header(begin);
    if (image == 0) {
        (void)send_nak(seq, hwlink_Error_MALFORMED, "missing header", 0U);
        return;
    }

    boot_image_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.magic           = hwlink_ImageHeader_magic(image);
    desc.header_version  = hwlink_ImageHeader_header_version(image);
    desc.header_size     = hwlink_ImageHeader_header_size(image);
    desc.image_type      = hwlink_ImageHeader_image_type(image);
    desc.version         = hwlink_ImageHeader_version(image);
    desc.image_size      = hwlink_ImageHeader_image_size(image);
    desc.image_crc32     = hwlink_ImageHeader_image_crc32(image);
    desc.load_address    = hwlink_ImageHeader_load_address(image);
    desc.entry_address   = hwlink_ImageHeader_entry_address(image);
    desc.build_timestamp = hwlink_ImageHeader_build_timestamp(image);
    desc.header_crc32   = hwlink_ImageHeader_header_crc32(image);
    desc.header_crc32_present =
        hwlink_ImageHeader_header_crc32_is_present(image) != 0;

    flatbuffers_string_t name = hwlink_ImageHeader_name(image);
    size_t name_len = flatbuffers_string_len(name);
    if (name_len > sizeof(desc.name)) {
        name_len = sizeof(desc.name);
    }
    if (name != NULL) {
        memcpy(desc.name, name, name_len);
    }
    desc.name_len = (uint32_t)name_len;

    flatbuffers_uint8_vec_t commit = hwlink_ImageHeader_git_commit(image);
    size_t commit_len = flatbuffers_uint8_vec_len(commit);
    if (commit_len > sizeof(desc.git_commit)) {
        commit_len = sizeof(desc.git_commit);
    }
    for (size_t i = 0U; i < commit_len; i++) {
        desc.git_commit[i] = flatbuffers_uint8_vec_at(commit, i);
    }
    desc.git_commit_len = (uint32_t)commit_len;

    bool force = hwlink_UpdateBegin_force(begin) != 0;
    char detail[64];
    boot_info_cache_invalidate();
    boot_error_t error = boot_update_begin(g_update, g_selection, &desc,
                                           force, detail, sizeof(detail));
    if (error == BOOT_ERR_NONE) {
        (void)send_ack_offset(seq, 0U);
    } else {
        (void)send_nak(seq, (hwlink_Error_enum_t)error, detail, 0U);
    }
}

static void handle_update_data(uint16_t seq, hwlink_UpdateData_table_t data)
{
    if (data == 0) {
        (void)send_nak(seq, hwlink_Error_MALFORMED, "missing data", 0U);
        return;
    }

    uint32_t offset = hwlink_UpdateData_offset(data);
    flatbuffers_uint8_vec_t bytes = hwlink_UpdateData_data(data);
    size_t len = flatbuffers_uint8_vec_len(bytes);

    char detail[64];
    uint32_t next = g_update->expected_offset;
    boot_error_t error =
        boot_update_data(g_update, offset, bytes, (uint32_t)len, &next,
                         detail, sizeof(detail));

    if (error == BOOT_ERR_NONE) {
        (void)send_ack_offset(seq, next);
    } else if (error == BOOT_ERR_OUT_OF_ORDER) {
        (void)send_nak(seq, hwlink_Error_OUT_OF_ORDER, detail, next);
    } else {
        (void)send_nak(seq, (hwlink_Error_enum_t)error, detail, next);
    }
}

static void handle_update_end(uint16_t seq)
{
    char detail[64];
    uint32_t crc = 0U;
    boot_info_cache_invalidate();
    boot_error_t error =
        boot_update_end(g_update, &crc, detail, sizeof(detail));

    if (error == BOOT_ERR_NONE) {
        (void)send_update_result(seq, true, crc, hwlink_Error_NONE, "");
    } else {
        (void)send_update_result(seq, false, crc,
                                 (hwlink_Error_enum_t)error, detail);
    }
}

static void handle_activate(uint16_t seq)
{
    char detail[64];
    boot_info_cache_invalidate();
    boot_error_t error =
        boot_update_activate(g_update, g_selection, detail, sizeof(detail));
    if (error == BOOT_ERR_NONE) {
        /* P2-5: give the Ack time to leave the UART before the reset.  If
         * the host still loses the reply it treats the timeout as
         * success-pending and confirms with GetBootInfo. */
        if (send_ack_offset(seq, 0U)) {
            HAL_Delay(20U);
            NVIC_SystemReset();
        }
    } else {
        (void)send_nak(seq, (hwlink_Error_enum_t)error, detail, 0U);
    }
}

static void handle_set_baud(uint16_t seq, hwlink_SetBaud_table_t set)
{
    uint32_t rate = hwlink_SetBaud_rate(set);
    if (rate != BOOT_UART_DEFAULT_BAUD && rate != BOOT_UART_FAST_BAUD) {
        (void)send_nak(seq, hwlink_Error_BAD_ARGUMENT, "baud rate", 0U);
        return;
    }
    if (!send_ack(seq)) {
        return;
    }

    rx_enc_len = 0U;
    rx_enc_overflow = false;
    last_request_valid = false;
    last_response_len = 0U;
    (void)boot_uart3_set_baud(rate);
    last_activity_ms = HAL_GetTick();
}

static void handle_payload(uint16_t seq, const uint8_t *payload, size_t len)
{
    if (hwlink_Message_verify_as_root(payload, len) != flatcc_verify_ok) {
        (void)send_nak(seq, hwlink_Error_MALFORMED, "flatbuffer verifier", 0U);
        return;
    }

    hwlink_Message_table_t message = hwlink_Message_as_root(payload);
    hwlink_Body_union_t body = hwlink_Message_body_union(message);

    switch (body.type) {
    case hwlink_Body_Hello:
        hello_seen = true;
        (void)send_hello_reply(seq);
        break;

    case hwlink_Body_Ping:
        (void)send_pong(seq);
        break;

    case hwlink_Body_GetBootInfo:
        if (!send_boot_info(seq)) {
            (void)send_nak(seq, hwlink_Error_BUSY, "boot info build failed",
                           0U);
        }
        break;

    case hwlink_Body_SetBaud:
        handle_set_baud(seq, (hwlink_SetBaud_table_t)body.value);
        break;

    case hwlink_Body_UpdateBegin:
        handle_update_begin(seq, (hwlink_UpdateBegin_table_t)body.value);
        break;

    case hwlink_Body_UpdateData:
        handle_update_data(seq, (hwlink_UpdateData_table_t)body.value);
        break;

    case hwlink_Body_UpdateEnd:
        handle_update_end(seq);
        break;

    case hwlink_Body_Activate:
        handle_activate(seq);
        break;

    case hwlink_Body_UpdateAbort:
        boot_info_cache_invalidate();
        boot_update_abort(g_update);
        (void)send_ack_offset(seq, 0U);
        break;

    case hwlink_Body_Reset:
        if (send_ack_offset(seq, 0U)) {
            NVIC_SystemReset();
        }
        break;

    case hwlink_Body_HelloReply:
    case hwlink_Body_Pong:
    case hwlink_Body_Ack:
    case hwlink_Body_Nak:
    case hwlink_Body_Log:
    case hwlink_Body_Progress:
    case hwlink_Body_BootInfo:
    case hwlink_Body_UpdateResult:
    case hwlink_Body_SetLed:
    case hwlink_Body_SetLedAll:
    case hwlink_Body_ClearFault:
    case hwlink_Body_EnterBootloader:
    case hwlink_Body_GetStatus:
    case hwlink_Body_Status:
    case hwlink_Body_ButtonEvent:
    default:
        (void)send_nak(seq, hwlink_Error_UNSUPPORTED,
                       "message not supported", 0U);
        break;
    }
}

static void reset_parser(void)
{
    rx_enc_len = 0U;
    rx_enc_overflow = false;
}

static void handle_raw(const uint8_t *raw, size_t raw_len)
{
    size_t payload_len = 0U;
    uint8_t flags = 0U;
    uint16_t seq = 0U;

    if (!hwlink_decode_raw(raw, raw_len, rx_payload, sizeof(rx_payload),
                           &payload_len, &flags, &seq)) {
        return;   /* LNK-004: bad CRC is dropped silently */
    }

    last_activity_ms = HAL_GetTick();

    if ((flags & (HWLINK_FLAG_RESPONSE | HWLINK_FLAG_EVENT)) != 0U) {
        return;
    }

    /* A host session starts with Hello.  Never answer it from the replay
     * cache: a new session restarts at sequence 1, which may equal the last
     * sequence of the previous session (LNK-008). */
    if (hwlink_Message_verify_as_root(rx_payload, payload_len) ==
        flatcc_verify_ok) {
        hwlink_Message_table_t message = hwlink_Message_as_root(rx_payload);
        if (hwlink_Message_body_union(message).type == hwlink_Body_Hello) {
            last_request_valid = false;
            last_response_len = 0U;
        }
    }

    if (last_request_valid && seq == last_request_seq &&
        last_response_len > 0U) {
        size_t encoded_len = 0U;
        if (hwlink_cobs_encode(last_response, last_response_len, tx_frame,
                               sizeof(tx_frame), &encoded_len)) {
            (void)boot_uart3_send(tx_frame, (uint16_t)encoded_len,
                                  tx_timeout_ms((uint16_t)encoded_len));
        }
        return;
    }

    last_request_valid = seq != 0U;
    last_request_seq   = seq;
    last_response_len  = 0U;

    handle_payload(seq, rx_payload, payload_len);
}

static void feed_byte(uint8_t byte)
{
    if (byte == 0U) {
        if (!rx_enc_overflow && rx_enc_len > 0U) {
            size_t raw_len = 0U;
            if (hwlink_cobs_decode(rx_enc, rx_enc_len, rx_raw,
                                   sizeof(rx_raw), &raw_len) == 0) {
                handle_raw(rx_raw, raw_len);
            }
        }
        reset_parser();
    } else if (rx_enc_len < sizeof(rx_enc)) {
        rx_enc[rx_enc_len++] = byte;
    } else {
        rx_enc_overflow = true;
    }
}

void boot_proto_poll(void)
{
    uint8_t chunk[64];

    for (uint32_t batch = 0U; batch < 8U; batch++) {
        uint32_t got = boot_uart3_read(chunk, sizeof(chunk));
        if (got == 0U) {
            break;
        }
        for (uint32_t i = 0U; i < got; i++) {
            feed_byte(chunk[i]);
        }
    }
}

void boot_proto_init(boot_selection_t *selection, boot_update_t *update)
{
    g_selection = selection;
    g_update    = update;
    hello_seen  = false;
    boot_info_cache_valid = false;
    last_activity_ms = HAL_GetTick();

    reset_parser();
    last_response_len = 0U;
    last_request_valid = false;

    (void)flatcc_builder_init(&builder);
    boot_update_set_progress_cb(proto_progress);
}

bool boot_proto_listen_for_hello(uint32_t window_ms)
{
    uint32_t start = HAL_GetTick();

    hello_seen = false;
    while ((HAL_GetTick() - start) < window_ms) {
        boot_iwdg_kick();
        boot_proto_poll();
        if (hello_seen || (g_update != NULL && g_update->active)) {
            /* A Hello entered update mode, or an UpdateBegin did so without
             * a preceding Hello.  Either way, do not boot over an active
             * update session. */
            return true;
        }
        HAL_Delay(1U);
    }
    return hello_seen;
}

void boot_proto_run(bool can_boot)
{
    uint32_t last_toggle = HAL_GetTick();
    bool led_on = true;

    for (;;) {
        /* P3-9: the software watchdog is only started before the hand-over.
         * BOOT may still be running after a reset with the IWDG active, so
         * reloading it here is harmless; see boot_proto_run() for the long
         * update session. */
        boot_iwdg_kick();
        boot_proto_poll();

        uint32_t now = HAL_GetTick();
        bool updating = g_update != NULL && g_update->active;

        if (updating) {
            if ((now - last_toggle) >= 100U) {
                led_on = !led_on;
                HAL_GPIO_WritePin(BOOT_LED2_PORT, BOOT_LED2_PIN,
                                  led_on ? GPIO_PIN_SET : GPIO_PIN_RESET);
                last_toggle = now;
            }
        } else {
            HAL_GPIO_WritePin(BOOT_LED2_PORT, BOOT_LED2_PIN, GPIO_PIN_SET);
        }

        if (can_boot &&
            (now - last_activity_ms) >= BOOT_IDLE_TIMEOUT_MS) {
            return;
        }
        HAL_Delay(1U);
    }
}
