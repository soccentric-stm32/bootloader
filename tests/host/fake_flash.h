/*
 * fake_flash.h - control surface for the host-test flash
 */

#ifndef FAKE_FLASH_H
#define FAKE_FLASH_H

#include <stdbool.h>
#include <stdint.h>

void fake_flash_reset(void);
uint8_t *fake_flash_ptr(uint32_t address);
void fake_flash_write_raw(uint32_t address, const void *data, uint32_t len);
void fake_flash_fail_meta_offset(uint32_t offset, bool fail);

#endif /* FAKE_FLASH_H */
