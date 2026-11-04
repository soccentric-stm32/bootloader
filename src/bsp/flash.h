/*
 * flash.h - guarded flash wrapper
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * Target:  STM32F767ZIT6, dual-bank flash
 * SRS:     LAY-004, SWP-003, BSAFE-001..004, UPD-002/003/005, Appendix B
 *
 * This is the only module that talks to the flash controller.  It maps a
 * slot letter to a physical bank and sector in one place, and rejects every
 * address that BOOT must never touch (its own sectors, the reserved bank 2
 * sectors, option bytes) as well as any write to the active slot.
 */

#ifndef FLASH_H
#define FLASH_H

#include <stdbool.h>
#include <stdint.h>
#include "boot_layout.h"

/* BSAFE-005: no erase or program is allowed in single-bank mode. */
void boot_flash_set_allowed(bool allowed);

/* Set the selected slot after the metadata scan.  When no slot is valid,
 * the update target is slot A (UPD-001). */
void boot_flash_set_active(boot_slot_t slot, bool valid);

/* The slot an update may write. */
boot_slot_t boot_flash_target_slot(void);

/* Erase the metadata sector of the inactive bank. */
bool boot_flash_erase_meta(boot_slot_t slot);

/* Erase one sector of the inactive slot (index 0..BOOT_SLOT_SECTOR_COUNT-1). */
bool boot_flash_erase_slot_sector(boot_slot_t slot, uint32_t index);

/* Program the 256-byte header or one 32-byte record into a metadata sector.
 * The active slot is only writable at the CONFIRMED/REJECTED offsets
 * (BSAFE-003). */
bool boot_flash_program_meta(boot_slot_t slot, uint32_t offset,
                             const void *data, uint32_t len);

/* Program 32-bit words into the inactive slot and verify each one. */
bool boot_flash_program_slot(boot_slot_t slot, uint32_t offset,
                             const void *data, uint32_t len,
                             uint32_t *fail_addr);

/* True when every byte in [address, address+len) reads 0xFF. */
bool boot_flash_range_erased(uint32_t address, uint32_t len);

#endif /* FLASH_H */
