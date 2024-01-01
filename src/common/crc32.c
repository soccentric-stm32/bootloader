/**
 * @file    crc32.c
 * @brief   CRC-32/ISO-HDLC (zlib-compatible) on the STM32 hardware CRC unit.
 * @details Polynomial 0x04C11DB7, init 0xFFFFFFFF, byte-reversed input,
 *          reversed output, final XOR 0xFFFFFFFF.  Whole words are fed
 *          byte-swapped (`__REV`) and the 1-3 byte tail with narrow writes,
 *          so every byte goes through the hardware.
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  STM32F767ZIT6
 * @srs     BGEN-005 - hardware CRC unit, result equal to zlib.crc32
 */

#include "stm32f7xx_hal.h"
#include "crc32.h"

/**
 * @brief   Feed the final 1-3 bytes into the CRC data register.
 * @details Uses 8- and 16-bit writes; the half-word is big-endian, as in
 *          ST's `CRC_Handle_8()`.
 *
 * @param[in] p    Tail bytes.
 * @param[in] len  Tail length, 0..3; 0 writes nothing.
 */
static void crc32_tail(const uint8_t *p, size_t len)
{
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

/**
 * @brief   Compute the CRC-32/ISO-HDLC of a buffer.
 * @details Enables the CRC clock, resets the unit to INIT, feeds the data and
 *          applies the final XOR.
 *
 * @param[in] data  Buffer to checksum; any alignment, must not be NULL.
 * @param[in] len   Length in bytes; 0 yields 0x00000000.
 * @return  CRC-32 value as zlib returns it.
 */
uint32_t boot_crc32(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t state = 0U;

    __HAL_RCC_CRC_CLK_ENABLE();

    /* Write INIT before RESET: the reset loads DR from the current INIT. */
    CRC->INIT = 0xFFFFFFFFUL;
    CRC->CR   = CRC_CR_REV_IN_0 | CRC_CR_REV_OUT | CRC_CR_RESET;
    while ((CRC->CR & CRC_CR_RESET) != 0U) {
        /* RESET is self-clearing. */
    }

    size_t words = len / 4U;
    for (size_t i = 0U; i < words; i++) {
        uint32_t word = 0U;
        for (uint32_t b = 0U; b < 4U; b++) {
            ((uint8_t *)&word)[b] = p[(i * 4U) + b];
        }
        /* First byte goes in the MSB; the unit reverses bits per byte. */
        CRC->DR = __REV(word);
    }

    size_t tail = len - (words * 4U);
    if (tail > 0U) {
        crc32_tail(p + (words * 4U), tail);
    }
    state = (words > 0U || tail > 0U) ? CRC->DR : 0xFFFFFFFFUL;
    return state ^ 0xFFFFFFFFUL;
}
