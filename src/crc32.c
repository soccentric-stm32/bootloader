/*
 * crc32.c - CRC-32/ISO-HDLC (zlib) using the STM32 CRC unit
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * Target:  STM32F767ZIT6
 * SRS:     BGEN-005 - hardware CRC unit, result equal to zlib.crc32
 *
 * The CRC unit is configured for byte-wise input bit reversal, the
 * 0x04C11DB7 polynomial, init 0xFFFFFFFF and output bit reversal.  Feeding
 * byte-swapped little-endian words produces the reflected register state
 * used by zlib.  The final 1-3 bytes are written with byte and half-word
 * accesses to the same unit, so every byte goes through the hardware for any
 * length and alignment.
 */

#include "stm32f7xx_hal.h"
#include "crc32.h"

static void crc32_tail(const uint8_t *p, size_t len)
{
    /* The CRC unit accepts byte and half-word writes for the final 1-3
     * bytes, so every byte goes through the hardware.  The half-word is
     * written big-endian like ST's CRC_Handle_8(). */
    if (len == 1U) {
        *(volatile uint8_t *)&CRC->DR = p[0];
    } else if (len == 2U) {
        uint16_t half = (uint16_t)(((uint16_t)p[0] << 8U) | p[1]);
        *(volatile uint16_t *)&CRC->DR = half;
    } else if (len == 3U) {
        uint16_t half = (uint16_t)(((uint16_t)p[0] << 8U) | p[1]);
        *(volatile uint16_t *)&CRC->DR = half;
        *(volatile uint8_t *)&CRC->DR = p[2];
    }
}

uint32_t boot_crc32(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t state;

    __HAL_RCC_CRC_CLK_ENABLE();

    /* Reset the unit, then set the init value and the reflection mode that
     * is equivalent to the reflected zlib algorithm. */
    CRC->CR = CRC_CR_RESET;
    while ((CRC->CR & CRC_CR_RESET) != 0U) {
        /* reset is self-clearing */
    }
    CRC->INIT = 0xFFFFFFFFUL;
    CRC->CR   = CRC_CR_REV_IN_0 | CRC_CR_REV_OUT;

    size_t words = len / 4U;
    for (size_t i = 0U; i < words; i++) {
        uint32_t word;
        for (uint32_t b = 0U; b < 4U; b++) {
            ((uint8_t *)&word)[b] = p[(i * 4U) + b];
        }
        /* Put the first byte in the most significant position, then let
         * the unit reverse the bits inside each byte. */
        CRC->DR = __REV(word);
    }

    size_t tail = len - (words * 4U);
    if (tail > 0U) {
        crc32_tail(p + (words * 4U), tail);
    }
    state = (words > 0U || tail > 0U) ? CRC->DR : 0xFFFFFFFFUL;
    return state ^ 0xFFFFFFFFUL;
}
