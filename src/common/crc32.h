/**
 * @file    crc32.h
 * @brief   CRC-32/ISO-HDLC (zlib.crc32) on the STM32 hardware CRC unit.
 * @details One entry point for the image CRC, the header CRC and the
 *          Appendix B record CRCs.  Results match any standard CRC-32 tool.
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  STM32F767ZIT6
 * @srs     BGEN-005, IMG-004, IMG-005, Appendix B
 */

#ifndef CRC32_H
#define CRC32_H

#include <stddef.h>
#include <stdint.h>

/**
 * @brief   Compute the CRC-32/ISO-HDLC of a buffer.
 * @details Equal to `zlib.crc32(0, data, len)`.  Blocks until the hardware
 *          has consumed every byte.
 *
 * @param[in] data  Buffer to checksum; any alignment, must not be NULL.
 * @param[in] len   Length in bytes; 0 yields 0x00000000.
 * @return  CRC-32 value as zlib returns it.
 *
 * @srs     BGEN-005, IMG-004, Appendix B
 */
uint32_t boot_crc32(const void *data, size_t len);

#endif /* CRC32_H */
