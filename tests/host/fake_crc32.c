/*
 * fake_crc32.c - software CRC-32/ISO-HDLC for the host tests
 *
 * The production implementation uses the STM32 CRC peripheral.  This one is
 * the same reflected 0xEDB88320 algorithm as zlib and keeps the metadata and
 * image checks identical on the host.
 */

#include <stddef.h>
#include <stdint.h>
#include "crc32.h"

uint32_t boot_crc32(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFUL;

    for (size_t i = 0U; i < len; i++) {
        crc ^= p[i];
        for (uint32_t bit = 0U; bit < 8U; bit++) {
            crc = (crc >> 1U) ^
                  (0xEDB88320UL & (uint32_t)-(int32_t)(crc & 1U));
        }
    }
    return crc ^ 0xFFFFFFFFUL;
}
