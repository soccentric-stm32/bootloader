/*
 * handover.c - application checks and hand-over (section 10)
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * Target:  STM32F767ZIT6
 * SRS:     IMG-003, HND-001, HND-002
 *
 * Slot B (SWP_FB set from a RAM routine, SWP-002), the IWDG start
 * (HND-004) and the hand-over record in RTC_BKP2R (HND-003) are added
 * with sections 6 and 7.
 */

#include "stm32f7xx_hal.h"
#include "boot_layout.h"
#include "handover.h"

bool boot_app_vectors_valid(uint32_t slot_base)
{
    const volatile uint32_t *vec = (const volatile uint32_t *)slot_base;
    uint32_t sp    = vec[0];
    uint32_t entry = vec[1];

    /* Thumb bit set, address inside the slot. */
    uint32_t entry_addr = entry & ~1UL;
    bool sp_ok    = (sp > BOOT_RAM_START) && (sp <= BOOT_RAM_END) &&
                    ((sp & 0x7UL) == 0U);
    bool entry_ok = ((entry & 1UL) != 0U) &&
                    (entry_addr >= slot_base) &&
                    (entry_addr < (slot_base + BOOT_SLOT_SIZE));

    return sp_ok && entry_ok;
}

void boot_handover(uint32_t app_base)
{
    const volatile uint32_t *vec = (const volatile uint32_t *)app_base;
    uint32_t sp    = vec[0];
    uint32_t entry = vec[1];

    __disable_irq();

    /* HND-001: SysTick off, every NVIC interrupt disabled and cleared. */
    SysTick->CTRL = 0U;
    SysTick->LOAD = 0U;
    SysTick->VAL  = 0U;
    for (uint32_t i = 0U; i < (sizeof(NVIC->ICER) / sizeof(NVIC->ICER[0])); i++) {
        NVIC->ICER[i] = 0xFFFFFFFFUL;
        NVIC->ICPR[i] = 0xFFFFFFFFUL;
    }
    SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk;

    /* Clock tree back to reset (HSI, PLL off, 0 WS) and every peripheral
     * reset, which also turns the LEDs off (BGEN-008). */
    (void)HAL_RCC_DeInit();
    (void)HAL_DeInit();

    /* HND-002 */
    SCB->VTOR = app_base;
    __DSB();
    __ISB();

    /* Nothing is pending any more; the application starts with interrupts
     * enabled, as after a reset. */
    __enable_irq();

    __asm volatile(
        "msr msp, %0 \n"
        "bx  %1      \n"
        :
        : "r"(sp), "r"(entry)
        : "memory");

    while (1) {
        /* not reached */
    }
}
