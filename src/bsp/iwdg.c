/**
 * @file    iwdg.c
 * @brief   Independent watchdog for the trial boot.
 * @details LSI (~32 kHz) / 64 with reload 1999 gives ~4.0 s (TRY-004).  All
 *          waits are bounded by @ref IWDG_WAIT_LOOPS so a dead LSI cannot
 *          hang the boot.
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  STM32F767ZIT6
 * @srs     TRY-004 (4 s build-time timeout), HND-004
 */

#include "stm32f7xx_hal.h"
#include "iwdg.h"

/**
 * @name IWDG keys and timeout
 * @{
 */

/** Key that unlocks the prescaler and reload registers. */
#define IWDG_KEY_ACCESS   0x00005555UL

/** Key that reloads the counter. */
#define IWDG_KEY_RELOAD   0x0000AAAAUL

/** Key that starts the watchdog; irreversible until reset. */
#define IWDG_KEY_ENABLE   0x0000CCCCUL

/** Reload value: 2000 ticks at 500 Hz = ~4.0 s. */
#define IWDG_RELOAD_4S    1999UL

/** PR encoding for LSI / 64 (500 Hz counter clock). */
#define IWDG_PR_DIV64     4U

/**
 * @brief   Poll limit for LSI and register-update waits (~40 ms at 216 MHz).
 * @details An update needs only ~160 us of LSI cycles.
 */
#define IWDG_WAIT_LOOPS   1000000UL

/** @} */

/**
 * @brief   Turn on the LSI and wait, bounded, for LSIRDY.
 * @details Returns silently on timeout; without LSI the PR/RLR updates
 *          never complete, but the boot does not lock up.
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
 * @brief   Wait, bounded, for the PVU, RVU and WVU update flags to clear.
 * @details Returns silently on timeout.
 */
static void iwdg_wait_ready(void)
{
    for (uint32_t i = 0U; i < IWDG_WAIT_LOOPS; i++) {
        if ((IWDG->SR & (IWDG_SR_PVU | IWDG_SR_RVU | IWDG_SR_WVU)) == 0U) {
            return;
        }
    }
}

/** @brief Reload the watchdog counter; see @ref boot_iwdg_kick. */
void boot_iwdg_kick(void)
{
    IWDG->KR = IWDG_KEY_RELOAD;
}

/**
 * @brief   Start the IWDG with a 4 s timeout; see @ref boot_iwdg_start.
 * @details Follows HAL_IWDG_Init(): enable, unlock, program PR/RLR, wait for
 *          the update, reload.
 */
void boot_iwdg_start(void)
{
    /* LSI first, so the PR/RLR updates can complete. */
    iwdg_start_lsi();
    IWDG->KR = IWDG_KEY_ENABLE;
    IWDG->KR = IWDG_KEY_ACCESS;
    IWDG->PR = IWDG_PR_DIV64;
    IWDG->RLR = IWDG_RELOAD_4S;
    iwdg_wait_ready();
    IWDG->KR = IWDG_KEY_RELOAD;
}
