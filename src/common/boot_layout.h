/**
 * @file    boot_layout.h
 * @brief   Dual-bank flash and RAM layout shared by BOOT, the applications
 *          and the host image tool.
 * @details The only place physical addresses are spelled out (LAY-001..005).
 *          All addresses are AXIM with SWP_FB = 0, the state BOOT always
 *          runs in (SWP-003).
 *
 * Bank layout (section 3.1):
 *
 * | Offset  | Size        | Contents                          |
 * |---------|-------------|-----------------------------------|
 * | 0x00000 | 32 KB       | BOOT region (sectors 0-1)         |
 * | 0x08000 | 16 KB       | Append-only metadata (sector 2)   |
 * | 0x0C000 | 976 KB      | Application slot (sectors 3-11)   |
 * | 0x40000 | 16 KB       | Factory identity (bank 2 only)    |
 *
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  STM32F767ZIT6, dual-bank flash (nDBANK = 0)
 * @srs     LAY-001..LAY-005 (section 3), SWP-003, HND-002, IMG-003,
 *          A-01, A-9, Appendix B
 */

#ifndef BOOT_LAYOUT_H
#define BOOT_LAYOUT_H

#include <stdint.h>

/** Base address of bank 1 (AXIM, SWP_FB = 0). */
#define BOOT_BANK1_BASE         0x08000000UL

/** Base address of bank 2 (AXIM, SWP_FB = 0). */
#define BOOT_BANK2_BASE         0x08100000UL

/** Size of one flash bank (1 MB, LAY-001). */
#define BOOT_BANK_SIZE          0x00100000UL

/** Offset of the BOOT region inside a bank (sectors 0-1, 32 KB). */
#define BOOT_REGION_OFFSET      0x00000000UL

/** Size of the BOOT region (32 KB). */
#define BOOT_REGION_SIZE        0x00008000UL

/** Offset of the metadata sector inside a bank (sector 2, 16 KB). */
#define BOOT_META_OFFSET        0x00008000UL

/** Size of the metadata sector (16 KB). */
#define BOOT_META_SIZE          0x00004000UL

/** Offset of the application slot inside a bank (sectors 3-11, 976 KB). */
#define BOOT_SLOT_OFFSET        0x0000C000UL

/** Size of the application slot (976 KB). */
#define BOOT_SLOT_SIZE          0x000F4000UL

/**
 * @brief   Link address of every application (LAY-002).
 * @details The bank swap maps either slot here, so one binary runs from
 *          both banks.
 */
#define BOOT_APP_LOAD_ADDR      (BOOT_BANK1_BASE + BOOT_SLOT_OFFSET)

/** Base of the application slot in bank 1 (slot A). */
#define BOOT_SLOT_A_BASE        (BOOT_BANK1_BASE + BOOT_SLOT_OFFSET)

/** Base of the application slot in bank 2 (slot B). */
#define BOOT_SLOT_B_BASE        (BOOT_BANK2_BASE + BOOT_SLOT_OFFSET)

/** Base of the metadata sector in bank 1 (slot A). */
#define BOOT_META_A_BASE        (BOOT_BANK1_BASE + BOOT_META_OFFSET)

/** Base of the metadata sector in bank 2 (slot B). */
#define BOOT_META_B_BASE        (BOOT_BANK2_BASE + BOOT_META_OFFSET)

/**
 * @brief   In-bank offset of the factory identity sector (A-01).
 * @details Bank 2 sector 13 holds the PRV1 identity record; BOOT never
 *          erases or writes it.  With SWP_FB = 1 it appears at 0x08004000.
 */
#define BOOT_IDENTITY_OFFSET    0x00004000UL

/** Size of the identity sector (16 KB). */
#define BOOT_IDENTITY_SIZE      0x00004000UL

/** Unswapped base of the identity sector (bank 2). */
#define BOOT_IDENTITY_BASE      (BOOT_BANK2_BASE + BOOT_IDENTITY_OFFSET)

/** Swapped (`SWP_FB = 1`) view of the identity sector. */
#define BOOT_IDENTITY_SWAPPED   (BOOT_BANK1_BASE + BOOT_IDENTITY_OFFSET)

/** Physical HAL sector number of the identity area (bank 2, index 1). */
#define BOOT_IDENTITY_SECTOR    13U

/**
 * @brief   Start of internal RAM (DTCM + SRAM1 + SRAM2, contiguous).
 * @details IMG-003: a valid initial SP is in (BOOT_RAM_START, BOOT_RAM_END].
 */
#define BOOT_RAM_START          0x20000000UL
#define BOOT_RAM_END            0x20080000UL /**< End of RAM; SP may equal it. */

/**
 * @brief   Start of the no-init window for application fault records (A-9).
 * @details BOOT uses DTCM 0x20000000..0x2000EFFF and SRAM1
 *          0x20020000..0x20020FFF (USART3 DMA ring), so a record after the
 *          application's .bss could be overwritten.  Applications link
 *          .noinit to the top 1 KB of DTCM and start their stack below it.
 *          BOOT never touches this window.
 */
#define BOOT_NOINIT_ADDR        0x2001FC00UL

/** Size of the no-init window (1 KB). */
#define BOOT_NOINIT_SIZE        0x00000400UL

/** One-past-the-end address of the no-init window. */
#define BOOT_NOINIT_END         0x20020000UL

/*
 * Append-only metadata sector layout (Appendix B): 32-byte records at the
 * offsets below, each ending in the CRC-32 of its first 28 bytes.
 */

/** Offset of the 256-byte header record inside the metadata sector. */
#define BOOT_META_HEADER_OFFSET    0x000UL

/** Offset of the VERIFIED record ("VERI" magic). */
#define BOOT_META_VERIFIED_OFFSET  0x100UL

