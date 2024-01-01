/**
 * @file    flash.c
 * @brief   Guarded flash wrapper implementation.
 * @details Never touches option bytes, OTP or the bank-swap bit.  Every
 *          programmed word is read back, the identity sector (A-01) is never
 *          erased, and the IWDG is kicked around each erase.
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  STM32F767ZIT6, dual-bank flash
 * @srs     LAY-004, SWP-003, BSAFE-001..004, UPD-002/003/005
 */

#include "stm32f7xx_hal.h"
#include "boot_layout.h"
#include "flash.h"
#include "iwdg.h"

/**
 * @name Module state
 * @brief Selection published by the selection module.
 * @{
 */

/** Active slot. */
static boot_slot_t g_active_slot = BOOT_SLOT_A;

/** true when @ref g_active_slot holds a bootable image. */
static bool g_active_valid;

/** false forbids all erase/program (BSAFE-005). */
static bool g_flash_allowed;

/** @} */

/** @brief Set the erase/program gate; see @ref boot_flash_set_allowed. */
void boot_flash_set_allowed(bool allowed)
{
    g_flash_allowed = allowed;
}

/** @brief Record the active slot; see @ref boot_flash_set_active. */
void boot_flash_set_active(boot_slot_t slot, bool valid)
{
    g_active_slot  = slot;
    g_active_valid = valid;
}

/** @brief Return the update target; see @ref boot_flash_target_slot. */
boot_slot_t boot_flash_target_slot(void)
{
    return g_active_valid ? boot_other_slot(g_active_slot) : BOOT_SLOT_A;
}

/**
 * @brief   Test that [address, address+len) lies inside [base, base+size).
 * @details Overflow-safe.
 *
 * @param[in] address  Range start.
 * @param[in] len      Range length in bytes; 0 fails.
 * @param[in] base     Window start.
 * @param[in] size     Window size in bytes.
 * @return  true when the range fits.
 */
static bool range_inside(uint32_t address, uint32_t len, uint32_t base,
                         uint32_t size)
{
    if (len == 0U) {
        return false;
    }
    if (address < base) {
        return false;
    }
    uint32_t offset = address - base;
    return offset <= size && len <= (size - offset);
}

/**
 * @brief   Test a range for 0xFF; see @ref boot_flash_range_erased.
 * @details Reads words, then the 1-3 byte tail bytewise.  Touches no
 *          controller state.
 */
bool boot_flash_range_erased(uint32_t address, uint32_t len)
{
    const volatile uint32_t *words = (const volatile uint32_t *)address;
    uint32_t count = len / 4U;

    for (uint32_t i = 0U; i < count; i++) {
        if (words[i] != 0xFFFFFFFFUL) {
            return false;
        }
    }
    for (uint32_t i = (count * 4U); i < len; i++) {
        if (((const volatile uint8_t *)address)[i] != 0xFFU) {
            return false;
        }
    }
    return true;
}

/**
 * @brief   Unlock the flash controller and clear EOP and error flags.
 * @details Clearing stale flags keeps an earlier failure from failing the
 *          next operation.
 * @return  true when unlocked.
 */
static bool flash_begin(void)
{
    if (HAL_FLASH_Unlock() != HAL_OK) {
        return false;
    }
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_ALL_ERRORS);
    return true;
}

/**
 * @brief   Lock the flash controller.
 */
static void flash_end(void)
{
    (void)HAL_FLASH_Lock();
}

/**
 * @brief   Erase one flash sector by physical number.
 * @details Refuses BOOT_IDENTITY_SECTOR (A-01).  Uses voltage range 3
 *          (2.7-3.6 V).  The controller must be unlocked.
 *
 * @param[in] sector  Physical sector, 0..11 bank 1, 12..23 bank 2.
 * @return  true on success.
 */
static bool erase_sector(uint32_t sector)
{
    /* A-01: never erase the factory identity record. */
    if (sector == BOOT_IDENTITY_SECTOR) {
        return false;
    }
    FLASH_EraseInitTypeDef erase = {0};
    uint32_t error = 0U;

    erase.TypeErase    = FLASH_TYPEERASE_SECTORS;
    erase.Banks        = sector >= 12U ? FLASH_BANK_2 : FLASH_BANK_1;
    erase.Sector       = sector;
    erase.NbSectors    = 1U;
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;   /* 2.7-3.6 V, word */

    return HAL_FLASHEx_Erase(&erase, &error) == HAL_OK;
}

/** @brief Erase a metadata sector; see @ref boot_flash_erase_meta. */
bool boot_flash_erase_meta(boot_slot_t slot)
{
    if (!g_flash_allowed || slot != boot_flash_target_slot()) {
        return false;
    }
    /* A sector erase can take ~2 s. */
    boot_iwdg_kick();
    if (!flash_begin()) {
        return false;
    }
    bool ok = erase_sector(boot_meta_sector_number(slot));
    flash_end();
    boot_iwdg_kick();
    return ok;
}

