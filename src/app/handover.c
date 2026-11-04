/*
 * handover.c - application checks and hand-over (section 10)
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * Target:  STM32F767ZIT6
 * SRS:     HND-001..HND-004, SWP-001/SWP-002
 *
 * BOOT always reads the application vector table from the slot's physical
 * address.  For slot B the final swap and jump run from the .ramfunc copy in
 * DTCM because SWP_FB remaps the bank BOOT executes from.
 */

#include "stm32f7xx_hal.h"
#include "boot_board.h"
#include "boot_layout.h"
#include "clock.h"
#include "handover.h"
#include "uart3.h"

extern void boot_swap_and_jump(uint32_t slot_b, uint32_t sp, uint32_t entry)
    __attribute__((noreturn));

extern uint32_t _sramfunc[];
extern uint32_t _eramfunc[];
extern uint32_t _siramfunc[];

void boot_ramfunc_init(void)
{
    uint32_t *dst = _sramfunc;
    const uint32_t *src = _siramfunc;

    while (dst < _eramfunc) {
        *dst++ = *src++;
    }
    __DSB();
    __ISB();
}

static void leds_off(void)
{
    uint32_t mask = BOOT_LED1_PIN | BOOT_LED2_PIN | BOOT_LED3_PIN;
    BOOT_LED1_PORT->BSRR = mask << 16U;
}

void boot_handover(boot_slot_t slot)
{
    uint32_t phys_base = boot_slot_base(slot);
    const volatile uint32_t *vec = (const volatile uint32_t *)phys_base;
    uint32_t sp    = vec[0];
    uint32_t entry = vec[1];

    /* HND-001: USART3, its DMA, the GPIO ports and the CRC unit.  This is
     * done before SysTick is stopped because HAL_DMA_Abort uses HAL_GetTick
     * for its timeout. */
    NVIC_DisableIRQ(DMA1_Stream1_IRQn);
    boot_uart3_deinit();

    /* BGEN-008: all LEDs off at hand-over (GPIO clocks still on). */
    leds_off();

    /* HND-001: return the GPIO pins used by BOOT to their reset state. */
    HAL_GPIO_DeInit(BOOT_LED1_PORT,
                    BOOT_LED1_PIN | BOOT_LED2_PIN | BOOT_LED3_PIN);
    HAL_GPIO_DeInit(BOOT_BTN1_PORT, BOOT_BTN1_PIN);

    __HAL_RCC_CRC_CLK_DISABLE();
    __HAL_RCC_GPIOB_CLK_DISABLE();
    __HAL_RCC_GPIOC_CLK_DISABLE();
    __HAL_RCC_GPIOD_CLK_DISABLE();

    __disable_irq();

    /* HND-001: SysTick off, every NVIC interrupt disabled and cleared. */
    SysTick->CTRL = 0U;
    SysTick->LOAD = 0U;
    SysTick->VAL  = 0U;
    for (uint32_t i = 0U; i < (sizeof(NVIC->ICER) / sizeof(NVIC->ICER[0]));
         i++) {
        NVIC->ICER[i] = 0xFFFFFFFFUL;
        NVIC->ICPR[i] = 0xFFFFFFFFUL;
    }
    SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk;

    /* HND-001: clock tree back to reset (HSI, PLL off, 0 WS).  BGEN-007:
     * never clear RCC_CSR, the application reports the reset cause. */
    boot_clock_deinit();
    (void)HAL_DeInit();

    /* SWP-001/SWP-002: swap for slot B and enter the application from
     * DTCM.  Never returns. */
    boot_swap_and_jump(slot == BOOT_SLOT_B ? 1U : 0U, sp, entry);
}
