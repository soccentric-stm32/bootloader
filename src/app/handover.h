/**
 * @file    handover.h
 * @brief   Checks before jump and the final hand-over of control to the
 *          application (SRS section 10).
 *
 * @details
 * The hand-over code is the last bootloader phase: it tears down every
 * piece of state the bootloader created (peripherals, clock tree,
 * interrupt controller) so that the application starts from a
 * well-defined, reset-equivalent baseline, and then transfers control
 * to the selected image slot.
 *
 * Two distinct phases are exported from this module:
 *
 * 1. **RAM-function relocation** - @ref boot_ramfunc_init copies the
 *    `.ramfunc` linker section from its flash source into DTCM and is
 *    called once after reset.  This provides a scratch home for the
 *    privileged operations of slot-B hand-over (the final flash swap
 *    plus branch), which would deadlock if executed from the bank
 *    being swapped out.
 * 2. **Hand-over** - @ref boot_handover performs the teardown, the
 *    optional bank swap, and the branch to the application vector
 *    table.  It never returns.
 *
 * The bootloader always reads the application vector table from the
 * slot's physical flash address (see @ref boot_slot_base).  For slot B
 * the final swap-and-jump runs from the DTCM-resident copy provided by
 * phase 1.
 *
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @srs     IMG-003, HND-001..HND-004, SWP-001/002
 */

#ifndef HANDOVER_H
#define HANDOVER_H

#include <stdbool.h>
#include <stdint.h>
#include "boot_layout.h"

/**
 * @brief   Copy the `.ramfunc` section from flash into DTCM.
 *
 * @details Relocates every symbol that the section contains (notably
 *          the privileged `boot_swap_and_jump` code path, which must
 *          run from DTCM rather than from a flash bank during a swap).
 *          Uses a word-at-a-time copy loop and issues a full data and
 *          an instruction synchronisation before returning, so that
 *          subsequent branches land on the DTCM-resident copy.
 *
 * @warning Must be called **exactly once** after reset, before any
 *          code in `.ramfunc` is invoked.  Calling it a second time is
 *          harmless with respect to the destination content (the same
 *          source is re-copied), but the cost is paid again.
 *
 * @note    DTCM size must accommodate the section; the linker script
 *          enforces fit at link time, so no runtime size check is
 *          performed here.
 *
 * @srs     SWP-001/SWP-002 (supporting step)
 */
void boot_ramfunc_init(void);

/**
 * @brief   Tear down bootloader state and jump to the application in
 *          the selected slot.
 *
 * @details Execution outline (details in <tt>handover.c</tt>):
 *
 * -# Abort/disable the USART3 DMA stream, the UART itself, the GPIO
 *    ports it uses, and the CRC unit (peripheral clock off).
 * -# Switch all three user LEDs off and return every GPIO pin used by
 *    the bootloader (LEDs, user button) to its reset state.
 * -# Globally disable interrupts, switch off SysTick, mask-and-clear
 *    the entire NVIC, and clear the pending ST bit in `SCB->ICSR`.
 * -# Reset the RCC clock tree back to its power-on state (HSI, PLL
 *    off, 0 wait states) and call `HAL_DeInit()` for completeness;
 *    `RCC->CSR` is intentionally **not** cleared - the application
 *    is responsible for reporting the reset cause (BGEN-007).
 * -# For slot B, perform the bank swap (SWP-001/SWP-002) from the
 *    DTCM-resident copy of `boot_swap_and_jump`; for slot A, branch
 *    directly at the application's initial-SP / reset-vector pair.
 *
 * @param[in] slot  The slot whose image is being launched:
 *           - `BOOT_SLOT_A` - jump straight to the slot-A vector table.
 *           - `BOOT_SLOT_B` - execute the bank swap first (from DTCM),
 *             then land in the slot-B image.
 *
 * @warning **This function does not return.** Callers in the
 *          bootloader treat any instruction after the call as
 *          unreachable; the `noreturn` attribute lets the optimizer
 *          assume the same.
 *
 * @pre     @ref boot_ramfunc_init has already been called (required
 *          for any slot; the cost is negligible and keeps the call
 *          convention uniform).
 * @pre     The vector table at `boot_slot_base(slot)` has already been
 *          verified (signature, CRC, anti-rollback) by the selection
 *          logic earlier in the boot chain.
 *
 * @srs     HND-001..HND-004, SWP-001/SWP-002
 */
void boot_handover(boot_slot_t slot) __attribute__((noreturn));

#endif /* HANDOVER_H */
