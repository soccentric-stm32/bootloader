/**
 * @file    flash.h
 * @brief   Guarded flash wrapper; the only module that drives the flash
 *          controller.
 * @details Maps slots to physical banks and sectors (LAY-004, SWP-003) and
 *          refuses erase/program outside the update target slot, except the
 *          CONFIRMED, REJECTED and TRIAL records of the active slot
 *          (BSAFE-003).  All calls are synchronous and leave the controller
 *          locked.
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
 * @brief   Allow or forbid all flash erase/program operations.
 * @details BOOT holds this false until the dual-bank check passes; in
 *          single-bank mode it stays false (BSAFE-005).
 *
 * @param[in] allowed  true to allow erase/program; false makes them fail.
 *
 * @srs     BSAFE-005
 */
void boot_flash_set_allowed(bool allowed);

/**
 * @brief   Publish the active slot; this also fixes the update target.
 * @details The target is the other slot when @p valid, else slot A
 *          (UPD-001).
 *
 * @param[in] slot   Active slot.
 * @param[in] valid  true when @p slot holds a bootable image.
 *
 * @srs     SWP-003, UPD-001
 */
void boot_flash_set_active(boot_slot_t slot, bool valid);

/**
 * @brief   Return the slot an update may write.
 * @return  Inactive slot when the active one is valid, else BOOT_SLOT_A.
 *
 * @srs     UPD-001
 */
boot_slot_t boot_flash_target_slot(void);

/**
 * @brief   Erase the metadata sector (sector 2 of the bank, 16 KB).
 * @details Blocks for up to ~2 s; kicks the IWDG before and after.
 *
 * @param[in] slot  Slot to erase; must be the update target.
 * @return  false on a flash error, wrong slot, or writes not allowed.
 *
 * @srs     LAY-004, UPD-002/003
 */
bool boot_flash_erase_meta(boot_slot_t slot);

/**
 * @brief   Erase one sector of a slot.
 * @details Blocks for up to ~2 s; kicks the IWDG before and after.
 *
 * @param[in] slot   Slot to erase; must be the update target.
 * @param[in] index  Slot-local sector, 0..BOOT_SLOT_SECTOR_COUNT-1
 *                   (0 = 16 KB first sector, 8 = last 128 KB sector).
 * @return  false on a flash error, bad argument, or writes not allowed.
 *
 * @srs     LAY-004, UPD-003
 */
bool boot_flash_erase_slot_sector(boot_slot_t slot, uint32_t index);

/**
 * @brief   Program the header or one record into a metadata sector.
 * @details The update target accepts the header and every Appendix B
 *          record; the other slot only CONFIRMED, REJECTED and TRIAL
 *          (BSAFE-003, P2-6).  The destination must read erased, so no word
 *          is programmed twice.  Each word is read back.
 *
 * @param[in] slot    Slot whose metadata sector to write.
 * @param[in] offset  BOOT_META_*_OFFSET value.
 * @param[in] data    Bytes to program; 4-byte aligned.
 * @param[in] len     Bytes, multiple of 4; <= 256 for the header, <= 32
 *                    for a record.
 * @return  true when every word programmed and verified.
 *
 * @srs     BSAFE-003, UPD-002, Appendix B
 */
bool boot_flash_program_meta(boot_slot_t slot, uint32_t offset,
                             const void *data, uint32_t len);

/**
 * @brief   Program words into a slot's image area and verify each one.
 * @details Stops at the first word that fails to program or read back.
 *
 * @param[in]  slot      Slot to write; must be the update target.
 * @param[in]  offset    Slot-relative byte offset, multiple of 4.
 * @param[in]  data      Words to program; 4-byte aligned.
 * @param[in]  len       Bytes, multiple of 4, within BOOT_SLOT_SIZE.
 * @param[out] fail_addr Absolute address of the failing word, else 0;
 *                       may be NULL.
 * @return  true when every word programmed and verified.
 *
 * @srs     UPD-003/005
 */
bool boot_flash_program_slot(boot_slot_t slot, uint32_t offset,
                             const void *data, uint32_t len,
                             uint32_t *fail_addr);

/**
 * @brief   Test whether every byte of a range reads 0xFF.
 * @details Tells a clean "no record" from a torn one, which cannot be
 *          rewritten.  No bounds check: the caller passes a valid flash
 *          range.
 *
 * @param[in] address  Absolute start address.
 * @param[in] len      Length in bytes; 0 returns true.
 * @return  true when the whole range is erased.
 *
 * @srs     LAY-004, BSAFE-003 (torn-record detection)
 */
bool boot_flash_range_erased(uint32_t address, uint32_t len);

#endif /* FLASH_H */
