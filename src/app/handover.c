/**
 * @file    handover.c
 * @brief   Pre-jump checks and hand-over of control to the application
 *          (SRS section 10).
 *
 * @details
 * This module owns the last two bootloader phases:
 *
 * 1. **`.ramfunc` relocation** (@ref boot_ramfunc_init) - copies the
 *    `.ramfunc` linker section from flash into DTCM right after reset.
 *    The DTCM copy exists because the slot-B swap must be invoked from a
 *    memory region that is *not* remapped by the swap itself.
 * 2. **Hand-over proper** (@ref boot_handover) - tears down peripheral,
 *    interrupt and clock state, then either jumps to slot A or performs
 *    the A/B bank swap and jumps to slot B.  Never returns.
 *
 * Design rule: *BOOT always reads the application vector table from the
 * slot's physical address* (`boot_slot_base(slot)`), so the vector table
 * is read once at the moment of hand-over and both the initial SP and
 * the reset-vector entry are then passed to the jump helper as plain
 * values.  For slot B, the final swap-and-jump execute from the
 * DTCM-resident `.ramfunc` copy, because SWP_FB remaps the flash bank
 * the bootloader itself is executing from - executing from that bank
 * during the swap would corrupt the running code.
 *
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
#include "uart3.h"

/**
 * @brief   Slot-B privileged helper: perform the bank swap, then branch
 *          to the application vector table.
 *
 * @details Defined (out-of-band, typically a small asm stub) to satisfy
 *          the SWP-001/SWP-002 sequence.  Lives in the `.ramfunc` linker
 *          section so a DTCM-resident copy is guaranteed to be present
 *          at the moment it is invoked from @ref boot_handover.
 *
 * @param[in] slot_b Flag telling the helper whether the target is
 *                   slot B (1) or slot A (0); slot B enables the
 *                   bank-swap register write before the branch.
 * @param[in] sp     Initial stack pointer to load into `MSP` before the
 *                   branch (taken from word 0 of the target vector
 *                   table).
 * @param[in] entry  Reset-vector entry of the target image (word 1 of
 *                   the vector table).
 *
 * @warning Does not return.
 */
extern void boot_swap_and_jump(uint32_t slot_b, uint32_t sp, uint32_t entry)
    __attribute__((noreturn));

/**
 * @name RAM-function section linker symbols.
 * @brief Boundaries of the `.ramfunc` linker section used by the copy
 *        loop in @ref boot_ramfunc_init.
 * @{
 */

/** Start of the `.ramfunc` section's copy *destination* in DTCM. */
extern uint32_t _sramfunc[];

/** End (one past) of the `.ramfunc` copy destination in DTCM. */
extern uint32_t _eramfunc[];

/** Start of the `.ramfunc` section's *source* in flash (LMA). */
extern uint32_t _siramfunc[];

/** @} */

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
 * @brief   Switch all three user LEDs off via the reset-state BSRR mask.
 *
 * @details Direct register write to `GPIOx->BSRR` with the pin bits in
 *          the high half, which clears them to LOW.  This runs before
 *          the GPIO clocks are gated, so the port registers are still
 *          live.  Using BSRR (instead of ODR) keeps the operation
 *          atomic with respect to any concurrent read-modify-write -
 *          there are no other writers here, but the pattern is the
 *          safe, interrupt-race-free one on STM32.
 *
 * @srs     BGEN-008
 */
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
