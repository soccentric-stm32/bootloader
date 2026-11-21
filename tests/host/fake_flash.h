/**
 * @file    fake_flash.h
 * @brief   Control surface for the host-test in-memory flash.
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  Host tests
 */

#ifndef FAKE_FLASH_H
#define FAKE_FLASH_H

#include <stdbool.h>
#include <stdint.h>

/** @brief Erase both fake banks and reset the flash-driver state. */
void fake_flash_reset(void);

/**
 * @brief   Translate an STM32 flash address to host memory.
 * @param[in] address  Absolute address inside the fake 2 MB flash.
 * @return Host pointer, or NULL when @p address is outside both banks.
 */
uint8_t *fake_flash_ptr(uint32_t address);

/**
 * @brief   Write raw bytes, bypassing the flash driver's guards.
 * @details Used to inject torn records and pre-existing slot contents.
 * @param[in] address  Absolute destination address.
 * @param[in] data     Source bytes.
 * @param[in] len      Byte count.
 */
void fake_flash_write_raw(uint32_t address, const void *data, uint32_t len);

/**
 * @brief   Make one metadata record offset fail to program.
 * @param[in] offset  Metadata-sector offset to fail, or 0 to clear.
 * @param[in] fail    true to inject the failure, false to clear it.
 */
void fake_flash_fail_meta_offset(uint32_t offset, bool fail);

#endif /* FAKE_FLASH_H */
