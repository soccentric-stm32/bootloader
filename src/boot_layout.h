/*
 * boot_layout.h - dual-bank flash layout shared by BOOT, the applications
 *                 and the host image tool
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * Target:  STM32F767ZIT6, dual-bank flash (nDBANK = 0)
 * SRS:     LAY-001..LAY-005 (section 3), HND-002, IMG-003
 *
 * Addresses are AXIM addresses without bank swap (SWP_FB = 0), which is
 * the state BOOT always runs in (SWP-003).
 */

#ifndef BOOT_LAYOUT_H
#define BOOT_LAYOUT_H

#define BOOT_BANK1_BASE         0x08000000UL
#define BOOT_BANK2_BASE         0x08100000UL
#define BOOT_BANK_SIZE          0x00100000UL   /* 1 MB                   */

/* Offsets inside each bank (section 3.1). */
#define BOOT_REGION_OFFSET      0x00000000UL   /* sectors 0-1, 32 KB     */
#define BOOT_REGION_SIZE        0x00008000UL
#define BOOT_META_OFFSET        0x00008000UL   /* sector 2, 16 KB        */
#define BOOT_META_SIZE          0x00004000UL
#define BOOT_SLOT_OFFSET        0x0000C000UL   /* sectors 3-11, 976 KB   */
#define BOOT_SLOT_SIZE          0x000F4000UL

/* LAY-002: every application is linked here and runs from either slot
 * because of the bank swap. */
#define BOOT_APP_LOAD_ADDR      (BOOT_BANK1_BASE + BOOT_SLOT_OFFSET)

#define BOOT_SLOT_A_BASE        (BOOT_BANK1_BASE + BOOT_SLOT_OFFSET)
#define BOOT_SLOT_B_BASE        (BOOT_BANK2_BASE + BOOT_SLOT_OFFSET)
#define BOOT_META_A_BASE        (BOOT_BANK1_BASE + BOOT_META_OFFSET)
#define BOOT_META_B_BASE        (BOOT_BANK2_BASE + BOOT_META_OFFSET)

/* IMG-003: an application's initial stack pointer must lie in internal
 * RAM (DTCM + SRAM1 + SRAM2 are contiguous); the top is inclusive. */
#define BOOT_RAM_START          0x20000000UL
#define BOOT_RAM_END            0x20080000UL

#endif /* BOOT_LAYOUT_H */
