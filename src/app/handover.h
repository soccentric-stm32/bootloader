/**
 * @file    handover.h
 * @brief   Hand-over of control to the application (SRS section 10).
 * @details boot_handover() returns peripherals, NVIC and clocks to a reset
 *          baseline, then jumps to the slot.  For slot B the bank swap and
 *          jump run from a DTCM copy of `.ramfunc`, because SWP_FB remaps the
 *          bank BOOT executes from.
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
 * @details Word copy, then DSB and ISB.  The linker script guarantees fit.
 *
 * @warning Call once after reset, before any `.ramfunc` code runs.
 *
 * @srs     SWP-001/SWP-002 (supporting step)
 */
void boot_ramfunc_init(void);

/**
 * @brief   Reset BOOT's state and jump to the application in @p slot.
 * @details Order: USART3 + DMA, debug UART, LEDs off, GPIO and clock
 *          gates, IRQs off, SysTick off, NVIC cleared, clock tree reset,
 *          HAL_DeInit(), then boot_swap_and_jump().  RCC_CSR is kept
 *          (BGEN-007).  The vector table is read from the slot's physical
 *          address.
 *
 * @param[in] slot  BOOT_SLOT_A jumps directly; BOOT_SLOT_B sets SWP_FB first.
 *
 * @pre     boot_ramfunc_init() has run and @p slot was validated by the
 *          selection logic.
 * @warning Does not return.
 *
 * @srs     HND-001..HND-004, SWP-001/SWP-002
 */
void boot_handover(boot_slot_t slot) __attribute__((noreturn));

#endif /* HANDOVER_H */