/** Offset of the ACTIVATED record ("ACTV" magic). */
#define BOOT_META_ACTIVATED_OFFSET 0x120UL

/** Offset of the CONFIRMED record ("CONF" magic). */
#define BOOT_META_CONFIRMED_OFFSET 0x140UL

/** Offset of the REJECTED record ("REJE" magic). */
#define BOOT_META_REJECTED_OFFSET  0x160UL

/**
 * @brief   Offset of the TRIAL record block (P2-6 / OPEN-B03).
 * @details Each pending boot appends one record; the count of programmed
 *          records is the power-loss-safe trial counter.
 */
#define BOOT_META_TRIAL_OFFSET     0x180UL

/** Number of TRIAL records in the metadata sector (8). */
#define BOOT_META_TRIAL_RECORDS    8UL

/** Size of one metadata record in bytes (32). */
#define BOOT_META_RECORD_SIZE      32UL

/** Size of the metadata header record in bytes (256). */
#define BOOT_META_HEADER_SIZE      256UL

/** Magic of the VERIFIED record: ASCII "VERI". */
#define BOOT_META_VERIFIED_MAGIC   0x56455249UL

/** Magic of the ACTIVATED record: ASCII "ACTV". */
#define BOOT_META_ACTIVATED_MAGIC  0x41435456UL

/** Magic of the CONFIRMED record: ASCII "CONF". */
#define BOOT_META_CONFIRMED_MAGIC  0x434F4E46UL

/** Magic of the REJECTED record: ASCII "REJE". */
#define BOOT_META_REJECTED_MAGIC   0x52454A45UL

/** Magic of the TRIAL records: ASCII "TRIL". */
#define BOOT_META_TRIAL_MAGIC      0x5452494CUL

/** REJECTED record value: trial limit exhausted (Appendix B). */
#define BOOT_REJECT_TRIAL_LIMIT    0x00000001UL

/**
 * @brief   Number of flash sectors in one slot.
 * @details Bank sectors 3-11: 16 KB, 64 KB, then seven of 128 KB.
 */
#define BOOT_SLOT_SECTOR_COUNT     9U

/**
 * @brief   Identifier of one of the two application slots.
 */
typedef enum {
    BOOT_SLOT_A = 0,   /**< Bank 1 slot. */
    BOOT_SLOT_B = 1    /**< Bank 2 slot. */
} boot_slot_t;

/**
 * @brief   Return the slot opposite to @p slot.
 *
 * @param[in] slot  A slot identifier.
 * @return  `BOOT_SLOT_B` when @p slot is `BOOT_SLOT_A`, and vice versa.
 */
static inline boot_slot_t boot_other_slot(boot_slot_t slot)
{
    return slot == BOOT_SLOT_A ? BOOT_SLOT_B : BOOT_SLOT_A;
}

/**
 * @brief   Return the bank base address of @p slot.
 *
 * @param[in] slot  A slot identifier.
 * @return  `BOOT_BANK1_BASE` or `BOOT_BANK2_BASE`.
 */
static inline uint32_t boot_bank_base(boot_slot_t slot)
{
    return slot == BOOT_SLOT_A ? BOOT_BANK1_BASE : BOOT_BANK2_BASE;
}

/**
 * @brief   Return the metadata sector base of @p slot.
 *
 * @param[in] slot  A slot identifier.
 * @return  `BOOT_META_A_BASE` or `BOOT_META_B_BASE`.
 */
static inline uint32_t boot_meta_base(boot_slot_t slot)
{
    return slot == BOOT_SLOT_A ? BOOT_META_A_BASE : BOOT_META_B_BASE;
}

/**
 * @brief   Return the application slot base of @p slot.
 *
 * @param[in] slot  A slot identifier.
 * @return  `BOOT_SLOT_A_BASE` or `BOOT_SLOT_B_BASE`.
 */
static inline uint32_t boot_slot_base(boot_slot_t slot)
{
    return slot == BOOT_SLOT_A ? BOOT_SLOT_A_BASE : BOOT_SLOT_B_BASE;
}

/**
 * @brief   Return the physical HAL sector number of a slot sector.
 * @details Bank 1 sectors 3-11, bank 2 sectors 15-23.
 *
 * @param[in] slot    A slot identifier.
 * @param[in] index   Slot-sector index, `0 .. BOOT_SLOT_SECTOR_COUNT-1`.
 * @return  The physical sector number to pass to the FLASH HAL.
 */
static inline uint32_t boot_slot_sector_number(boot_slot_t slot,
                                               uint32_t index)
{
    return (slot == BOOT_SLOT_A ? 3U : 15U) + index;
}

/**
 * @brief   Return the physical HAL sector number of a metadata sector.
 *
 * @param[in] slot  A slot identifier.
 * @return  `2U` (bank 1) or `14U` (bank 2).
 */
static inline uint32_t boot_meta_sector_number(boot_slot_t slot)
{
    return slot == BOOT_SLOT_A ? 2U : 14U;
}

/**
 * @brief   Return the in-bank offset of a slot sector.
 * @details 0 -> 0x0C000, 1 -> 0x10000, n >= 2 -> 0x20000 + (n - 2) * 128 KB.
 *
 * @param[in] index  Slot-sector index, `0 .. BOOT_SLOT_SECTOR_COUNT-1`.
 * @return  The offset inside the bank of that sector.
 */
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

/**
 * @brief   Return the size in bytes of a slot sector.
 * @details 0 -> 16 KB, 1 -> 64 KB, n >= 2 -> 128 KB.
 *
 * @param[in] index  Slot-sector index, `0 .. BOOT_SLOT_SECTOR_COUNT-1`.
 * @return  The sector size in bytes.
 */
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
