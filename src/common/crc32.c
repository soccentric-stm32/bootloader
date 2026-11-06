/**
 * @file    crc32.c
 * @brief   CRC-32/ISO-HDLC (zlib-compatible) using the STM32 hardware
 *          CRC unit.
 *
 * @details
 * Implementation notes (why each part looks the way it does):
 *
 * - **Configuration**: reflected input, 0x04C11DB7 polynomial, init
 *   `0xFFFFFFFF`, reflected output - the standard CRC-32/ISO-HDLC.
 * - **Word feeding**: the CRC unit is *not* the reflected algorithm by
 *   default, so feeding byte-swapped words (via `__REV`) into a unit
 *   with REV_IN yields the same register state that a software
 *   table-driven implementation produces.  This is the "hardware does
 *   the same thing as `zlib.crc32`" contract (BGEN-005).
 * - **Tail**: the final 1-3 bytes are pushed with byte or half-word
 *   writes to the same register, so every byte traverses the
 *   hardware regardless of the buffer alignment or length.
 * - **Reset order**: `INIT` is written *before* `RESET` is triggered;
 *   the reset latches the current `INIT` value, so an accidental
 *   pre-state (e.g. `INIT == 0` from a power-on) cannot leak in.
 *
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  STM32F767ZIT6
 * @srs     BGEN-005 - hardware CRC unit, result equal to zlib.crc32
 */

#include "stm32f7xx_hal.h"
#include "crc32.h"

/**
 * @brief   Push the final 1-3 bytes of a CRC buffer through the
 *          hardware CRC register.
 *
 * @details The CRC unit accepts 8- and 16-bit writes to the data
 *          register, so the tail bytes can be pushed without a
 *          software fallback.  The half-word is composed big-endian so
 *          the hardware sees the same byte order it saw in the whole-word
 *          path (consistent with ST's `CRC_Handle_8()` convention).
 *
 * @param[in] p    Pointer to the tail bytes.
 * @param[in] len  Number of tail bytes (1, 2 or 3).  Other values are
 *                 not handled by this helper (the caller passes 0..3).
 */
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

/**
 * @brief   See @ref boot_crc32 (crc32.h) for the full contract.
 *
 * @details Implementation order:
 *          1. Enable the CRC clock (idempotent).
 *          2. Program `INIT = 0xFFFFFFFF` and trigger a hardware RESET
 *             (with a self-clearing busy bit) so the register starts
 *             from the known state every call.
 *          3. Push whole 32-bit words, byte-swapped (`__REV`), so the
 *             hardware's bit-reflection matches `zlib.crc32`'s output.
 *          4. Push the 1-3-byte tail via `crc32_tail` so that every
 *             byte traverses the hardware.
 *          5. Read the final state and XOR with `0xFFFFFFFF` (the
 *             standard final XOR of the CRC-32/ISO-HDLC family).
 *
 *          For `len == 0`, skip the loop and return the empty-data
 *          CRC (which XORs back to `0x00000000` - matching zlib).
 */
uint32_t boot_crc32(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t state;

    __HAL_RCC_CRC_CLK_ENABLE();

    /* Program INIT and the reflection mode first, then reset: RESET loads
     * DR from the *current* INIT value.  (It only worked before because
     * INIT was already 0xFFFFFFFF.) */
    CRC->INIT = 0xFFFFFFFFUL;
    CRC->CR   = CRC_CR_REV_IN_0 | CRC_CR_REV_OUT | CRC_CR_RESET;
    while ((CRC->CR & CRC_CR_RESET) != 0U) {
        /* reset is self-clearing */
    }

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
