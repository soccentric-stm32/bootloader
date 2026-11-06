/**
 * @file    crc32.h
 * @brief   CRC-32/ISO-HDLC (identical to `zlib.crc32`) using the STM32
 *          hardware CRC unit.
 *
 * @details
 * Single, unambiguous entry point for every CRC-32 the bootloader
 * computes:
 *
 * - the *image* CRC-32 over the slot bytes (`boot_image_crc_matches`),
 * - the *header* CRC-32 stored in `boot_image_header_t.header_crc32`,
 * - the record-level CRCs in the append-only metadata layout (Appendix
 *   B of the SRS - the last word of every 32-byte record is the CRC-32
 *   of the first 28 bytes).
 *
 * The algorithm is the industry-standard CRC-32/ISO-HDLC (the same one
 * `gzip`, `zlib`, and `adler`-era tools use), so any external tool that
 * computes a "standard" CRC-32 gets bit-compatible values with this
 * module.
 *
 * The F7's dedicated CRC peripheral is used (BGEN-005) - it is clocked
 * independently of the core, handles unaligned starts, and is
 * fundamentally faster than a software table for the ~1 MB image scans
 * the selection module performs.
 *
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
 *
 * @details Bit-identical to the reference:
 *
 *     uint32_t zlib_crc32(uint32_t c, const Bytef *buf, uInt len);
 *     // with c = 0xFFFFFFFFUL as the standard initial value
 *
 * (i.e. `zlib.crc32(0, data, len)`).
 *
 * @param[in] data  Pointer to the buffer (may be unaligned; any valid
 *                  readable memory address works).
 * @param[in] len   Length in bytes (0 is allowed, yields the empty-data
 *                  CRC-32, `0x00000000` for the standard init).
 * @return  The 32-bit CRC value in "zlib" endianness and post-processing
 *          (i.e. the value `zlib.crc32()` would return).
 *
 * @note    `data == NULL` is /not/ a defined case; callers must pass a
 *          valid, readable region.  The function is blocking: it
 *          waits for the hardware to finish, so it is not preemptable in
 *          the middle of a scan - this matches how the rest of the
 *          selection / metadata code treats it.
 *
 * @srs     BGEN-005 (hardware CRC), IMG-004 (image CRC),
 *          Appendix B (record CRCs)
 */
uint32_t boot_crc32(const void *data, size_t len);

#endif /* CRC32_H */
