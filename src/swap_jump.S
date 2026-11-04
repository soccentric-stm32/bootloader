/*
 * swap_jump.S - bank-swap and jump routine, executed from DTCM
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * Target:  STM32F767ZIT6
 * SRS:     SWP-001, SWP-002, HND-002
 *
 * void boot_swap_and_jump(uint32_t slot_b, uint32_t sp, uint32_t entry)
 *
 * The function is linked into .ramfunc and copied to DTCM at start-up,
 * because setting SYSCFG_MEMRMP.SWP_FB remaps the bank BOOT itself runs
 * from.  No literal pool is used: every constant is built with MOVW/MOVT.
 */

    .syntax unified
    .thumb
    .section .ramfunc, "ax", %progbits
    .global boot_swap_and_jump
    .type boot_swap_and_jump, %function
    .thumb_func
boot_swap_and_jump:
    /* r0 = slot_b, r1 = initial SP, r2 = reset vector */
    cbz     r0, 1f

    /* Enable the SYSCFG interface clock (RCC_APB2ENR.SYSCFGEN). */
    movw    r3, #0x3844
    movt    r3, #0x4002
    ldr     r4, [r3]
    orr     r4, r4, #0x4000
    str     r4, [r3]
    dsb

    /* SYSCFG_MEMRMP.SWP_FB = 1: bank 2 at 0x08000000. */
    movw    r3, #0x3800
    movt    r3, #0x4001
    ldr     r4, [r3]
    orr     r4, r4, #0x100
    str     r4, [r3]
    dsb
    isb

1:
    /* HND-001 / SWP-002: leave prefetch and the ART accelerator off,
     * flush the ART accelerator (ARTRST) and invalidate the instruction
     * cache (SCB->ICIALLU). */
    movw    r3, #0x3C00
    movt    r3, #0x4002
    ldr     r4, [r3]
    bic     r4, r4, #0x300          /* PRFTEN | ARTEN */
    str     r4, [r3]
    orr     r4, r4, #0x800          /* ARTRST = 1 */
    str     r4, [r3]
    dsb
    bic     r4, r4, #0x800          /* ARTRST = 0 */
    str     r4, [r3]
    dsb
    isb

    movw    r3, #0xEF50
    movt    r3, #0xE000
    movs    r4, #0
    str     r4, [r3]
    dsb
    isb

    /* HND-002: vector table at the slot alias, then MSP and reset vector. */
    movw    r3, #0xED08
    movt    r3, #0xE000
    movw    r4, #0xC000
    movt    r4, #0x0800
    str     r4, [r3]
    dsb
    isb

    msr     msp, r1
    dsb
    isb

    /* The application starts with interrupts enabled, as after reset.
     * SysTick is off and the NVIC was cleared by BOOT. */
    cpsie   i
    dsb
    isb
    bx      r2

    .size boot_swap_and_jump, . - boot_swap_and_jump
