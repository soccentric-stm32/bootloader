/*
 * fault.c - HardFault handler (BGEN-006)
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * Target:  STM32F767ZIT6
 * SRS:     BGEN-006 - blink LD3 in the fault pattern, reset after 5 s
 *
 * MemManage, BusFault and UsageFault are not enabled separately, so they
 * escalate to HardFault. The handler uses registers only: the HAL tick
 * cannot run at HardFault priority.
 */

#include "stm32f7xx_hal.h"
#include "boot_board.h"

void HardFault_Handler(void);

/* Roughly calibrated busy wait; exact timing does not matter here. */
static void fault_delay_ms(uint32_t ms)
{
    volatile uint32_t n = (SystemCoreClock / 4000U) * ms;

    while (n-- > 0U) {
        __NOP();
    }
}

static void fault_led3(int on)
{
    BOOT_LED3_PORT->BSRR = on ? BOOT_LED3_PIN : ((uint32_t)BOOT_LED3_PIN << 16U);
}

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
