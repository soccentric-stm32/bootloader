/**
 * @file    fault.c
 * @brief   HardFault handler (BGEN-006).
 *
 * @details
 * When BOOT hard-faults the board must advertise the failure in a way an
 * operator (or a bench tool) can recognise and then reset itself so the
 * board is not left in a permanently dead state.  The handler:
 *
 * 1. Forces LD3 to be a usable output even if the fault happened before
 *    normal GPIO setup completed (clock + MODER set directly).
 * 2. Emits a recognisable flash pattern - three short (100 ms) blinks
 *    followed by a 400 ms pause, repeated 5 times (5 seconds total) -
 *    which is the documented "fault" signature (BGEN-006).
 * 3. Calls `NVIC_SystemReset()` so the next boot re-enters the normal
 *    sequence.
 *
 * MemManage / BusFault / UsageFault are *not* installed separately, so
 * they escalate to HardFault and produce the same visible signature.
 *
 * The handler is register-only.  The HAL tick (and therefore any
 * `HAL_Delay` / `HAL_GetTick`) cannot run at HardFault priority, so a
 * busy-wait is used; its absolute duration is not load-bearing (the
 * *shape* of the pattern is the diagnostic).
 *
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  STM32F767ZIT6
 * @srs     BGEN-006
 */

#include "stm32f7xx_hal.h"
#include "boot_board.h"

/** @brief HardFault handler, defined in this file and linked from the
 *         vector table. */
void HardFault_Handler(void);

/**
 * @brief   Roughly calibrated busy-wait (why a busy-wait: the HAL tick
 *          cannot run at HardFault priority).
 *
 * @param[in] ms Nominal delay in milliseconds; actual duration scales
 *               with @ref SystemCoreClock and is not exact.
 */
static void fault_delay_ms(uint32_t ms)
{
    volatile uint32_t n = (SystemCoreClock / 4000U) * ms;

    while (n-- > 0U) {
        __NOP();
    }
}

/**
 * @brief   Set or clear LD3 with a single atomic BSRR write.
 *
 * @param[in] on  Non-zero to turn the LED on, zero to turn it off.
 */
static void fault_led3(int on)
{
    BOOT_LED3_PORT->BSRR = on ? BOOT_LED3_PIN : ((uint32_t)BOOT_LED3_PIN << 16U);
}

/**
 * @brief   HardFault handler; see the file header for the full pattern.
 *
 * @details Does not return (ends in a system reset).  Intentionally
 *          independent of any prior HAL initialisation state so it can
 *          be entered even very early in the boot path.
 *
 * @srs     BGEN-006
 */
void HardFault_Handler(void)
{
    /* LD3 as output, even if the fault happened before GPIO set-up. */
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOBEN;
    __DSB();
    BOOT_LED3_PORT->MODER = (BOOT_LED3_PORT->MODER & ~(3UL << (14U * 2U))) |
                            (1UL << (14U * 2U));

    /* Fault pattern: three short flashes, pause (about 1 s per cycle). */
    for (uint32_t cycle = 0U; cycle < 5U; cycle++) {
        for (uint32_t i = 0U; i < 3U; i++) {
            fault_led3(1);
            fault_delay_ms(100U);
            fault_led3(0);
            fault_delay_ms(100U);
        }
        fault_delay_ms(400U);
    }

    NVIC_SystemReset();
}
