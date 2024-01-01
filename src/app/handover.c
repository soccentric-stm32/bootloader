/**
 * @file    handover.c
 * @brief   Hand-over of control to the application (SRS section 10).
 * @details The vector table is read once from the slot's physical address;
 *          SP and entry are passed to boot_swap_and_jump() as values.
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  STM32F767ZIT6
 * @srs     HND-001..HND-004, SWP-001/SWP-002
 */

#include "stm32f7xx_hal.h"
#include "boot_board.h"
#include "boot_layout.h"
#include "clock.h"
#include "handover.h"
#include "debug_uart.h"
#include "uart3.h"

/**
 * @brief   Optionally set SWP_FB, load MSP and branch to the application.
 * @details Assembly in swap_jump.S, placed in `.ramfunc` so it runs from
 *          DTCM during the swap.
 *
 * @param[in] slot_b 1 to swap banks first (slot B), 0 for slot A.
 * @param[in] sp     Initial MSP (vector word 0).
 * @param[in] entry  Reset handler (vector word 1).
 *
 * @warning Does not return.
 */
extern void boot_swap_and_jump(uint32_t slot_b, uint32_t sp, uint32_t entry)
    __attribute__((noreturn));

/**
 * @name `.ramfunc` linker symbols
 * @{
 */

/** Start of the DTCM copy (VMA). */
extern uint32_t _sramfunc[];

/** One past the end of the DTCM copy. */
extern uint32_t _eramfunc[];

/** Start of the flash source (LMA). */
extern uint32_t _siramfunc[];

/** @} */

/** @brief Copy `.ramfunc` to DTCM; see @ref boot_ramfunc_init. */
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

/**
 * @brief   Switch LD1-LD3 off with one BSRR write.
 * @details Assumes all three LEDs share @ref BOOT_LED1_PORT.  GPIOB clock
 *          must still be on.
 * @srs     BGEN-008
 */
static void leds_off(void)
{
    uint32_t mask = BOOT_LED1_PIN | BOOT_LED2_PIN | BOOT_LED3_PIN;
    BOOT_LED1_PORT->BSRR = mask << 16U;
}

/** @brief Reset state and jump; see @ref boot_handover. */
void boot_handover(boot_slot_t slot)
{
    uint32_t phys_base = boot_slot_base(slot);
    const volatile uint32_t *vec = (const volatile uint32_t *)phys_base;
    uint32_t sp    = vec[0];
    uint32_t entry = vec[1];

    /* HND-001: before SysTick stops, since HAL_DMA_Abort times out on
     * HAL_GetTick. */
    NVIC_DisableIRQ(DMA1_Stream1_IRQn);
    boot_uart3_deinit();
    /* BDBG-001: drain and release USART6. */
    boot_dbg_deinit();

    /* BGEN-008: all LEDs off at hand-over (GPIO clocks still on). */
    leds_off();

    /* HND-001: BOOT's GPIO pins back to reset state. */
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

    /* SWP-001/SWP-002: from DTCM; never returns. */
    boot_swap_and_jump(slot == BOOT_SLOT_B ? 1U : 0U, sp, entry);
}
