/**
 * @file    fake_flash.c
 * @brief   In-memory 2 MB dual-bank flash for the host tests.
 * @details The mapping and address guards mirror src/bsp/flash.c, without
 *          the STM32 controller.  Tests can inject raw bytes (torn records,
 *          bad images) through fake_flash_write_raw().
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  Host tests
 */

#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <unistd.h>
#include "boot_layout.h"
#include "flash.h"
#include "fake_flash.h"

#define FAKE_FLASH_SIZE (2U * BOOT_BANK_SIZE)

/* The boot modules address flash through the absolute STM32 addresses, so
 * the host test maps the two banks at 0x08000000.  This keeps the real
 * pointer arithmetic and CRC ranges under test. */

/** Host mapping of the fake flash, or NULL before the first reset. */
static uint8_t *memory;

#ifndef _WIN32
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE MAP_FIXED
#endif
#endif

/** @brief Map the two flash banks at their STM32 addresses, once. */
static void map_memory(void)
{
    if (memory != NULL) {
        return;
    }
    void *wanted = (void *)(uintptr_t)BOOT_BANK1_BASE;
#ifdef _WIN32
    void *mapped = VirtualAlloc(wanted, FAKE_FLASH_SIZE,
                                MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (mapped == NULL) {
        fprintf(stderr, "VirtualAlloc fake flash failed (%lu)\n",
                (unsigned long)GetLastError());
        _exit(1);
    }
#else
    void *mapped = mmap(wanted, FAKE_FLASH_SIZE, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                        -1, 0);
    if (mapped == MAP_FAILED) {
        perror("mmap fake flash");
        _exit(1);
    }
#endif
    memory = (uint8_t *)mapped;
}

/** @name Flash-driver state mirrored by the fake controller. @{ */
static boot_slot_t g_active = BOOT_SLOT_A;  /**< Active slot. */
static bool g_active_valid;                 /**< Active slot is bootable. */
static bool g_allowed = true;               /**< Erase/program gate. */
static bool g_fail_meta;                    /**< Injected metadata failure. */
static uint32_t g_fail_meta_offset;         /**< Offset that fails. */
/** @} */

/** @brief Reset the fake flash; see fake_flash.h. */
void fake_flash_reset(void)
{
    map_memory();
    memset(memory, 0xFF, FAKE_FLASH_SIZE);
    g_active = BOOT_SLOT_A;
    g_active_valid = false;
    g_allowed = true;
    g_fail_meta = false;
    g_fail_meta_offset = 0U;
}

/** @brief Translate an address to host memory; see fake_flash.h. */
uint8_t *fake_flash_ptr(uint32_t address)
{
    map_memory();
    if (address < BOOT_BANK1_BASE ||
        address >= BOOT_BANK1_BASE + FAKE_FLASH_SIZE) {
        return NULL;
    }
    return &memory[address - BOOT_BANK1_BASE];
}

/** @brief Write raw bytes; see fake_flash.h. */
void fake_flash_write_raw(uint32_t address, const void *data, uint32_t len)
{
    uint8_t *dst = fake_flash_ptr(address);
    if (dst != NULL) {
        memcpy(dst, data, len);
    }
}

/** @brief Inject a metadata programming failure; see fake_flash.h. */
void fake_flash_fail_meta_offset(uint32_t offset, bool fail)
{
    g_fail_meta = fail;
    g_fail_meta_offset = offset;
}

/** @brief Set the erase/program gate; mirrors flash.h. */
void boot_flash_set_allowed(bool allowed)
{
    g_allowed = allowed;
}

/** @brief Publish the active slot; mirrors flash.h. */
void boot_flash_set_active(boot_slot_t slot, bool valid)
{
    g_active = slot;
    g_active_valid = valid;
}

/** @brief Return the update target; mirrors flash.h. */
boot_slot_t boot_flash_target_slot(void)
{
    return g_active_valid ? boot_other_slot(g_active) : BOOT_SLOT_A;
}

/** @brief Test a range for 0xFF; mirrors flash.h. */
bool boot_flash_range_erased(uint32_t address, uint32_t len)
{
    const uint8_t *src = fake_flash_ptr(address);
    if (src == NULL) {
        return false;
    }
    for (uint32_t i = 0U; i < len; i++) {
        if (src[i] != 0xFFU) {
            return false;
        }
    }
    return true;
}

/** @brief Erase a metadata sector; mirrors flash.h. */
bool boot_flash_erase_meta(boot_slot_t slot)
{
    if (!g_allowed || slot != boot_flash_target_slot()) {
        return false;
    }
    uint8_t *dst = fake_flash_ptr(boot_meta_base(slot));
    if (dst == NULL) {
        return false;
    }
    memset(dst, 0xFF, BOOT_META_SIZE);
    return true;
}

/** @brief Erase one slot sector; mirrors flash.h. */
bool boot_flash_erase_slot_sector(boot_slot_t slot, uint32_t index)
{
    if (!g_allowed || slot != boot_flash_target_slot() ||
        index >= BOOT_SLOT_SECTOR_COUNT) {
        return false;
    }
    /* boot_slot_sector_offset() is relative to the bank, like the
     * physical sector number the real driver erases. */
    uint8_t *dst = fake_flash_ptr(boot_bank_base(slot) +
                                  boot_slot_sector_offset(index));
    if (dst == NULL) {
        return false;
    }
    memset(dst, 0xFF, boot_slot_sector_size(index));
    return true;
}

/**
 * @brief   Mirror the metadata write guard of the real flash driver.
 * @param[in] slot    Slot to write.
 * @param[in] offset  Metadata-sector offset.
 * @param[in] len     Length in bytes.
 * @return true when the write would be allowed.
 */
static bool meta_allowed(boot_slot_t slot, uint32_t offset, uint32_t len)
{
    if (offset >= BOOT_META_SIZE || len > BOOT_META_SIZE - offset) {
        return false;
    }
    if (offset == BOOT_META_HEADER_OFFSET) {
        return slot == boot_flash_target_slot() &&
               len <= BOOT_META_HEADER_SIZE;
    }
    if (len != BOOT_META_RECORD_SIZE) {
        return false;
    }
    bool trial = offset >= BOOT_META_TRIAL_OFFSET &&
                 ((offset - BOOT_META_TRIAL_OFFSET) %
                  BOOT_META_RECORD_SIZE) == 0U &&
                 (offset - BOOT_META_TRIAL_OFFSET) /
                     BOOT_META_RECORD_SIZE < BOOT_META_TRIAL_RECORDS;
    if (slot == boot_flash_target_slot()) {
        return offset == BOOT_META_VERIFIED_OFFSET ||
               offset == BOOT_META_ACTIVATED_OFFSET ||
               offset == BOOT_META_CONFIRMED_OFFSET ||
               offset == BOOT_META_REJECTED_OFFSET || trial;
    }
    return offset == BOOT_META_CONFIRMED_OFFSET ||
           offset == BOOT_META_REJECTED_OFFSET || trial;
}

/** @brief Program metadata; mirrors flash.h. */
bool boot_flash_program_meta(boot_slot_t slot, uint32_t offset,
                             const void *data, uint32_t len)
{
    if (!g_allowed || data == NULL || (len % 4U) != 0U ||
        !meta_allowed(slot, offset, len)) {
        return false;
    }
    if (g_fail_meta && offset == g_fail_meta_offset) {
        return false;
    }
    uint32_t address = boot_meta_base(slot) + offset;
    if (!boot_flash_range_erased(address, len)) {
        return false;
    }
    fake_flash_write_raw(address, data, len);
    return true;
}

/** @brief Program slot words; mirrors flash.h. */
bool boot_flash_program_slot(boot_slot_t slot, uint32_t offset,
                             const void *data, uint32_t len,
                             uint32_t *fail_addr)
{
    if (fail_addr != NULL) {
        *fail_addr = 0U;
    }
    if (!g_allowed || data == NULL || (offset % 4U) != 0U ||
        (len % 4U) != 0U || offset > BOOT_SLOT_SIZE ||
        len > BOOT_SLOT_SIZE - offset || slot != boot_flash_target_slot()) {
        return false;
    }
    uint32_t address = boot_slot_base(slot) + offset;
    if (!boot_flash_range_erased(address, len)) {
        return false;
    }
    fake_flash_write_raw(address, data, len);
    return true;
}
