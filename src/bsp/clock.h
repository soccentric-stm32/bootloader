/**
 * @file    clock.h
 * @brief   216 MHz clock tree from the ST-LINK MCO.
 *
 * @details
 * Provides two operations:
 *
 * - **Setup** (@ref boot_clock_init) - configure the PLL from the 8 MHz
 *   external reference (the ST-LINK MCO running in bypass mode) up to a
 *   216 MHz SYSCLK / HCLK with the split bus prescalers listed below.
 * - **Teardown** (@ref boot_clock_deinit) - restore the clock tree to a
 *   reset-like state (HSI, prescalers /1, 0 flash wait states) without
 *   touching `RCC_CSR`, so the reset cause is preserved for the
 *   application (BGEN-007).
 *
 * The clock plan keeps USART3 (on PCLK1) within 0.01 % at 921600 baud,
 * which is the tolerance the update-protocol framing relies on.
 *
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  STM32F767ZIT6 rev Z, 3.3 V (voltage scale 1)
 * @srs     BGEN-003, BGEN-004, HND-001
 */

#ifndef CLOCK_H
#define CLOCK_H

#include <stdbool.h>

/**
 * @name Target clock tree.
 * @brief
 * - HSE bypass:  8 MHz (ST-LINK MCO)
 * - PLL output:  216 MHz  (SYSCLK / HCLK)
 * - APB1/PCLK1:  54 MHz   (/4)
 * - APB2/PCLK2:  108 MHz  (/2)
 * - Flash:       7 wait states, over-drive enabled
 * @{
 */

/**
 * @brief   Configure the PLL and bus clocks to the target tree above.
 *
 * @details Order of operations inside the driver:
 *
 * 1. Enable the PWR clock and request voltage scale 1 (3.3 V board).
 * 2. Start the HSE in *bypass* mode (the ST-LINK MCO already drives
 *    PH0; no internal oscillator startup needed), and configure the
 *    PLL (M=8, N=432, P=/2, Q=9).
 * 3. Enable the over-drive mode required for the 216 MHz core.
 * 4. Switch SYSCLK to the PLL and set the AHB / APB1 / APB2 prescalers
 *    (1 / 4 / 2) with 7 flash wait states; the HAL updates the
 *    latency before the switch (latency-then-frequency order).
 * 5. Enable the flash prefetch buffer and the ART accelerator for the
 *    216 MHz core.
 *
 * Any failed HAL call returns `false`.  (The SysTick handler that
 * `HAL_RCC_ClockConfig` starts is provided in this file - see the
 * note on `SysTick_Handler` at the bottom of clock.c.)
 *
 * @return  `true`  when the clock tree is set up and stable; `false`
 *          on any step that could not complete (HSE timeout, PLL did
 *          not lock, switch verification failed).  The state of the
 *          clocks on failure is unspecified - the caller (main) treats
 *          a failure as a fatal condition and enters the error loop.
 *
 * @note    Not idempotent-safe against a prior partial start; call once
 *          per reset, before any driver that assumes the target tree
 *          (UART3, IWDG timing, `HAL_GetTick`).
 *
 * @srs     BGEN-003, BGEN-004
 */
bool boot_clock_init(void);

/**
 * @brief   Restore the clock tree to a reset-like state.
 *
 * @details Used by @ref boot_handover so the application starts from a
 *          well-defined clock baseline regardless of which slot it
 *          lands in.  Specifically:
 *
 * - SYSCLK back to HSI,
 * - PLL and HSE disabled,
 * - AHB / APB1 / APB2 prescalers /1,
 * - flash wait states 0, over-drive off,
 * - voltage scale returned to 3 (safe lowest) if it was lowered for
 *   the PLL path,
 * - `RCC_CSR` intentionally /not/ cleared - BGEN-007 leaves the reset
 *   flags for the application to read and act on (see the reset-cause
 *   reporting path in the fault handler).
 *
 * @note    After a call to this function `HAL_GetTick()` is no longer
 *          backed by a running timer (SysTick is also stopped by the
 *          hand-over path), so any later call that relies on the HAL
 *          tick is a bug.
 *
 * @srs     HND-001, BGEN-007
 */
void boot_clock_deinit(void);

/** @} */

#endif /* CLOCK_H */
