/**
 * @file    fault.c
 * @brief   HardFault handler (BGEN-006).
 *
 * @details Blinks LD3 three times (100 ms) then pauses 400 ms, five times
 *          (~5 s), then resets.  MemManage, BusFault and UsageFault are not
 *          installed, so they escalate here.  Register-only: the HAL tick
 *          cannot run at HardFault priority.
 *
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  STM32F767ZIT6
 * @srs     BGEN-006
 */

#include "stm32f7xx_hal.h"
#include "boot_board.h"

/** @brief HardFault handler; overrides the weak vector-table entry. */
void HardFault_Handler(void);

/**
 * @brief   Busy-wait for roughly @p ms milliseconds.
 * @param[in] ms Nominal delay in ms; scales with SystemCoreClock, inexact.
 */
static void fault_delay_ms(uint32_t ms)
{
    volatile uint32_t n = (SystemCoreClock / 4000U) * ms;

    while (n-- > 0U) {
        __NOP();
    }
}

/**
 * @brief   Set or clear LD3 with one atomic BSRR write.
 * @param[in] on  Non-zero for on, zero for off.
 */
static void fault_led3(int on)
{
    BOOT_LED3_PORT->BSRR = on ? BOOT_LED3_PIN : ((uint32_t)BOOT_LED3_PIN << 16U);
}

/**
 * @brief   Show the fault pattern on LD3, then reset; does not return.
 * @details Independent of HAL initialisation, so safe early in boot.
 * @srs     BGEN-006
 */
void HardFault_Handler(void)
{
    /* LD3 as output, even if the fault happened before GPIO set-up. */
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOBEN;
    __DSB();
    BOOT_LED3_PORT->MODER = (BOOT_LED3_PORT->MODER & ~(3UL << (14U * 2U))) |
                            (1UL << (14U * 2U));

    /* Three short flashes and a pause: ~1 s per cycle. */
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