/** @brief Erase a slot sector; see @ref boot_flash_erase_slot_sector. */
bool boot_flash_erase_slot_sector(boot_slot_t slot, uint32_t index)
{
    if (!g_flash_allowed || slot != boot_flash_target_slot() ||
        index >= BOOT_SLOT_SECTOR_COUNT) {
        return false;
    }
    boot_iwdg_kick();
    if (!flash_begin()) {
        return false;
    }
    bool ok = erase_sector(boot_slot_sector_number(slot, index));
    flash_end();
    boot_iwdg_kick();
    return ok;
}

/**
 * @brief   Decide whether a metadata write is allowed.
 * @details The update target accepts the header and any Appendix B record;
 *          the other slot only CONFIRMED, REJECTED and TRIAL, because the
 *          trial counter lives in the booting slot.
 *
 * @param[in] slot    Slot to write.
 * @param[in] offset  Metadata-sector offset.
 * @param[in] len     Length in bytes.
 * @return  true when the write is allowed.
 *
 * @srs     BSAFE-003, P2-6
 */
static bool meta_offset_allowed(boot_slot_t slot, uint32_t offset,
                                uint32_t len)
{
    uint32_t end = offset + len;
    bool target  = slot == boot_flash_target_slot();

    if (len == 0U || offset >= BOOT_META_SIZE ||
        end > BOOT_META_SIZE || end < offset) {
        return false;
    }

    /* Header record: update target only. */
    if (offset == BOOT_META_HEADER_OFFSET) {
        return target && len <= BOOT_META_HEADER_SIZE;
    }

    /* One complete 32-byte record at one of the Appendix B offsets. */
    bool trial_offset = false;
    if (offset >= BOOT_META_TRIAL_OFFSET) {
        uint32_t index = (offset - BOOT_META_TRIAL_OFFSET) /
                         BOOT_META_RECORD_SIZE;
        trial_offset = index < BOOT_META_TRIAL_RECORDS &&
                       ((offset - BOOT_META_TRIAL_OFFSET) %
                        BOOT_META_RECORD_SIZE) == 0U;
    }
    bool record_offset = offset == BOOT_META_VERIFIED_OFFSET ||
                         offset == BOOT_META_ACTIVATED_OFFSET ||
                         offset == BOOT_META_CONFIRMED_OFFSET ||
                         offset == BOOT_META_REJECTED_OFFSET ||
                         trial_offset;
    if (!record_offset || len > BOOT_META_RECORD_SIZE) {
        return false;
    }
    if (target) {
        return true;
    }
    /* BSAFE-003 exception for the booting slot. */
    return offset == BOOT_META_CONFIRMED_OFFSET ||
           offset == BOOT_META_REJECTED_OFFSET ||
           trial_offset;
}

/** @brief Program a metadata record; see @ref boot_flash_program_meta. */
bool boot_flash_program_meta(boot_slot_t slot, uint32_t offset,
                             const void *data, uint32_t len)
{
    if (!g_flash_allowed || data == NULL || (len % 4U) != 0U ||
        !meta_offset_allowed(slot, offset, len)) {
        return false;
    }

    /* Metadata is append-only: never program a word twice. */
    uint32_t address = boot_meta_base(slot) + offset;
    if (!boot_flash_range_erased(address, len)) {
        return false;
    }

    if (!flash_begin()) {
        return false;
    }

    const uint32_t *words = (const uint32_t *)data;
    bool ok = true;
    for (uint32_t i = 0U; i < (len / 4U); i++) {
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, address + (i * 4U),
                              words[i]) != HAL_OK ||
            *(volatile uint32_t *)(address + (i * 4U)) != words[i]) {
            ok = false;
            break;
        }
    }

    flash_end();
    return ok;
}

/** @brief Program slot words; see @ref boot_flash_program_slot. */
bool boot_flash_program_slot(boot_slot_t slot, uint32_t offset,
                             const void *data, uint32_t len,
                             uint32_t *fail_addr)
{
    if (fail_addr != NULL) {
        *fail_addr = 0U;
    }
    if (!g_flash_allowed || data == NULL || (offset % 4U) != 0U ||
        (len % 4U) != 0U ||
        !range_inside(offset, len, 0U, BOOT_SLOT_SIZE)) {
        return false;
    }
    if (slot != boot_flash_target_slot()) {
        return false;
    }

    uint32_t address = boot_slot_base(slot) + offset;
    const uint32_t *words = (const uint32_t *)data;

    if (!flash_begin()) {
        return false;
    }

    bool ok = true;
    for (uint32_t i = 0U; i < (len / 4U); i++) {
        uint32_t word_addr = address + (i * 4U);
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, word_addr,
                              words[i]) != HAL_OK ||
            *(volatile uint32_t *)word_addr != words[i]) {
            if (fail_addr != NULL) {
                *fail_addr = word_addr;
            }
            ok = false;
            break;
        }
    }

    flash_end();
    return ok;
}
