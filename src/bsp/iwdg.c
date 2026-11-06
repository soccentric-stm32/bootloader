/**
 * @file    iwdg.c
 * @brief   Independent watchdog for the trial boot.
 *
 * @details
 * - LSI is nominally 32 kHz; prescaler /64 and reload 1999 give a
 *   4.0 s timeout (TRY-004).  LSI is /not/ on the main clock tree, so
 *   the watchdog keeps running across the clock teardown of
 *   @ref boot_clock_deinit.
 * - The watchdog is started once, immediately before the hand-over, so
 *   a hung application resets and counts as a failed trial on the next
 *   boot (TRY-004 / HND-004).  BOOT itself never kicks it - it does
 *   not return from the hand-over.
 * - All waits on this file are bounded (see @ref IWDG_WAIT_LOOPS): a
 *   dead LSI must not hang the boot, and the PVU/RVU flags must be
 *   allowed to clear between register updates.
 *
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  STM32F767ZIT6
 * @srs     TRY-004 (4 s build-time timeout), HND-004
 */

#include "stm32f7xx_hal.h"
#include "iwdg.h"

/**
 * @name IWDG key registers and timeout constants.
 * @brief Magic keys for the control register and the 4 s timeout setup.
 * @{
 */

/** Key that unlocks the prescaler and reload registers. */
#define IWDG_KEY_ACCESS   0x00005555UL

/** Key that performs a counter reload (a "kick"). */
#define IWDG_KEY_RELOAD   0x0000AAAAUL

/** Key that enables the watchdog (irreversible until reset). */
#define IWDG_KEY_ENABLE   0x0000CCCCUL

/** Reload counter that, with /64 prescaler at 32 kHz LSI, gives ~4.0 s. */
#define IWDG_RELOAD_4S    1999UL

/** Prescaler encoding for LSI / 64 (500 Hz counter input). */
#define IWDG_PR_DIV64     4U      /* PR encoding for LSI / 64 */

/**
 * @brief  Bounded wait loop count (about 40 ms at 216 MHz).
 *
 * @details Generous for the handful of LSI cycles (about 160 us) an
 *          update actually needs, but short enough that a dead LSI
 *          cannot hang the boot forever.
 */
#define IWDG_WAIT_LOOPS   1000000UL

/** @} */

/**
 * @brief   Start the LSI and wait (with a bound) for it to be ready.
 *
 * @details Without LSI running the PVU/RVU flags in @c IWDG->SR never
 *          clear and the following register updates cannot complete -
 *          this bounded wait is the guard against that.  If the LSI
 *          has not started by the time the bound is reached, the boot
 *          continues and the watchdog will simply be in its start-pending
 *          state at hand-over (better than an infinite lockup).
 */
static void iwdg_start_lsi(void)
{
    RCC->CSR |= RCC_CSR_LSION;
    for (uint32_t i = 0U; i < IWDG_WAIT_LOOPS; i++) {
        if ((RCC->CSR & RCC_CSR_LSIRDY) != 0U) {
            return;
        }
    }
}

/**
 * @brief   Wait (with a bound) for the pending-update flags in
 *          @c IWDG->SR to clear.
 *
 * @details The IWDG latches a "programmed value update" or
 *          "reload value update" pending flag after a write to
 *          @c PR / @c RLR; these must be read clear before the next
 *          write is meaningful.  The wait is bounded; on timeout the
 *          caller proceeds (a stuck flag does not change the timeout
 *          significantly enough to matter for the boot contract).
 */
static void iwdg_wait_ready(void)
{
    for (uint32_t i = 0U; i < IWDG_WAIT_LOOPS; i++) {
        if ((IWDG->SR & (IWDG_SR_PVU | IWDG_SR_RVU | IWDG_SR_WVU)) == 0U) {
            return;
        }
    }
}

/**
 * @brief   Reload the running watchdog (see @ref boot_iwdg_kick).
 *
 * @details Single register write; safe to call before @ref
 *          boot_iwdg_start (it is a no-op until the watchdog is
 *          running).
 */
void boot_iwdg_kick(void)
{
    IWDG->KR = IWDG_KEY_RELOAD;
}

/**
 * @brief   Start the IWDG with the 4 s timeout (see @ref
 *          boot_iwdg_start).
 *
 * @details Sequence is exactly the HAL's documented order:
 *          LSI on → enable → unlock (KR=0x5555) → program PR/RLR →
 *          wait for the update flags to clear → reload, so that the
 *          first count-down begins from the full 4 s window.
 */
void boot_iwdg_start(void)
{
    /* LSI first, so the prescaler/reload updates can complete. */
    iwdg_start_lsi();
    /* ST's sequence (HAL_IWDG_Init): enable the watchdog, unlock PR/RLR,
     * program them, wait for the update to land, then reload. */
    IWDG->KR = IWDG_KEY_ENABLE;
    IWDG->KR = IWDG_KEY_ACCESS;
    IWDG->PR = IWDG_PR_DIV64;
    IWDG->RLR = IWDG_RELOAD_4S;
    iwdg_wait_ready();
    IWDG->KR = IWDG_KEY_RELOAD;
}
