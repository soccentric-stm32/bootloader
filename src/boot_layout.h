/*
 * boot_layout.h - dual-bank flash layout shared by BOOT, the applications
 *                 and the host image tool
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * Target:  STM32F767ZIT6, dual-bank flash (nDBANK = 0)
 * SRS:     LAY-001..LAY-005 (section 3), SWP-003, HND-002, IMG-003,
 *          Appendix B
 *
 * Addresses are AXIM addresses without bank swap (SWP_FB = 0), which is
 * the state BOOT always runs in (SWP-003).  All slot/bank/sector mapping
 * lives in this one header so that the flash wrapper never spells out a
 * physical address itself.
 */

#ifndef BOOT_LAYOUT_H
#define BOOT_LAYOUT_H

#include <stdint.h>

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

/* A-9: reserved no-init window for application fault records.  BOOT uses
 * DTCM 0x20000000..0x2000EFFF (its 60 KB budget) and SRAM1
 * 0x20020000..0x20020FFF (the USART3 DMA ring), so a record placed after
 * the application's .bss can be cleared by BOOT.  Applications link
 * .noinit to the top 1 KB of DTCM and start their main stack below it;
 * BOOT never reads or writes this window. */
#define BOOT_NOINIT_ADDR        0x2001FC00UL
#define BOOT_NOINIT_SIZE        0x00000400UL
#define BOOT_NOINIT_END         0x20020000UL

/* Appendix B: the metadata sector is written append-only.  Each record
 * has its own 32-byte area; the last word is the CRC of the first 28. */
#define BOOT_META_HEADER_OFFSET    0x000UL
#define BOOT_META_VERIFIED_OFFSET  0x100UL
#define BOOT_META_ACTIVATED_OFFSET 0x120UL
#define BOOT_META_CONFIRMED_OFFSET 0x140UL
#define BOOT_META_REJECTED_OFFSET  0x160UL
#define BOOT_META_RECORD_SIZE      32UL
#define BOOT_META_HEADER_SIZE      256UL

#define BOOT_META_VERIFIED_MAGIC   0x56455249UL  /* "VERI" */
#define BOOT_META_ACTIVATED_MAGIC  0x41435456UL  /* "ACTV" */
#define BOOT_META_CONFIRMED_MAGIC  0x434F4E46UL  /* "CONF" */
#define BOOT_META_REJECTED_MAGIC   0x52454A45UL  /* "REJE" */

/* Rejection reasons (Appendix B, REJECTED record value). */
#define BOOT_REJECT_TRIAL_LIMIT    0x00000001UL

/* Sector map inside a slot: sectors 3-11 of a bank.  Sector 3 is 16 KB,
 * sector 4 is 64 KB and sectors 5-11 are 128 KB each. */
#define BOOT_SLOT_SECTOR_COUNT     9U

typedef enum {
    BOOT_SLOT_A = 0,
    BOOT_SLOT_B = 1
} boot_slot_t;

static inline boot_slot_t boot_other_slot(boot_slot_t slot)
{
    return slot == BOOT_SLOT_A ? BOOT_SLOT_B : BOOT_SLOT_A;
}

static inline uint32_t boot_bank_base(boot_slot_t slot)
{
    return slot == BOOT_SLOT_A ? BOOT_BANK1_BASE : BOOT_BANK2_BASE;
}

static inline uint32_t boot_meta_base(boot_slot_t slot)
{
    return slot == BOOT_SLOT_A ? BOOT_META_A_BASE : BOOT_META_B_BASE;
}

static inline uint32_t boot_slot_base(boot_slot_t slot)
{
    return slot == BOOT_SLOT_A ? BOOT_SLOT_A_BASE : BOOT_SLOT_B_BASE;
}

/* Physical HAL sector number of a slot sector (bank 2 starts at 12). */
static inline uint32_t boot_slot_sector_number(boot_slot_t slot,
                                               uint32_t index)
{
    return (slot == BOOT_SLOT_A ? 3U : 15U) + index;
}

/* Physical HAL sector number of the metadata sector. */
static inline uint32_t boot_meta_sector_number(boot_slot_t slot)
{
    return slot == BOOT_SLOT_A ? 2U : 14U;
}

/* Offset of a slot sector inside the bank. */
static inline uint32_t boot_slot_sector_offset(uint32_t index)
{
    if (index == 0U) {
        return BOOT_SLOT_OFFSET;                  /* 0x0C000, 16 KB     */
    }
    if (index == 1U) {
        return BOOT_SLOT_OFFSET + 0x00004000UL;   /* 0x10000, 64 KB     */
    }
    return BOOT_SLOT_OFFSET + 0x00014000UL +
           ((index - 2U) * 0x00020000UL);         /* 0x20000, 128 KB    */
}

static inline uint32_t boot_slot_sector_size(uint32_t index)
{
    if (index == 0U) {
        return 0x00004000UL;
    }
    if (index == 1U) {
        return 0x00010000UL;
    }
    return 0x00020000UL;
}

#endif /* BOOT_LAYOUT_H */
