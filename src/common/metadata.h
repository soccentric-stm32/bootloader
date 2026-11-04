/*
 * metadata.h - append-only slot metadata (Appendix B)
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * Target:  STM32F767ZIT6, dual-bank flash
 * SRS:     IMG-006, SEL-001, TRY-003/007, UPD-002/006/007, Appendix B
 */

#ifndef METADATA_H
#define METADATA_H

#include <stdbool.h>
#include <stdint.h>
#include "boot_layout.h"
#include "image.h"

/* Copy the 256-byte header record.  Returns true when the header magic is
 * present; boot_image_header_valid() tells whether it is usable. */
bool boot_meta_header_read(boot_slot_t slot, boot_image_header_t *header);

/* True when the header magic is present (even if the rest is torn). */
bool boot_meta_header_present(boot_slot_t slot);

/* Read one Appendix B record.  *present is true only when the magic and the
 * record CRC are both correct; *value is the record's second word. */
bool boot_meta_read_record(boot_slot_t slot, uint32_t offset, uint32_t magic,
                           uint32_t *value, bool *present);

/* Program the header or a record into the inactive metadata sector.  The
 * area must still be erased (append-only, BSAFE-004). */
bool boot_meta_write_header(boot_slot_t slot,
                            const boot_image_header_t *header);
bool boot_meta_write_record(boot_slot_t slot, uint32_t offset,
                            uint32_t magic, uint32_t value);

#endif /* METADATA_H */
