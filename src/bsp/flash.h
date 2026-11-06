/**
 * @file    flash.h
 * @brief   Guarded flash wrapper (the only module that talks to the
 *          flash controller).
 *
 * @details
 * This module is deliberately the sole entry point to the on-chip flash
 * from the rest of BOOT.  It centralises:
 *
 * - **address translation**: a slot letter (A/B) → the physical bank and
 *   sector for that slot, so no other module spells out a physical
 *   address (LAY-004, SWP-003), and
 * - **safety rails**: every erase/program call is checked against
 *   address ranges that BOOT must never touch (its own sectors, the
 *   reserved bank-2 sectors, the option-byte area), and it refuses any
 *   write to the *active* slot except at the allowed
 *   `CONFIRMED` / `REJECTED` metadata offsets (BSAFE-003).
 *
 * All entry points are synchronous and either complete the operation or
 * return `false` (for the erase/program family) without leaving the
 * flash controller in a partially-configured state.  The host-facing
 * `boot_error_t` mapping happens in the update / selection modules.
 *
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  STM32F767ZIT6, dual-bank flash
 * @srs     LAY-004, SWP-003, BSAFE-001..004, UPD-002/003/005, Appendix B
 */

#ifndef FLASH_H
#define FLASH_H

#include <stdbool.h>
#include <stdint.h>
#include "boot_layout.h"

/**
 * @brief   Allow (or forbid) flash erase/program operations.
 *
 * @details BSAFE-005: in single-bank flash mode **no** erase or program
 *          is allowed - the A/B slot assumptions break and the safe
 *          response is to refuse everything.  Even in dual-bank mode the
 *          bootloader holds this at `false` until the dual-bank check
 *          has passed, so an early bug cannot corrupt an image.
 *
 * @param[in] allowed  `true` to allow erase/program, `false` to forbid
 *                     them (all subsequent erase/program calls fail
 *                     immediately with the "flash not allowed" detail).
 *
 * @srs     BSAFE-005
 */
void boot_flash_set_allowed(bool allowed);

/**
 * @brief   Publish the selected (active) slot and its validity.
 *
 * @details Called by the selection module after each scan-and-choose.
 *          Also fixes the *update target* slot: when the selection is
 *          `valid == false` (no bootable image) the target defaults to
 *          slot A per UPD-001 (a fresh install has no active slot to
 *          protect, so A is the natural destination).
 *
 * @param[in] slot     The active slot (A or B), as published by the
 *                     selection module.
 * @param[in] valid    Whether @ref slot actually has a valid image.
 *
 * @srs     SWP-003, UPD-001
 */
void boot_flash_set_active(boot_slot_t slot, bool valid);

/**
 * @brief   Query which slot an update may write into.
 *
 * @details Usually the *inactive* slot; when the active slot is not
 *          valid (no bootable image) it can be either slot - the update
 *          is installing an image into an empty / broken bank.
 *
 * @return  The target slot (A or B).
 *
 * @srs     UPD-001
 */
boot_slot_t boot_flash_target_slot(void);

/**
 * @brief   Erase the metadata sector of a slot.
 *
 * @details The metadata sector (sector 2 of the bank, 16 KB) is where
 *          the image header and its lifecycle records live.  Erasing it
 *          is the first step of an update and the *last* step of an
 *          abort (upstream it is what makes an aborted update inert).
 *
 * @param[in] slot  Slot whose metadata sector is to be erased.
 * @return  `true` on success, `false` on a flash error or if writes are
 *          not currently allowed.
 *
 * @srs     LAY-004, UPD-002/003
 */
bool boot_flash_erase_meta(boot_slot_t slot);

/**
 * @brief   Erase a single sector inside a slot.
 *
 * @details Called lazily by the update flow when the first chunk that
 *          falls into a slot sector arrives (UPD-003).  `index` is slot
 *          -local (0..`BOOT_SLOT_SECTOR_COUNT-1`); the physical sector
 *          is computed from the slot here (LAY-004).
 *
 * @param[in] slot   Slot containing the target sector.
 * @param[in] index  Slot-local sector index (0 = the 16 KB sector at
 *                   the slot start, 8 = the 128 KB tail sector).
 * @return  `true` on success, `false` on a flash error or if writes are
 *          not currently allowed.
 *
 * @srs     LAY-004, UPD-003
 */
