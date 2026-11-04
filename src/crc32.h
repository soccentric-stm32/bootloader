/*
 * crc32.h - CRC-32/ISO-HDLC (zlib) using the STM32 CRC unit
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * Target:  STM32F767ZIT6
 * SRS:     BGEN-005, IMG-004, IMG-005, Appendix B
 */

#ifndef CRC32_H
#define CRC32_H

#include <stddef.h>
#include <stdint.h>

/* Returns zlib.crc32(data, len) for any length and alignment. */
uint32_t boot_crc32(const void *data, size_t len);

#endif /* CRC32_H */
