/*
 * update.h - image update state machine (section 9)
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * Target:  STM32F767ZIT6
 * SRS:     UPD-001..UPD-010, IMG-001..IMG-006
 */

#ifndef UPDATE_H
#define UPDATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "boot_layout.h"
#include "image.h"
#include "selection.h"

/* Error values match hwlink.Error in protocol/hwlink.fbs. */
typedef enum {
    BOOT_ERR_NONE           = 0,
    BOOT_ERR_MALFORMED      = 1,
    BOOT_ERR_UNSUPPORTED    = 2,
    BOOT_ERR_BAD_ARGUMENT   = 3,
    BOOT_ERR_BAD_STATE      = 4,
    BOOT_ERR_BUSY           = 5,
    BOOT_ERR_NOT_FOUND      = 6,
    BOOT_ERR_BAD_HEADER     = 7,
    BOOT_ERR_BAD_LAYOUT     = 8,
    BOOT_ERR_BAD_VECTORS    = 9,
    BOOT_ERR_OUT_OF_ORDER   = 10,
    BOOT_ERR_FLASH_ERROR    = 11,
    BOOT_ERR_CRC_MISMATCH   = 12,
    BOOT_ERR_WRONG_FLASH_MODE = 13
} boot_error_t;

typedef struct {
    uint32_t magic;
    uint16_t header_version;
    uint16_t header_size;
    uint32_t image_type;
    uint32_t version;
    uint32_t image_size;
    uint32_t image_crc32;
    uint32_t load_address;
    uint32_t entry_address;
    uint32_t build_timestamp;
    uint8_t  git_commit[20];
    uint32_t git_commit_len;
    char     name[32];
    uint32_t name_len;
    uint32_t header_crc32;
    bool     header_crc32_present;
} boot_image_desc_t;

typedef struct {
    bool active;
    bool verified;
    boot_slot_t slot;
    uint32_t image_size;
    uint32_t image_crc32;
    uint32_t expected_offset;
    uint32_t last_offset;
    bool last_offset_valid;
    uint32_t erased_mask;
    boot_image_header_t header;
    uint8_t word_stage[4];
    uint32_t word_stage_len;
    uint32_t word_stage_offset;
} boot_update_t;

typedef void (*boot_progress_fn)(const char *operation, uint32_t done,
                                 uint32_t total);

void boot_update_reset(boot_update_t *update);
void boot_update_set_progress_cb(boot_progress_fn callback);

/* UPD-002: validate the header, erase the inactive metadata sector, write
 * the header record.  P0-1: refuses to touch the rollback slot while the
 * active slot is still on trial; force is the bench-only override. */
boot_error_t boot_update_begin(boot_update_t *update,
                               const boot_selection_t *selection,
                               const boot_image_desc_t *desc, bool force,
                               char *detail, size_t detail_cap);

/* UPD-003/004/005: program one chunk and return the next expected offset. */
boot_error_t boot_update_data(boot_update_t *update, uint32_t offset,
                              const uint8_t *data, uint32_t len,
                              uint32_t *next_offset,
                              char *detail, size_t detail_cap);

/* UPD-006: verify vectors and CRC read back from flash, write VERIFIED. */
boot_error_t boot_update_end(boot_update_t *update, uint32_t *image_crc,
                             char *detail, size_t detail_cap);

/* UPD-007: write ACTIVATED with the next sequence number. */
boot_error_t boot_update_activate(boot_update_t *update,
                                  const boot_selection_t *selection,
                                  char *detail, size_t detail_cap);

/* UPD-008: abandon the update; the inactive slot is left invalid. */
void boot_update_abort(boot_update_t *update);

#endif /* UPDATE_H */