bool boot_flash_erase_slot_sector(boot_slot_t slot, uint32_t index);

/**
 * @brief   Program a metadata record (header or one of the lifecycle
 *          records) into a slot's metadata sector.
 *
 * @details The active slot is writable only at the `CONFIRMED`,
 *          `REJECTED` and `TRIAL` record offsets - those are the records
 *          the selection flow updates without erasing the rest of the
 *          metadata, and the trial in particular must be updated on the
 *          sector of the slot actually booting (BSAFE-003, P2-6).  Every
 *          other offset (header, VERIFIED, ACTIVATED) is restricted to
 *          the update target slot.  The call also enforces append-only:
 *          the destination area must be fully erased (0xFF) before the
 *          write, so a word is never programmed twice.
 *
 * @param[in] slot    Target slot.
 * @param[in] offset  Offset within the metadata sector (see
 *                    `BOOT_META_*_OFFSET` in boot_layout.h).
 * @param[in] data    Bytes to program (256 for the header, 32 for a
 *                    single or run of records).
 * @param[in] len     Length in bytes of @ref data (multiple of 4, does
 *                    not exceed the record area).
 * @return  `true` on success (write + verify completed), `false` on a
 *          flash error, a BSAFE-003 refusal, disallowed offset, or
 *          non-erased (already-programmed) destination.
 *
 * @srs     BSAFE-003, UPD-002, Appendix B
 */
bool boot_flash_program_meta(boot_slot_t slot, uint32_t offset,
                             const void *data, uint32_t len);

/**
 * @brief   Program 32-bit words into the image area of a slot and verify
 *          each word back from flash after writing.
 *
 * @details The write is word-at-a-time (the hardware programs 32-bit
 *          words), and *every* word is read back and compared with what
 *          was written; the first mismatch aborts the operation and its
 *          address is returned via @ref fail_addr.  This read-back
 *          is the flash-layer's own safety rail (independent of the
 *          image-CRC the selection module runs afterwards) - it catches
 *          a programming fault (wear, power glitch during the program,
 *          etc.) immediately instead of letting it surface as a boot
 *          failure later.
 *
 * @param[in]  slot      Target slot (must be the inactive one or the
 *                       update target for the call to succeed).
 * @param[in]  offset    Slot-relative byte offset of the first word.
 * @param[in]  data      Words to program (little-endian on the wire).
 * @param[in]  len       Number of bytes in @ref data (multiple of 4).
 * @param[out] fail_addr On a programming/verify mismatch, the first
 *                       offending byte address; on success it is left
 *                       untouched.
 * @return  `true` if every word was programmed and verified, `false`
 *          otherwise (mismatch detected, flash error, or not allowed).
 *
 * @srs     UPD-003/005
 */
bool boot_flash_program_slot(boot_slot_t slot, uint32_t offset,
                             const void *data, uint32_t len,
                             uint32_t *fail_addr);

/**
 * @brief   Check whether an address range is fully in the erased state
 *          (all bytes read 0xFF).
 *
 * @details Used to distinguish a clean "no record" state from a torn /
 *          partial record - a torn record cannot be rewritten (append-
 *          only, see the trial-record design) and must be treated
 *          conservatively (e.g. the selection module treats a non-
 *          erased REJECTED area as a rejection).
 *
 * @param[in] address  Start address (within one of the two banks).
 * @param[in] len      Length of the range in bytes (at least 1).
 * @return  `true` if every byte reads 0xFF; `false` otherwise or if the
 *          range is not inside a known bank.
 *
 * @srs     LAY-004, BSAFE-003 (torn-record detection)
 */
bool boot_flash_range_erased(uint32_t address, uint32_t len);

#endif /* FLASH_H */
